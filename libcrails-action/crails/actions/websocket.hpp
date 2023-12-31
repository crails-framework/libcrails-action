#ifndef  CRAILS_WEBSOCKET_ACTION_HPP
# define CRAILS_WEBSOCKET_ACTION_HPP

# include <crails/context.hpp>
# include <boost/beast/websocket.hpp>

namespace Crails
{
  template<typename WEBSOCKET>
  class WebSocketRoute
  {
    typedef void (WEBSOCKET::*Method)();
  public:
    static void trigger(Crails::Context& context, Method method, std::function<void()> callback)
    {
      const HttpRequest& request = context.connection->get_request();

      if (boost::beast::websocket::is_upgrade(request))
      {
        auto websocket = std::make_shared<WEBSOCKET>(context);

        context.response.set_status_code(HttpStatus::switching_protocols);
        websocket->accept(request);
        (websocket.get()->*method)();
      }
      else
        context.response.set_status_code(HttpStatus::bad_request);
      callback();
    }
  };
}

# define match_websocket(path, websocket, action) \
  match("GET", path, [](Crails::Context& context, std::function<void()> callback) \
  { \
    Crails::WebSocketRoute<websocket>::trigger(context, &websocket::action, callback); \
  });

#endif
