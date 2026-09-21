#ifndef  CRAILS_CONTROLLER_ACTION_HPP
# define CRAILS_CONTROLLER_ACTION_HPP

# include <crails/context.hpp>
# include <crails/controller/action.hpp>
# include <crails/controller/coroutine.hpp>
# include <crails/logger.hpp>
# include <climits>

namespace Crails
{
  template<typename CONTROLLER, bool WITH_ASYNC = std::is_base_of<Crails::CoroutineController, CONTROLLER>::value>
  class ActionRoute
  {
    typedef void (CONTROLLER::*Method)();
  public:
    static void trigger(Crails::Context& context, Method action, std::function<void()> callback)
    {
      std::shared_ptr<CONTROLLER> controller(new CONTROLLER(context), &ActionRoute<CONTROLLER>::destroy);
      ActionController* base = controller.get();

      if (!context.response.sent())
      {
        base->callback =
          std::bind(&ActionRoute<CONTROLLER>::finalize, controller.get(), callback);
        run(controller.get(), action);
      }
      else
        callback();
    }

    template<typename ACTION>
    static void trigger(Crails::Context&, ACTION, std::function<void()>)
    {
      static_assert(
        sizeof(ACTION) == 0,
        "Crails::ActionRoute: a synchronous action signature must be `void Controller::*action)()`.\n"
        "An asynchronous action requires a controller inheriting Crails::CoroutineController.\n"
        "This assertion failed because neither conditions were met."
      );
    }
  private:
    static void run(CONTROLLER* controller, Method action)
    {
      ActionController* base = controller;

      base->initialize();
      if (!base->is_closed())
        (controller->*action)();
      base->should_close_on_deletion = !base->is_closed();
    }

    static void finalize(CONTROLLER* controller, std::function<void()> callback)
    {
      ActionController* base = controller;

      base->finalize();
      callback();
    }

    static void destroy(CONTROLLER* controller)
    {
      ActionController* base = controller;

      if (base->should_close_on_deletion && !base->is_closed() && !base->response.sent())
      {
        try
        {
          base->close();
        }
        catch (...) {}
      }
      delete controller;
    }
  };
}

# include "coroutine.hpp"

# define match_action_with_priority(priority, method, path, controller, action) \
  match(priority, method, path, [](Crails::Context& context, std::function<void()> callback) \
  { \
    Crails::logger << Crails::Logger::Debug << "ActionRequestHandler: triggering action " << #controller << "::" << #action << Crails::Logger::endl; \
    context.params["controller-data"]["name"]   = #controller; \
    context.params["controller-data"]["action"] = #action; \
    Crails::ActionRoute<controller>::trigger(context, &controller::action, callback); \
  })

# define match_action(method, path, controller, action) \
  match(method, path, [](Crails::Context& context, std::function<void()> callback) \
  { \
    Crails::logger << Crails::Logger::Debug << "ActionRequestHandler: triggering action " << #controller << "::" << #action << Crails::Logger::endl; \
    context.params["controller-data"]["name"]   = #controller; \
    context.params["controller-data"]["action"] = #action; \
    Crails::ActionRoute<controller>::trigger(context, &controller::action, callback); \
  })

# define match_fallback_action(method, path, controller, action) \
  match_action_with_priority(SHRT_MAX, method, path, controller, action)

# define crud_actions(resource_name, controller) \
   match_action("GET",    '/' + std::string(resource_name),               controller,index)  \
  .match_action("GET",    '/' + std::string(resource_name) + "/:id" ,     controller,show)   \
  .match_action("POST",   '/' + std::string(resource_name),               controller,create) \
  .match_action("PATCH",  '/' + std::string(resource_name) + "/:id",      controller,update) \
  .match_action("PUT",    '/' + std::string(resource_name) + "/:id",      controller,update) \
  .match_action("DELETE", '/' + std::string(resource_name) + "/:id",      controller,destroy)

# define resource_actions(resource_name, controller) \
   match_action("GET",    '/' + std::string(resource_name) + "/new",      controller,new_)   \
  .match_action("GET",    '/' + std::string(resource_name) + "/:id/edit", controller,edit)   \
  .crud_actions(resource_name, controller)

#endif
