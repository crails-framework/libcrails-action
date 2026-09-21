#include <crails/environment.hpp>
#include <crails/session_store/no_session_store.hpp>
#include <crails/request_handlers/action.hpp>
#include <crails/actions/controller.hpp>
#include <crails/router.hpp>
#include <iostream>
#include "test_server.hpp"

namespace Crails
{
  Environment environment = Test;
}

#undef NODEBUG
#include <cassert>

int main()
{
  using namespace Crails;
  using namespace std;

  // Simple test
  {
    auto* parser = new TestParser();
    auto* handler = new ActionRequestHandler();
    SingletonInstantiator<TestServer> server;
    SingletonInstantiator<Crails::NoSessionStore::Factory> store;
    SingletonInstantiator<Router> router;
    server->test_setup(vector<Crails::RequestHandler*>{handler}, vector<Crails::RequestParser*>{parser});
    bool success;

    router->match("POST", "/simple-route", [&](Crails::Context& context, std::function<void()> callback)
    {
      success = true;
      callback();
    });

    // Calls the callback
    {
      HttpRequest request;
      request.method(HttpVerb::post);
      request.target("/simple-route");
      auto connection = make_shared<Connection>(*server, request);
      auto context = make_shared<TestContext>(*server, *connection);

      success = false;
      context->test_run();
      assert(success);
    }

    // Respects HTTP verbs
    {
      HttpRequest request;
      request.method(HttpVerb::get);
      request.target("/simple-route");
      auto connection = make_shared<Connection>(*server, request);
      auto context = make_shared<TestContext>(*server, *connection);

      success = false;
      context->test_run();
      assert(!success);
    }

    // Implements _method workaround to override the actual query's verb
    {
      HttpRequest request;
      request.method(HttpVerb::get);
      request.target("/simple-route");
      auto connection = make_shared<Connection>(*server, request);
      auto context = make_shared<TestContext>(*server, *connection);

      parser->override_verb = "POST";
      success = false;
      context->test_run();
      assert(success);
    }
  }

  return 0;
}
