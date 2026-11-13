#pragma once

#include "Common.h"
#include "span.h"
#include "TinyVec.h"
#include "WebSocketMessageType.h"

#include <WebSockets.h>

#include <cstdint>
#include <functional>
#include <map>

namespace OpenShock {
  class WebSocketDeFragger {
    DISABLE_COPY(WebSocketDeFragger);
    DISABLE_MOVE(WebSocketDeFragger);

  public:
    typedef std::function<void(uint8_t socketId, WebSocketMessageType type, tcb::span<const uint8_t> data)> EventCallback;

    /// Called once for a client whose fragmented message went over `maxMessageBytes`, or could not be stored. The
    /// message is already dropped. This runs inside the socket server's event, so the callback must not disconnect
    /// the client itself.
    typedef std::function<void(uint8_t socketId)> OverflowCallback;

    WebSocketDeFragger(EventCallback callback, std::size_t maxMessageBytes, OverflowCallback onOverflow);
    ~WebSocketDeFragger();

    void handler(uint8_t socketId, WStype_t type, const uint8_t* payload, std::size_t length);
    void onEvent(const EventCallback& callback);
    void clear(uint8_t socketId);
    void clear();

  private:
    void start(uint8_t socketId, WebSocketMessageType type, const uint8_t* data, uint32_t length);
    void append(uint8_t socketId, const uint8_t* data, uint32_t length);
    void finish(uint8_t socketId, const uint8_t* data, uint32_t length);
    void overflow(uint8_t socketId);

    struct Message {
      TinyVec<uint8_t> data;
      WebSocketMessageType type;
    };

    bool store(uint8_t socketId, Message& message, const uint8_t* data, uint32_t length);

    std::map<uint8_t, Message> m_messages;
    EventCallback m_callback;
    std::size_t m_maxMessageBytes;
    OverflowCallback m_onOverflow;
  };
}  // namespace OpenShock
