#ifndef  ROUTER_HPP
# define ROUTER_HPP

# include <exception>
# include <iostream>
# include <crails/utils/singleton.hpp>
# include <crails/context.hpp>
# include <crails/router_base.hpp>
# include "actions/controller.hpp"
# include "actions/websocket.hpp"

namespace Crails
{
  class Router : public RouterBase<Crails::Params, std::function<void (Crails::Context&, std::function<void()>)> >
  {
    SINGLETON(Router)
  protected:
    Router() {}
    virtual ~Router() {}
  };
}

#endif
