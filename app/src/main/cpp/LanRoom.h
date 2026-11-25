#pragma once

#include "webcast/LocalWebCastSignaling.h"

namespace nearcast::android {

// The local server and its receiver run under the same serialized owner.
// Only sender connections cross the network.
class LanRoom {
public:
    using Deliver = std::function<void(webcast::LocalSignalDelivery)>;

    LanRoom(std::string name, std::function<std::string(unsigned)> random, Deliver deliver);
    webcast::Effects start(std::uint64_t now);
    webcast::Effects open(const std::string& connection, const std::string& token, std::uint64_t now);
    webcast::Effects receive(const std::string& connection, const std::string& text, std::uint64_t now);
    webcast::Effects close(const std::string& connection, std::uint64_t now);
    webcast::Effects tick(std::uint64_t now);
    webcast::Effects route(webcast::Effects effects, std::uint64_t now);
    webcast::WebCastSignaling& receiver() { return receiver_; }

private:
    webcast::Effects deliver(webcast::LocalSignalDeliveries messages, std::uint64_t now);
    webcast::LocalWebCastSignaling server_;
    webcast::WebCastSignaling receiver_;
    Deliver send_;
};

} // namespace nearcast::android
