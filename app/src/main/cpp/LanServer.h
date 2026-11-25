#pragma once

#include "webcast/LocalWebCastSignaling.h"

#include <filesystem>

namespace nearcast::android {

std::string randomHex(unsigned bytes);
std::string randomRoomValue(unsigned length);

struct LanEvent {
    enum class Kind { Open, Message, Closed } kind;
    std::string connection;
    std::string text;
};

class LanServer {
public:
    using Receive = std::function<bool(LanEvent)>;
    using Display = std::function<bool(const std::string&)>;

    LanServer(std::string device, std::string runtime, Receive receive, Display showCode, Display showTrust);
    ~LanServer();
    std::uint16_t start(const std::filesystem::path& directory);
    void send(const webcast::LocalSignalDelivery& delivery);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nearcast::android
