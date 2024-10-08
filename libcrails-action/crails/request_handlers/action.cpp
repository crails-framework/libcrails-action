#include "action.hpp"
#include "../router.hpp"
#include <crails/logger.hpp>
#include <crails/context.hpp>
#include <crails/logger.hpp>
#include <crails/environment.hpp>

using namespace std;
using namespace Crails;

void ActionRequestHandler::operator()(Context& context, function<void(bool)> callback) const
{
  const Router* router = Router::singleton::get();

  if (router)
  {
    Params&               params      = context.params;
    const auto&           request     = context.connection->get_request();
    string                implicit_method(boost::beast::http::to_string(request.method()));
    const string          uri    = params["uri"].defaults_to<string>(string(request.target()));
    string                method = params["_method"].defaults_to<string>(implicit_method);
    const Router::Action* action = router->get_action(method, uri, params);

    if (action == 0)
    {
      if (Crails::environment == Crails::Development)
        logger << Logger::Info << "Route not found for " << method << " `" << uri << "`:\n" << router->description() << Logger::endl;
      callback(false);
    }
    else
    {
      context.response.set_status_code(HttpStatus::ok);
      logger << Logger::Info << "# Responding to " << method << ' ' << uri << Logger::endl;
      params.session->load(request);
      (*action)(context, [callback, &context]()
      {
        context.params.session->finalize(context.response);
        callback(true);
      });
    }
  }
  else
  {
    logger << Logger::Error << "(!) Router not initialized" << Logger::endl;
    callback(false);
  }
}
