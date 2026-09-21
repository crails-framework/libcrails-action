#pragma once
#include <crails/server.hpp>
#include <crails/request_handler.hpp>
#include <crails/request_parser.hpp>
#include <thread>
#include <optional>
#include <iostream>

struct TestServer : public Crails::Server
{
  SINGLETON_IMPLEMENTATION(TestServer, Crails::Server)
public:
  TestServer()
  {
  }

  template<typename LISTA, typename LISTB>
  void test_setup(const LISTA& handlers, const LISTB& parsers)
  {
    for (auto* handler : handlers)
      add_request_handler(handler);
    for (auto* parser : parsers)
      add_request_parser(parser);
  }
};

struct TestParser : public Crails::RequestParser
{
  mutable std::optional<std::string> override_verb;
  Crails::RequestParser::Status return_value = Crails::RequestParser::Continue;

  void operator()(Crails::Context& context, std::function<void(Crails::RequestParser::Status)> callback) const override
  {
    if (override_verb)
    {
      context.params["_method"] = *override_verb;
      override_verb.reset();
    }
    callback(return_value);
  }
};

struct TestContext : public Crails::Context
{
  TestContext(const TestServer& server, Crails::Connection& connection)
    : Crails::Context(server, connection)
  {
  }

  void test_run() { run(); }
};
