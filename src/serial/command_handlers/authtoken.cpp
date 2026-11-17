#include "serial/command_handlers/common.h"

#include "config/Config.h"
#include "http/JsonAPI.h"

#include <string>

void _handleAuthtokenCommand(std::string_view arg, bool isAutomated)
{
  if (arg.empty()) {
    std::string authToken;
    if (!OpenShock::Config::GetBackendAuthToken(authToken)) {
      SERPR_ERROR("Failed to get auth token from config");
      return;
    }

    // Get auth token
    SERPR_RESPONSE("AuthToken|%s", authToken.c_str());
    return;
  }

#ifndef OPENSHOCK_GATEWAY_DIRECT_PATH
  auto apiResponse = OpenShock::HTTP::JsonAPI::GetHubInfo(arg);
  if (apiResponse.code == 401) {
    SERPR_ERROR("Invalid auth token, refusing to save it!");
    return;
  }
#endif
  // Softproxy fork (TIN-7125): with a direct gateway the domain serves no HTTP API, so the token is saved unchecked and
  // the gateway checks it on the WebSocket upgrade.

  // If we have some other kind of request fault just set it anyway, we probably arent connected to a network

  bool result = OpenShock::Config::SetBackendAuthToken(std::string(arg));

  if (result) {
    SERPR_SUCCESS("Saved config");
  } else {
    SERPR_ERROR("Failed to save config");
  }
}

OpenShock::Serial::CommandGroup OpenShock::Serial::CommandHandlers::AuthTokenHandler()
{
  auto group = OpenShock::Serial::CommandGroup("authtoken"sv);

  auto& getCommand = group.addCommand("Get the backend auth token"sv, _handleAuthtokenCommand);

  auto& setCommand = group.addCommand("Set the auth token"sv, _handleAuthtokenCommand);
  setCommand.addArgument("token"sv, "must be a string"sv, "mytoken"sv);

  return group;
}
