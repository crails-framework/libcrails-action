#ifndef  CRAILS_CONTROLLER_ACTION_HPP
# define CRAILS_CONTROLLER_ACTION_HPP

# include <crails/context.hpp>
# include <crails/controller/action.hpp>
# include <climits>

namespace Crails
{
  template<typename CONTROLLER>
  class ActionRoute
  {
    typedef void (CONTROLLER::*Method)();
  public:
    static void trigger(Crails::Context& context, Method method, std::function<void()> callback)
    {
      auto controller = std::make_shared<CONTROLLER>(context);

      if (!context.response.sent())
      {
        controller->ActionController::callback =
          std::bind(&ActionRoute<CONTROLLER>::finalize, controller.get(), callback);
        controller->initialize();
        if (!context.response.sent())
          (controller.get()->*method)();
      }
      else
        callback();
      controller->close_on_deletion = true;
    }

  private:
    static void finalize(CONTROLLER* controller, std::function<void()> callback)
    {
      controller->finalize();
      callback();
    }
  };
}

# define match_action_with_priority(priority, method, path, controller, action) \
  match(priority, method, path, [](Crails::Context& context, std::function<void()> callback) \
  { \
    context.params["controller-data"]["name"]   = #controller; \
    context.params["controller-data"]["action"] = #action; \
    Crails::ActionRoute<controller>::trigger(context, &controller::action, callback); \
  })

# define match_action(method, path, controller, action) \
  match_action_with_priority(0, method, path, controller, action)

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
