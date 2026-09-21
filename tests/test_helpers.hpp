#pragma once
#include "test_server.hpp"
#include <crails/context.hpp>
#include <crails/http_response.hpp>
#include <crails/params.hpp>
#include <crails/request_handlers/action.hpp>
#include <crails/router.hpp>
#include <crails/session_store.hpp>
#include <crails/session_store/no_session_store.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Harness
{
  using namespace std::chrono_literals;

  struct MemorySessionStore : public Crails::SessionStore
  {
    SESSION_STORE_IMPLEMENTATION(MemorySessionStore)
  public:
    static inline DataTree               cookie;
    static inline int                    loads = 0, finalizes = 0;
    static inline std::function<void()>  on_finalize;

    static void reset() { cookie = DataTree(); loads = finalizes = 0; on_finalize = nullptr; }

    void load(const Crails::HttpRequest&) override { ++loads; }
    void finalize(Crails::BuildingResponse&) override { ++finalizes; if (on_finalize) on_finalize(); }
    Data       to_data() override { return cookie.as_data(); }
    const Data to_data() const override { return cookie.as_data(); }
  };

  struct TestRequest
  {
    Crails::HttpVerb                  method = Crails::HttpVerb::get;
    std::string                       target = "/";
    std::map<std::string,std::string> headers;
    std::map<std::string,std::string> params;
  };

  struct PresetParser : public Crails::RequestParser
  {
    static inline const TestRequest* current = nullptr;

    void operator()(Crails::Context& context, std::function<void(Crails::RequestParser::Status)> callback) const override
    {
      if (current)
      {
        for (const auto& [name, value] : current->headers)
          context.params["headers"][name] = value;
        for (const auto& [name, value] : current->params)
          context.params[name] = value;
      }
      callback(Crails::RequestParser::Continue);
    }
  };

  struct Response
  {
    unsigned short                    status = 0;
    std::string                       body;
    std::map<std::string,std::string> headers;

    bool        has_header(const std::string& name) const { return headers.count(lowercase(name)) > 0; }
    std::string header(const std::string& name) const { auto it = headers.find(lowercase(name)); return it == headers.end() ? std::string() : it->second; }
    static std::string lowercase(std::string value) { for (auto& c : value) c = std::tolower(c); return value; }
  };

  struct Pending
  {
    std::shared_ptr<Crails::Connection> connection;
    std::shared_ptr<TestContext>        context;
    std::optional<Response>             captured;
    Crails::Context&                    get_context() { return *context; }
  };

  // Runs the io_context on this thread until the condition is met (or the time is up: then the condition is what the test asserts)
  inline bool pump_until(std::function<bool()> condition, std::chrono::milliseconds limit = 5000ms)
  {
    auto&      io       = Crails::Server::get_io_context();
    const auto deadline = std::chrono::steady_clock::now() + limit;

    io.restart();
    while (!condition() && std::chrono::steady_clock::now() < deadline)
      io.run_one_for(5ms);
    return condition();
  }

  // Lets what is still queued run (the destruction of the controllers, the writes of the connections...)
  inline void drain()
  {
    auto& io = Crails::Server::get_io_context();

    io.restart();
    for (int i = 0 ; i < 20 && io.poll() > 0 ; ++i) {}
  }

  inline Pending start(TestServer& server, const TestRequest& description)
  {
    Crails::HttpRequest request;
    Pending             pending;

    request.method(description.method);
    request.target(description.target);
    for (const auto& [name, value] : description.headers)
      request.set(name, value);
    pending.connection = std::make_shared<Crails::Connection>(server, request);
    pending.context    = std::make_shared<TestContext>(server, *pending.connection);
    PresetParser::current = &description;
    pending.context->test_run();               // the parsers run now: they are done with the description when it returns
    PresetParser::current = nullptr;
    return pending;
  }

  // Reads the response if the request is over. It has to be done at once: the connection, that has no socket here, recycles its
  // response when its write fails, which happens as soon as the io_context runs the next handler.
  inline bool capture(Pending& pending)
  {
    auto future = pending.context->get_future();

    if (pending.captured)
      return true;
    if (future.wait_for(0s) != std::future_status::ready)
      return false;
    Response result;
    auto&    response = pending.connection->get_response();

    result.status = future.get();
    result.body   = response.body();
    for (const auto& field : response)
      result.headers[Response::lowercase(std::string(field.name_string()))] = std::string(field.value());
    pending.captured = result;
    return true;
  }

  // Waits for the end of the requests, and reads their responses
  inline std::vector<Response> finish_all(std::vector<Pending>& pendings, std::chrono::milliseconds limit = 10000ms)
  {
    auto&      io       = Crails::Server::get_io_context();
    const auto deadline = std::chrono::steady_clock::now() + limit;
    auto       all_over = [&]() { bool over = true; for (auto& pending : pendings) over = capture(pending) && over; return over; };

    io.restart();
    while (!all_over() && std::chrono::steady_clock::now() < deadline)
      io.run_one_for(5ms);
    std::vector<Response> responses;
    for (auto& pending : pendings)
      responses.push_back(pending.captured ? *pending.captured : Response());
    return responses;
  }

  inline Response finish(Pending& pending)
  {
    std::vector<Pending> one{pending};
    auto                 responses = finish_all(one);

    pending.captured = one[0].captured;
    return responses[0];
  }

  inline Response perform(TestServer& server, const TestRequest& description)
  {
    Pending  pending  = start(server, description);
    Response response = finish(pending);

    drain();
    return response;
  }

  inline Response get(TestServer& server, const std::string& target, std::map<std::string,std::string> headers = {}, std::map<std::string,std::string> params = {})
  {
    TestRequest request;

    request.target = target;
    request.headers = headers;
    request.params = params;
    return perform(server, request);
  }

  inline Response post(TestServer& server, const std::string& target, std::map<std::string,std::string> params = {}, std::map<std::string,std::string> headers = {})
  {
    TestRequest request;

    request.method = Crails::HttpVerb::post;
    request.target = target;
    request.headers = headers;
    request.params = params;
    return perform(server, request);
  }

  // Counts the controllers that are alive: to check they are all destroyed, and none too early
  struct Counted
  {
    static inline std::atomic<int> alive{0};
    Counted()  { ++alive; }
    ~Counted() { --alive; }
  };

  // What happened, in which order
  struct Trace
  {
    static inline std::vector<std::string> events;
    static void add(const std::string& event) { events.push_back(event); }
    static void clear() { events.clear(); }
    static bool contains(const std::string& event) { return std::find(events.begin(), events.end(), event) != events.end(); }
    static int  position(const std::string& event) { return std::find(events.begin(), events.end(), event) - events.begin(); }
  };

  // Sets up what every test needs: the server, the router, the session store, the parser and the request handler
  struct Setup
  {
    SingletonInstantiator<TestServer>                    server;
    SingletonInstantiator<MemorySessionStore::Factory>   store;
    SingletonInstantiator<Crails::Router>                router;

    Setup()
    {
      server->test_setup(
        std::vector<Crails::RequestHandler*>{new Crails::ActionRequestHandler()},
        std::vector<Crails::RequestParser*>{new PresetParser()}
      );
      MemorySessionStore::reset();
      Trace::clear();
    }
  };
}
