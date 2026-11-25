#include "LanRoom.h"

namespace nearcast::android {
namespace {
const std::string kReceiverConnection = "android-receiver";

void append(webcast::Effects& output, webcast::Effects input) {
    for (auto& effect : input) output.push_back(std::move(effect));
}
} // namespace
LanRoom::LanRoom(std::string name, std::function<std::string(unsigned)> random, Deliver deliver)
    : server_(std::move(random)), receiver_(std::move(name), 1), send_(std::move(deliver)) {}

webcast::Effects LanRoom::start(std::uint64_t now) {
    auto effects = receiver_.SocketOpened(now);
    auto output = deliver(server_.Open(kReceiverConnection, true, {}, now), now);
    append(output, route(std::move(effects), now));
    return output;
}

webcast::Effects LanRoom::open(const std::string& connection, const std::string& token, std::uint64_t now) {
    return deliver(server_.Open(connection, false, token, now), now);
}

webcast::Effects LanRoom::receive(const std::string& connection, const std::string& text, std::uint64_t now) {
    return deliver(server_.Receive(connection, text, now), now);
}

webcast::Effects LanRoom::close(const std::string& connection, std::uint64_t now) {
    return deliver(server_.Closed(connection, now), now);
}

webcast::Effects LanRoom::tick(std::uint64_t now) {
    auto output = deliver(server_.Tick(now), now);
    append(output, route(receiver_.Tick(now), now));
    return output;
}

webcast::Effects LanRoom::route(webcast::Effects effects, std::uint64_t now) {
    webcast::Effects output;
    for (auto& effect : effects) {
        if (effect.kind == webcast::EffectKind::SocketSend) {
            append(output, deliver(server_.Receive(kReceiverConnection, effect.payload.dump(), now), now));
        } else {
            output.push_back(std::move(effect));
        }
    }
    return output;
}

webcast::Effects LanRoom::deliver(webcast::LocalSignalDeliveries messages, std::uint64_t now) {
    webcast::Effects output;
    for (auto& message : messages) {
        if (message.connection == kReceiverConnection && !message.close) {
            const auto epoch = receiver_.Snapshot().socketEpoch;
            append(output, route(receiver_.Receive(epoch, message.message.dump()), now));
        } else if (message.connection != kReceiverConnection) {
            send_(std::move(message));
        }
    }
    return output;
}

} // namespace nearcast::android
