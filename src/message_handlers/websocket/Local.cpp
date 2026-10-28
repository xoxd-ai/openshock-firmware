#include "message_handlers/WebSocket.h"

const char* const TAG = "LocalMessageHandlers";

#include "captiveportal/Manager.h"
#include "Logging.h"
#include "message_handlers/impl/WSLocal.h"
#include "serialization/WSLocal.h"
#include "softproxy/Lockout.h"

#include "serialization/_fbs/LocalToHubMessage_generated.h"

#include <WebSockets.h>

#include <array>
#include <cstdint>

namespace Schemas  = OpenShock::Serialization::Local;
namespace Handlers = OpenShock::MessageHandlers::Local::_Private;
typedef Schemas::LocalToHubMessagePayload PayloadType;

using namespace OpenShock;

const std::size_t HANDLER_COUNT = static_cast<std::size_t>(PayloadType::MAX) + 1;

#define SET_HANDLER(payload) handlers[static_cast<std::size_t>(PayloadType::payload)] = Handlers::Handle##payload

static std::array<Handlers::HandlerType, HANDLER_COUNT> s_localHandlers = []() {
  std::array<Handlers::HandlerType, HANDLER_COUNT> handlers {};
  handlers.fill(Handlers::HandleInvalidMessage);

  SET_HANDLER(WifiScanCommand);
  SET_HANDLER(WifiNetworkSaveCommand);
  SET_HANDLER(WifiNetworkForgetCommand);
  SET_HANDLER(WifiNetworkConnectCommand);
  SET_HANDLER(WifiNetworkDisconnectCommand);
  SET_HANDLER(OtaUpdateSetIsEnabledCommand);
  SET_HANDLER(OtaUpdateSetDomainCommand);
  SET_HANDLER(OtaUpdateSetUpdateChannelCommand);
  SET_HANDLER(OtaUpdateSetCheckIntervalCommand);
  SET_HANDLER(OtaUpdateSetAllowBackendManagementCommand);
  SET_HANDLER(OtaUpdateSetRequireManualApprovalCommand);
  SET_HANDLER(OtaUpdateHandleUpdateRequestCommand);
  SET_HANDLER(OtaUpdateCheckForUpdatesCommand);
  SET_HANDLER(OtaUpdateStartUpdateCommand);
  SET_HANDLER(AccountLinkCommand);
  SET_HANDLER(AccountUnlinkCommand);
  SET_HANDLER(SetRfTxPinCommand);
  SET_HANDLER(SetEstopEnabledCommand);
  SET_HANDLER(SetEstopPinCommand);

  return handlers;
}();

void MessageHandlers::WebSocket::HandleLocalBinary(uint8_t socketId, tcb::span<const uint8_t> data)
{
  // Deserialize
  auto msg = flatbuffers::GetRoot<Schemas::LocalToHubMessage>(data.data());
  if (msg == nullptr) {
    OS_LOGE(TAG, "Failed to deserialize message");
    return;
  }

  // Validate buffer
  flatbuffers::Verifier::Options verifierOptions {
    .max_size = 4096,  // TODO: Profile this
  };
  flatbuffers::Verifier verifier(data.data(), data.size(), verifierOptions);
  if (!msg->Verify(verifier)) {
    OS_LOGE(TAG, "Failed to verify message");
    return;
  }

  if (msg->payload_type() < PayloadType::MIN || msg->payload_type() > PayloadType::MAX) {
    Handlers::HandleInvalidMessage(socketId, msg);
    return;
  }

  // Softproxy fork (SP-46): the port 81 socket runs only the commands softproxy/Lockout.h allows. The E-Stop,
  // E-Stop pin, Wi-Fi and account-link commands refuse here and stay on the serial port.
  const char* payloadName = Schemas::EnumNameLocalToHubMessagePayload(msg->payload_type());
  if (!Softproxy::LocalSocketAllows(payloadName)) {
    OS_LOGW(TAG, "Refusing local socket command %s from client #%u, use the serial port", payloadName, socketId);
    Serialization::Local::SerializeErrorMessage("refused_local_command", [socketId](tcb::span<const uint8_t> data) { return CaptivePortal::SendMessageBIN(socketId, data); });
    return;
  }

  s_localHandlers[static_cast<std::size_t>(msg->payload_type())](socketId, msg);
}
