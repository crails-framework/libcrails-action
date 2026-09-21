#pragma once
#include "controller.hpp"
#include <crails/controller/coroutine.hpp>

#include <iostream>

namespace Crails
{
  template<typename CONTROLLER>
  class ActionRoute<CONTROLLER, true>
  {
    template<typename ACTION>
    using ActionResult = std::invoke_result_t<ACTION, CONTROLLER&>;

    static ActionController*    as_action(CONTROLLER* controller)    { return controller; }
    static CoroutineController* as_coroutine(CONTROLLER* controller) { return controller; }
  public:
    template<typename ACTION>
    static void trigger(Crails::Context& context, ACTION action, std::function<void()> callback)
    {
      static_assert(
        std::is_void_v<ActionResult<ACTION>> || std::is_same_v<ActionResult<ACTION>, boost::asio::awaitable<void>>,
        "Crails::ActionRoute: an action must return void, or boost::asio::awaitable<void>."
      );

      std::shared_ptr<CONTROLLER> controller(new CONTROLLER(context), &ActionRoute<CONTROLLER>::destroy);
      CoroutineController* base = controller.get();

      if (!context.response.sent())
      {
        base->callback =
          std::bind(&ActionRoute<CONTROLLER>::finalize, controller.get(), callback);
        base->co_spawn([controller, action]() -> boost::asio::awaitable<void>
        {
          co_await run(controller, action);
        });
      }
      else
        callback();
    }

  private:
    template<typename ACTION>
    static boost::asio::awaitable<void> run(std::shared_ptr<CONTROLLER> controller, ACTION action)
    {
      std::exception_ptr error;
      CoroutineController* base = controller.get();

      try
      {
        base->initialize();
        if (!base->is_closed())
          co_await base->co_initialize();
        if (!base->is_closed())
        {
          if constexpr (std::is_void_v<ActionResult<ACTION>>)
            std::invoke(action, *controller);
          else
            co_await std::invoke(action, *controller);
        }
      }
      catch (...)
      {
        error = std::current_exception();
      }
      if (error)
      {
        base->callback = nullptr;
        base->context->protect([error]() { std::rethrow_exception(error); });
      }
      base->should_close_on_deletion = !base->is_closed();
    }

    static void finalize(CONTROLLER* controller, std::function<void()> callback)
    {
      CoroutineController* base = controller;

      base->finalize();
      base->co_spawn([controller, base, callback]() -> boost::asio::awaitable<void>
      {
        co_await base->co_finalize();
        callback();
      });
    }

    static void destroy(CONTROLLER* controller)
    {
      CoroutineController* base = controller;

      if (base->should_close_on_deletion && !base->is_closed())
      {
        std::shared_ptr<CONTROLLER> owner(controller, &ActionRoute<CONTROLLER>::deallocate);

        try
        {
          boost::asio::post(as_coroutine(controller)->get_io_executor(), [owner, base]()
          {
            base->context->protect(std::bind(&ActionController::close, owner));
          });
        }
        catch (...) {}
      }
      else
        deallocate(controller);
    }

    static void deallocate(CONTROLLER* controller)
    {
      delete controller;
    }
  };
}
