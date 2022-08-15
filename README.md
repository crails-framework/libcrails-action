# libcrails-action

A router for the Crails Server.

## Usage

### Adding the action handler to the request pipe:

Before using the router, you must add the ActionRequestHandler to the server's request pipe, as such:

config/request_pipe.cpp
``` 
#include <crails/request_handlers/action.hpp>

void Crails::Server::initialize_request_pipe()
{
  add_request_handler(new ActionRequestHandler);
}
```

### Instantiating the router

An instance of the Router must always be available as the server runs. A good way to ensure that is
to instantiate it in the main function:

app/main.cpp
```
#include <crails/server.hpp>
#include <crails/router.hpp>

int main(int argc, const char** argv)
{
  SingletonInstantiator<Crails::Router> router;

  Crails::Server::launch(argc, argv);
  return 0;
}
```

### Using the router

You must now declare the `Crails::Router::initialize` method, and create your routes from within it:

app/routes.cpp
```
#include <crails/router.hpp>
#include <crails/http.hpp>

void Crails::Router::initialize()
{
  match("GET", "/", [](Crails::Context& context, std::function<void()> callback)
  {
    context.response.set_header(Crails::HttpHeader::context_type, "text/plain");
    context.response.set_response(Crails::HttpStatus::ok, "Hello world !");
    callback();
  });
}
```
