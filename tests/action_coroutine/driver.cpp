#include <crails/environment.hpp>
#include <crails/controller.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "../test_helpers.hpp"

#undef NDEBUG
#include <cassert>

using namespace Crails;
using namespace Harness;
using namespace std;
using boost::asio::awaitable;

static const Crails::Connection::Strand* expected_strand = nullptr;

static void trace(const string& what)
{
  bool on_strand = !expected_strand || expected_strand->running_in_this_thread();
  Trace::add(what + (on_strand ? "" : " (NOT ON THE STRAND)"));
}

static awaitable<void> pause(chrono::milliseconds duration)
{
  boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);

  timer.expires_after(duration);
  co_await timer.async_wait(boost::asio::use_awaitable);
}

// A controller that says everything it does
struct Traced : public Crails::Controller
{
  typedef Crails::Controller Super;
  Counted counted;
  Traced(Crails::Context& context) : Super(context) {}

  static inline atomic<int>        finalize_count{0};
  static inline atomic<HttpStatus> status_at_finalize{HttpStatus::ok};

  bool must_protect_from_forgery() const override { return false; }

  void initialize() override { trace("initialize"); Super::initialize(); }
  awaitable<void> co_initialize() override
  {
    trace("co_initialize:begin");
    co_await Super::co_initialize();
    co_await pause(5ms);
    trace("co_initialize:end");
  }
  void finalize() override
  {
    ++finalize_count;
    status_at_finalize = response.get_status_code();
    trace("finalize");
    Super::finalize();
  }
  awaitable<void> co_finalize() override
  {
    trace("co_finalize:begin");
    co_await Super::co_finalize();
    co_await pause(5ms);
    response.set_header("X-Finalized", "yes");             // the response can still be changed
    session["audit"] = "written by co_finalize";           // and so can the session: it is saved after
    trace("co_finalize:end");
  }
};

struct Actions : public Traced
{
  Actions(Crails::Context& context) : Traced(context) {}

  void plain() { trace("action"); render(TEXT, string_view("plain")); trace("action:after render"); }
  awaitable<void> coroutine()
  {
    trace("action:begin");
    co_await pause(5ms);
    render(TEXT, get_controller_name() + "::" + get_action_name() + '/' + params["id"].defaults_to<string>("-"));
    trace("action:after render");
  }
  void forgets() { trace("action"); }
  awaitable<void> forgets_async() { trace("action:begin"); co_await pause(5ms); trace("action:end"); }
  void throws() { trace("action"); throw std::runtime_error("boom"); }
  // starts a task that fails, after the action has returned normally
  void spawns_failing_task()
  {
    trace("action");
    co_spawn([]() -> awaitable<void> { co_await pause(10ms); throw std::runtime_error("the task failed"); });
  }

  // starts a task that would answer later, and throws before it does
  void spawns_then_throws()
  {
    trace("action");
    co_spawn([this]() -> awaitable<void> { co_await pause(20ms); trace("task"); render(TEXT, string_view("late")); });
    throw std::runtime_error("boom");
  }
  awaitable<void> throws_async() { trace("action:begin"); co_await pause(5ms); throw std::runtime_error("boom"); }
  void spawns()
  {
    trace("action");
    co_spawn([this]() -> awaitable<void> { co_await pause(20ms); trace("task"); render(TEXT, string_view("task")); });
  }
  awaitable<void> spawns_async()
  {
    trace("action:begin");
    co_spawn([this]() -> awaitable<void> { co_await pause(20ms); trace("task"); render(TEXT, string_view("task")); });
    co_return ;
  }
};

struct Guarded : public Traced
{
  Guarded(Crails::Context& context) : Traced(context) {}
  void initialize() override { Traced::initialize(); if (params["sync-deny"].exists()) respond_with(HttpStatus::unauthorized); }
  awaitable<void> co_initialize() override
  {
    co_await Traced::co_initialize();
    if (params["deny"].exists())  respond_with(HttpStatus::forbidden);
    if (params["fail"].exists())  throw std::runtime_error("initializer failed");
  }
  void action() { trace("action"); render(TEXT, string_view("allowed")); }
};

struct FinalizeFails : public Traced
{
  FinalizeFails(Crails::Context& context) : Traced(context) {}
  void finalize() override { Traced::finalize(); if (params["sync"].exists()) throw std::runtime_error("finalize failed"); }
  awaitable<void> co_finalize() override { co_await Traced::co_finalize(); if (params["async"].exists()) throw std::runtime_error("co_finalize failed"); }
  void action() { trace("action"); render(TEXT, string_view("fine")); }
};

struct Layer1 : public Actions
{
  Layer1(Crails::Context& context) : Actions(context) {}
  awaitable<void> co_initialize() override { co_await Actions::co_initialize(); trace("layer1"); }
  awaitable<void> co_finalize()   override { co_await Actions::co_finalize();   trace("layer1:finalize"); }
};
struct Layer2 : public Layer1
{
  Layer2(Crails::Context& context) : Layer1(context) {}
  awaitable<void> co_initialize() override { co_await Layer1::co_initialize(); trace("layer2"); }
  awaitable<void> co_finalize()   override { co_await Layer1::co_finalize();   trace("layer2:finalize"); }
  void layered() { Actions::plain(); }
};

// The methods that ActionRoute calls are protected here, as they often are in applications: it has to reach them through the base classes
struct Hidden : public Crails::Controller
{
  Counted counted;
  Hidden(Crails::Context& context) : Crails::Controller(context) {}
  bool must_protect_from_forgery() const override { return false; }
  void plain() { trace("hidden:action"); render(TEXT, string_view("hidden")); }
protected:
  void initialize() override { trace("hidden:initialize"); Crails::Controller::initialize(); }
  awaitable<void> co_initialize() override { trace("hidden:co_initialize"); co_return ; }
  void finalize() override { trace("hidden:finalize"); Crails::Controller::finalize(); }
  awaitable<void> co_finalize() override { trace("hidden:co_finalize"); co_return ; }
  boost::asio::any_io_executor get_io_executor() override { return Crails::Controller::get_io_executor(); }
};

static Response run(TestServer& server, const string& target, map<string,string> params = {})
{
  TestRequest request;

  request.target = target;
  request.params = params;
  Trace::clear();
  Traced::finalize_count = 0;
  MemorySessionStore::reset();
  MemorySessionStore::on_finalize = []() { trace("session:finalize"); };
  Pending pending = start(server, request);
  expected_strand = &pending.connection->get_strand();
  Response response = finish(pending);
  drain();
  expected_strand = nullptr;
  return response;
}

static bool in_order(const vector<string>& events)
{
  int last = -1;
  for (const auto& event : events)
  {
    int position = Trace::position(event);
    if (position >= static_cast<int>(Trace::events.size()) || position <= last) return false;
    last = position;
  }
  return true;
}

int main()
{
  Crails::environment = Crails::Test;
  Setup env;
  auto& server = *env.server;
  auto& router = *env.router;

  router.match_action("GET", "/plain",         Actions, plain)
        .match_action("GET", "/coroutine/:id", Actions, coroutine)
        .match_action("GET", "/forgets",       Actions, forgets)
        .match_action("GET", "/forgets-async", Actions, forgets_async)
        .match_action("GET", "/throws",        Actions, throws)
        .match_action("GET", "/throws-async",  Actions, throws_async)
        .match_action("GET", "/spawns-then-throws", Actions, spawns_then_throws)
        .match_action("GET", "/spawns-failing-task", Actions, spawns_failing_task)
        .match_action("GET", "/spawns",        Actions, spawns)
        .match_action("GET", "/spawns-async",  Actions, spawns_async)
        .match_action("GET", "/guarded",       Guarded, action)
        .match_action("GET", "/finalize-fails", FinalizeFails, action)
        .match_action("GET", "/layered",       Layer2, layered)
        .match_action("GET", "/hidden",        Hidden, plain);

  // The phases, in order, and the response is sent once everything is over. The action goes on while co_finalize waits.
  {
    auto response = run(server, "/plain");

    assert(response.status == 200 && response.body == "plain");
    assert((Trace::events == vector<string>{
      "initialize", "co_initialize:begin", "co_initialize:end", "action", "finalize", "co_finalize:begin",
      "action:after render", "co_finalize:end", "session:finalize"
    }));
    assert(response.header("X-Finalized") == "yes");                                        // co_finalize could still change the response
    assert(MemorySessionStore::cookie["audit"].defaults_to<string>("") == "written by co_finalize"); // and the session, that is saved after it
    assert(Traced::finalize_count == 1);
    assert(Counted::alive == 0);
  }

  // An action that is a coroutine: it can wait, and it finds its params (the ones of the path, and the names of the controller) when it wakes up
  {
    auto response = run(server, "/coroutine/12");

    assert(response.status == 200);
    assert(response.body == "Actions::coroutine/12");
    assert(response.header("Content-Type") == "text/plain");
    assert((Trace::events == vector<string>{
      "initialize", "co_initialize:begin", "co_initialize:end", "action:begin", "finalize", "co_finalize:begin",
      "action:after render", "co_finalize:end", "session:finalize"
    }));
    assert(Counted::alive == 0);
  }

  // A request that nobody closed is closed for you, and the finalizers run, even when co_finalize has to wait
  {
    for (string target : {"/forgets", "/forgets-async"})
    {
      auto response = run(server, target);

      assert(response.status == 200);
      assert(response.body.empty());
      assert(response.header("X-Finalized") == "yes");
      assert(in_order({"initialize", "action", "finalize", "co_finalize:begin", "co_finalize:end", "session:finalize"}) || in_order({"initialize", "action:begin", "action:end", "finalize", "co_finalize:begin", "co_finalize:end", "session:finalize"}));
      assert(Traced::finalize_count == 1);
      assert(Counted::alive == 0);
    }
  }

  // An action that starts a task, plain or coroutine: the request stays open until the task answers
  {
    for (string target : {"/spawns", "/spawns-async"})
    {
      auto response = run(server, target);

      assert(response.status == 200 && response.body == "task");
      assert(Trace::position("task") < Trace::position("finalize"));
      assert(Traced::finalize_count == 1);
      assert(Counted::alive == 0);
    }
  }

  // An initializer that answers: nothing that follows runs, but the finalizers do
  {
    auto response = run(server, "/guarded", {{"sync-deny", "yes"}});

    assert(response.status == 401);
    assert((Trace::events == vector<string>{"initialize", "finalize", "co_finalize:begin", "co_finalize:end", "session:finalize"}));
    response = run(server, "/guarded", {{"deny", "yes"}});
    assert(response.status == 403);
    assert(!Trace::contains("action"));
    assert(in_order({"co_initialize:begin", "co_initialize:end", "finalize", "co_finalize:end"}));
    response = run(server, "/guarded");
    assert(response.status == 200 && response.body == "allowed");
    assert(Trace::contains("action"));
  }

  // Failures: the exception catcher answers, and the request is not closed: a finalizer runs before the response is sent, or does not run
  {
    for (string target : {"/throws", "/throws-async"})
    {
      auto response = run(server, target);

      assert(response.status == 500);
      assert(Traced::finalize_count == 0);
      assert(!Trace::contains("finalize") && !Trace::contains("co_finalize:begin"));
      assert(Counted::alive == 0);                     // (nothing waits for a finalizer: the controller is released at once)
    }
    auto response = run(server, "/guarded", {{"fail", "yes"}});   // co_initialize() throws
    assert(response.status == 500);
    assert(!Trace::contains("action") && Traced::finalize_count == 0);
    response = run(server, "/finalize-fails", {{"async", "yes"}});  // co_finalize() throws: finalize() ran, before the response, and the response is the error
    assert(response.status == 500);
    assert(Traced::finalize_count == 1);
    assert(!Trace::contains("session:finalize"));
    response = run(server, "/finalize-fails", {{"sync", "yes"}});   // finalize() throws: it reaches the action that closed the request
    assert(response.status == 500);
    assert(Traced::finalize_count == 1);
    assert(!Trace::contains("co_finalize:begin"));
    assert(Counted::alive == 0);
  }

  // A task that fails after the action returned: the catcher answers, and when the controller is released the request is not closed (that would run the finalizers after the response)
  {
    auto response = run(server, "/spawns-failing-task");

    assert(response.status == 500);
    assert(pump_until([]() { return Counted::alive == 0; }));
    assert(Traced::finalize_count == 0);
    assert(!Trace::contains("finalize") && !Trace::contains("co_finalize:begin"));
  }

  // A task that closes the request after the catcher has answered does not run the finalizers late
  {
    auto response = run(server, "/spawns-then-throws");

    assert(response.status == 500 && response.body != "late");
    assert(pump_until([]() { return Counted::alive == 0; }));   // the controller lives until its task is over
    assert(Trace::contains("task"));
    assert(Traced::finalize_count == 0);
    assert(!Trace::contains("finalize"));
  }

  // The hooks can be protected methods of the controller: ActionRoute reaches them through the base classes
  {
    auto response = run(server, "/hidden");

    assert(response.status == 200 && response.body == "hidden");
    assert(in_order({"hidden:initialize", "hidden:co_initialize", "hidden:action", "hidden:finalize", "hidden:co_finalize", "session:finalize"}));
    assert(Counted::alive == 0);
  }

  // The initializers and the finalizers of the base classes are called by the ones of the derived classes
  {
    run(server, "/layered");
    assert(in_order({"co_initialize:end", "layer1", "layer2", "action"}));
    assert(in_order({"co_finalize:end", "layer1:finalize", "layer2:finalize", "session:finalize"}));
  }

  // Every phase of every request runs on the strand of its connection
  {
    for (string target : {"/plain", "/coroutine/1", "/forgets-async", "/spawns", "/throws-async"})
    {
      run(server, target);
      for (const auto& event : Trace::events)
        assert(event.find("NOT ON THE STRAND") == string::npos);
    }
  }

  // Many requests at once, all kinds: one response each, one finalize each, and every controller is released
  {
    const int count = 120;
    vector<Pending> pending;
    vector<string>  targets{"/plain", "/coroutine/7", "/forgets", "/forgets-async", "/spawns-async", "/throws-async"};

    Traced::finalize_count = 0;
    MemorySessionStore::reset();
    expected_strand = nullptr;
    for (int i = 0 ; i < count ; ++i)
    {
      TestRequest request;

      request.target = targets[i % targets.size()];
      pending.push_back(start(server, request));
    }
    auto responses = finish_all(pending);   // every response is read the moment its request is over
    for (int i = 0 ; i < count ; ++i)
    {
      auto& response = responses[i];

      switch (i % targets.size())
      {
      case 0: assert(response.status == 200 && response.body == "plain"); break ;
      case 1: assert(response.status == 200 && response.body == "Actions::coroutine/7"); break ;
      case 4: assert(response.status == 200 && response.body == "task"); break ;
      case 5: assert(response.status == 500); break ;
      default: assert(response.status == 200 && response.body.empty());
      }
    }
    pending.clear();
    drain();
    assert(Traced::finalize_count == count - count / 6);   // the /throws-async ones are answered by the catcher, and not finalized
    assert(Counted::alive == 0);
  }

  return 0;
}

