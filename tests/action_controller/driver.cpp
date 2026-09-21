#include <crails/environment.hpp>
#include <crails/controller.hpp>
#include "../test_helpers.hpp"

#undef NDEBUG
#include <cassert>

using namespace Crails;
using namespace Harness;
using namespace std;

struct Sync : public Crails::FlashController
{
  typedef Crails::FlashController Super;
  Counted counted;
  Sync(Crails::Context& context) : Super(context) {}

  static inline int        finalize_count = 0;
  static inline HttpStatus status_at_finalize = HttpStatus::ok;

  bool must_protect_from_forgery() const override { return false; }

  void initialize() override
  {
    Trace::add("initialize");
    Super::initialize();
    if (params["deny"].exists())
      respond_with(HttpStatus::forbidden);
  }
  void finalize() override
  {
    ++finalize_count;
    status_at_finalize = response.get_status_code();
    Trace::add("finalize");
    Super::finalize();
    if (params["finalize-fails"].exists())
      throw std::runtime_error("finalize failed");
  }

  void plain()   { Trace::add("action"); render(TEXT, string_view("sync")); Trace::add("action:after render"); }
  void forgets() { Trace::add("action"); }
  void throws()  { Trace::add("action"); throw std::runtime_error("boom"); }
};

struct Async : public Crails::Controller
{
  Counted counted;
  Async(Crails::Context& context) : Crails::Controller(context) {}
  bool must_protect_from_forgery() const override { return false; }
  void plain() { render(TEXT, string_view("coroutine controller")); }
};

// The methods that ActionRoute calls are protected here, as they often are in applications
struct HiddenSync : public Crails::FlashController
{
  Counted counted;
  HiddenSync(Crails::Context& context) : Crails::FlashController(context) {}
  bool must_protect_from_forgery() const override { return false; }
  void plain() { Trace::add("hidden:action"); render(TEXT, string_view("hidden")); }
protected:
  void initialize() override { Trace::add("hidden:initialize"); Crails::FlashController::initialize(); }
  void finalize() override { Trace::add("hidden:finalize"); Crails::FlashController::finalize(); }
};

// Sends the request, and looks at it at once: nothing has run the io_context
static Response at_once(TestServer& server, const string& target, map<string,string> params = {}, bool* ready = nullptr)
{
  TestRequest request;

  request.target = target;
  request.params = params;
  Trace::clear();
  Sync::finalize_count = 0;
  MemorySessionStore::reset();
  MemorySessionStore::on_finalize = []() { Trace::add("session:finalize"); };
  Pending pending = start(server, request);
  bool over = capture(pending);

  if (ready) *ready = over;
  return over ? *pending.captured : Response();
}

int main()
{
  Crails::environment = Crails::Test;
  Setup env;
  auto& server = *env.server;
  auto& router = *env.router;

  router.match_action("GET", "/plain",   Sync,  plain)
        .match_action("GET", "/forgets", Sync,  forgets)
        .match_action("GET", "/throws",  Sync,  throws)
        .match_action("GET", "/async",   Async, plain)
        .match_action("GET", "/hidden",  HiddenSync, plain);

  // The request is over when trigger() returns: no io_context is needed, and the controller is already released
  {
    bool ready = false;
    auto response = at_once(server, "/plain", {}, &ready);

    assert(ready);
    assert(response.status == 200 && response.body == "sync");
    assert((Trace::events == vector<string>{"initialize", "action", "finalize", "action:after render", "session:finalize"}) ||
           (Trace::events == vector<string>{"initialize", "action", "action:after render", "finalize", "session:finalize"}) ||
           (Trace::position("action") < Trace::position("finalize") && Trace::position("finalize") < Trace::position("session:finalize")));
    assert(Sync::finalize_count == 1);
    assert(Counted::alive == 0);
  }

  // A coroutine controller, on the contrary, needs the io_context: the request is not over yet
  {
    bool ready = true;
    TestRequest request;
    request.target = "/async";
    Pending pending = start(server, request);

    ready = capture(pending);
    assert(!ready);
    auto response = finish(pending);     // now the io_context runs
    assert(response.status == 200 && response.body == "coroutine controller");
    drain();
  }

  // A request that nobody closed is closed when the controller is released, which is before trigger() returns
  {
    bool ready = false;
    auto response = at_once(server, "/forgets", {}, &ready);

    assert(ready);
    assert(response.status == 200 && response.body.empty());
    assert(Sync::finalize_count == 1);
    assert(Counted::alive == 0);
  }

  // An initializer that responds skips the action, and the finalizers run
  {
    bool ready = false;
    auto response = at_once(server, "/plain", {{"deny", "yes"}}, &ready);

    assert(ready && response.status == 403);
    assert(!Trace::contains("action"));
    assert(Sync::finalize_count == 1);
  }

  // An exception goes to the Context::protect of the handlers: the exception catcher answers, and the request is not closed afterwards,
  // so the finalizers do not run (they run before the response is sent, or not at all)
  {
    bool ready = false;
    auto response = at_once(server, "/throws", {}, &ready);

    assert(ready && response.status == 500);
    assert(Sync::finalize_count == 0);
    assert(!Trace::contains("finalize"));
    assert(Counted::alive == 0);
  }

  // A finalizer that throws is an error like another, and the request is answered
  {
    bool ready = false;
    auto response = at_once(server, "/plain", {{"finalize-fails", "yes"}}, &ready);

    assert(ready && response.status == 500);
    assert(Sync::finalize_count == 1);
    assert(Counted::alive == 0);
  }

  // The hooks can be protected methods of the controller
  {
    bool ready = false;
    auto response = at_once(server, "/hidden", {}, &ready);

    assert(ready && response.status == 200 && response.body == "hidden");
    assert((Trace::events == vector<string>{"hidden:initialize", "hidden:action", "hidden:finalize", "session:finalize"}));
  }

  // Both kinds of controllers live on the same router, and many requests go through it
  {
    vector<Pending> pending;

    for (int i = 0 ; i < 100 ; ++i)
    {
      TestRequest request;
      request.target = i % 2 ? "/plain" : "/async";
      pending.push_back(start(server, request));
    }
    auto responses = finish_all(pending);
    for (int i = 0 ; i < 100 ; ++i)
      assert(responses[i].status == 200 && responses[i].body == (i % 2 ? "sync" : "coroutine controller"));
    pending.clear();
    drain();
    assert(Counted::alive == 0);
  }

  return 0;
}
