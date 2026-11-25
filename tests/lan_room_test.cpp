#include "LanRoom.h"

#include <cstdlib>
#include <iostream>

using nearcast::android::LanRoom;
using namespace nearcast::webcast;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

int main() {
    unsigned identity = 0;
    unsigned codeDraw = 0;
    LocalSignalDeliveries messages;
    LanRoom room("Android receiver", [&](unsigned length) {
        if (length == 4) return codeDraw++ == 0 ? std::string("0273") : std::string("6190");
        return std::string(length, static_cast<char>('A' + identity++));
    }, [&](LocalSignalDelivery message) { messages.push_back(std::move(message)); });
    room.start(100);
    check(room.receiver().Snapshot().state == SignalingState::Ready, "In-process receiver must create a room");
    check(room.receiver().PrivatePairCode() == "0273", "Initial room code must retain leading zeros");
    room.open("sender-1", "", 101);
    check(messages.size() == 2 && messages.front().message["role"] == "sender",
        "Network connections must open in the sender role");
    messages.clear();
    auto effects = room.receive("sender-1", R"({"type":"join","pairCode":"0000"})", 102);
    check(effects.empty() && messages.size() == 1 && messages.front().message["type"] == "join_failed",
        "An incorrect code must never open a media peer");

    messages.clear();
    effects = room.receive("sender-1", R"({"type":"join","pairCode":"0273"})", 103);
    check(!effects.empty() && effects.front().kind == EffectKind::PeerOpened,
        "A successful join must open the receiver peer");
    const auto peer = effects.front().peer;
    check(messages.size() == 1 && messages.front().message["type"] == "joined",
        "Sender must receive its joined message");
    const auto joined = messages.front().message;
    check(room.receiver().PrivatePairCode() == "6190", "A successful join must rotate the visible code");
    messages.clear();
    const Json offer{{"type", "offer"}, {"roomId", joined["roomId"]},
        {"offer", {{"type", "offer"}, {"sdp", "v=0\r\n"}}}};
    effects = room.receive("sender-1", offer.dump(), 104);
    check(effects.size() == 1 && effects.front().kind == EffectKind::RemoteSignal && effects.front().peer == peer,
        "An admitted sender's offer must reach its own peer");
    const Json answer{{"type", "answer"}, {"sdp", "v=0\r\n"}};
    room.route(room.receiver().SendSignal(peer, "answer", answer), 105);
    check(messages.size() == 1 && messages.front().connection == "sender-1" &&
            messages.front().message["userId"] == joined["hostUserId"],
        "Receiver answers must reach the sender with the receiver's identity");
    room.receiver().ChannelState(peer, true);
    effects = room.receiver().SendSignal(peer, "answer", answer);
    check(effects.size() == 1 && effects.front().kind == EffectKind::DataSend,
        "Renegotiation must use the open control channel");
    messages.clear();
    room.route(room.receiver().DataSendFailed(effects.front()), 106);
    check(messages.size() == 1 && messages.front().message["type"] == "answer",
        "A failed DataChannel send must fall back to WebSocket");
    room.route(room.receiver().Retire(peer), 107);
    check(room.receiver().Peers().empty(), "Retiring a sender must remove its media peer");
}
