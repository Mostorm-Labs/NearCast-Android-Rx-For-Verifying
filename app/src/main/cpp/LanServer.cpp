#include "LanServer.h"
#include "webcast/LocalWebCastDiscovery.h"

#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <charconv>
#include <chrono>
#include <sstream>
#include <unistd.h>

namespace nearcast::android {
using namespace webcast;
namespace {
constexpr auto kSignalLimit = 1024 * 1024;

std::uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string upperHex(const unsigned char* data, std::size_t size) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 15]);
    }
    return result;
}

std::string makeIdentity(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto privateKey = directory / "local-key.pem";
    const auto certificate = directory / "local-cert.pem";
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(nullptr, EVP_PKEY_free);
    if (std::filesystem::exists(privateKey)) {
        std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(privateKey.c_str(), "rb"), BIO_free);
        if (file) key.reset(PEM_read_bio_PrivateKey(file.get(), nullptr, nullptr, nullptr));
    } else {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* generated = nullptr;
        if (context && EVP_PKEY_keygen_init(context.get()) == 1 &&
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) == 1 &&
            EVP_PKEY_keygen(context.get(), &generated) == 1) {
            key.reset(generated);
        }
        const auto temporary = directory / "local-key.new";
        std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(temporary.c_str(), "wb"), BIO_free);
        if (!key || !file || PEM_write_bio_PrivateKey(file.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
            BIO_flush(file.get()) != 1) {
            throw std::runtime_error("WebCast TLS key generation failed");
        }
        file.reset();
        std::filesystem::rename(temporary, privateKey);
    }
    if (!key) throw std::runtime_error("WebCast TLS key could not be read");

    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    unsigned char serial[16]{};
    if (!cert || RAND_bytes(serial, sizeof serial) != 1) throw std::runtime_error("WebCast certificate creation failed");
    serial[0] = (serial[0] & 0x7f) | 1;
    std::unique_ptr<BIGNUM, decltype(&BN_free)> number(BN_bin2bn(serial, sizeof serial, nullptr), BN_free);
    auto* subject = X509_get_subject_name(cert.get());
    const auto* name = reinterpret_cast<const unsigned char*>("NearCast Android WebCast");
    if (!number || !X509_set_version(cert.get(), 2) ||
        !BN_to_ASN1_INTEGER(number.get(), X509_get_serialNumber(cert.get())) ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 365L * 24 * 60 * 60) ||
        !X509_set_pubkey(cert.get(), key.get()) ||
        !X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, name, -1, -1, 0) ||
        !X509_set_issuer_name(cert.get(), subject) ||
        X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) {
        throw std::runtime_error("WebCast certificate signing failed");
    }
    std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(certificate.c_str(), "wb"), BIO_free);
    if (!file || PEM_write_bio_X509(file.get(), cert.get()) != 1 || BIO_flush(file.get()) != 1) {
        throw std::runtime_error("WebCast certificate write failed");
    }

    unsigned char* der = nullptr;
    const auto size = i2d_PUBKEY(key.get(), &der);
    unsigned char digest[32]{};
    unsigned length = 0;
    const bool ok = size > 0 && EVP_Digest(der, size, digest, &length, EVP_sha256(), nullptr) && length == 32;
    OPENSSL_free(der);
    if (!ok) throw std::runtime_error("WebCast TLS fingerprint failed");
    return upperHex(digest, length);
}

std::uint16_t ephemeralPort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    socklen_t size = sizeof address;
    const auto ok = !bind(fd, reinterpret_cast<sockaddr*>(&address), size) &&
        !getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    close(fd);
    return ok ? ntohs(address.sin_port) : 0;
}

std::string response(const char* status, const Json& body) {
    const auto content = body.dump();
    return std::string("HTTP/1.1 ") + status + "\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "Content-Type: application/json\r\nContent-Length: " + std::to_string(content.size()) + "\r\n\r\n" + content;
}

std::optional<std::string> senderToken(const std::string& uri, const std::string& runtime) {
    const auto prefix = "/?role=sender&runtime=" + runtime;
    if (uri == prefix) return std::string{};
    const auto tokenPrefix = prefix + "&token=";
    if (uri.compare(0, tokenPrefix.size(), tokenPrefix) != 0) return {};
    const auto token = uri.substr(tokenPrefix.size());
    if (token.empty() || token.size() > 64 ||
        token.find_first_not_of("0123456789ABCDEF") != std::string::npos) return {};
    return token;
}
} // namespace

std::string randomHex(unsigned bytes) {
    std::vector<unsigned char> entropy(bytes);
    if (RAND_bytes(entropy.data(), bytes) != 1) throw std::runtime_error("Unable to generate receiver identity");
    return upperHex(entropy.data(), entropy.size());
}

std::string randomRoomValue(unsigned length) {
    if (length != 4) return randomHex(length / 2);
    unsigned short entropy = 0;
    do {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&entropy), sizeof entropy) != 1) {
            throw std::runtime_error("Unable to generate pairing code");
        }
    } while (entropy >= 60000);
    return std::to_string(10000 + entropy % 10000).substr(1);
}

struct LanServer::Impl {
    class Server final : public ix::WebSocketServer {
    public:
        Server(unsigned port, Impl& owner)
            : ix::WebSocketServer(port, "0.0.0.0", 8, 12, 3, AF_INET, 10), owner_(owner) {
            disablePerMessageDeflate();
        }

    private:
        void handleConnection(std::unique_ptr<ix::Socket> socket, std::shared_ptr<ix::ConnectionState> state) override {
            const auto deadline = nowMs() + 3000;
            auto expired = [&] { return nowMs() >= deadline; };
            std::string header;
            char next;
            while (header.size() < 16384 && socket->readByte(&next, expired)) {
                header += next;
                if (header.size() >= 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0) break;
            }
            std::istringstream input(header);
            std::string method, uri, version, line;
            input >> method >> uri >> version;
            std::getline(input, line);
            ix::WebSocketHttpHeaders headers;
            bool valid = version == "HTTP/1.1" && line == "\r" && uri.size() <= 4096 &&
                header.size() < 16384 && header.size() >= 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0;
            while (valid && std::getline(input, line) && line != "\r") {
                const auto split = line.find(':');
                if (split == std::string::npos || line.back() != '\r') {
                    valid = false;
                    break;
                }
                auto value = line.substr(split + 1, line.size() - split - 2);
                const auto start = value.find_first_not_of(" \t");
                value = start == std::string::npos ? "" : value.substr(start);
                valid = headers.emplace(line.substr(0, split), std::move(value)).second;
            }
            valid = valid && headers.count("Host") && !headers.count("Transfer-Encoding") &&
                (!headers.count("Origin") || headers.at("Origin") == "https://" + headers.at("Host"));
            if (valid && method == "GET" && headers["Upgrade"] == "websocket" && senderToken(uri, owner_.runtime) &&
                !headers.count("Content-Length")) {
                handleUpgrade(std::move(socket), state, std::make_shared<ix::HttpRequest>(uri, method, version, "", headers));
            } else if (valid && method == "POST" && (uri == "/nearcast/code-display" || uri == "/nearcast/trust-display")) {
                socket->writeBytes(display(uri, headers, *socket, expired, state->getRemoteIp()), expired);
            } else if (valid && method == "GET" && uri == "/health") {
                socket->writeBytes(response("200 OK", {{"service", "nearcast-webcast"}, {"version", 1}}), expired);
            } else {
                socket->writeBytes(response("400 Bad Request", {{"status", "invalid_argument"}}), expired);
            }
            state->setTerminated();
        }

        std::string display(const std::string& uri, ix::WebSocketHttpHeaders& headers,
            ix::Socket& socket, const std::function<bool()>& expired, const std::string& ip) {
            const bool trust = uri == "/nearcast/trust-display";
            const auto result = [trust](const std::string& id, const char* status) {
                return trust ? MakeTrustDisplayResult(id, status) : MakeCodeDisplayResult(id, status);
            };
            const auto& value = headers["Content-Length"];
            std::size_t length = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
            if (length == 0 || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                headers["Content-Type"] != "application/json") {
                return response("400 Bad Request", result("", "invalid_argument"));
            }
            if (length > 1024) return response("413 Content Too Large", result("", "invalid_argument"));
            const auto body = socket.readBytes(length, [](int, int) { return true; }, nullptr, expired);
            if (!body.first) return response("400 Bad Request", result("", "invalid_argument"));
            const auto json = Json::parse(body.second, nullptr, false);
            std::string error;
            const auto request = trust ? ParseTrustDisplayRequest(json, owner_.device, error) :
                ParseCodeDisplayRequest(json, owner_.device, error);
            if (!request) return response("400 Bad Request", result("", "invalid_argument"));
            const char* status = "unavailable";
            if (!owner_.limiter.Allow(ip, nowMs())) {
                status = "rate_limited";
            } else if (trust ? owner_.showTrust(owner_.fingerprint) :
                (request->version == 1 || (request->runtimeInstanceId == owner_.runtime &&
                    request->backend == "local")) && owner_.showCode("")) {
                status = "shown";
            }
            auto reply = result(request->requestId, status);
            if (request->version == 2) {
                reply["version"] = 2;
                reply["backend"] = request->backend;
                reply["deviceId"] = owner_.device;
                reply["runtimeInstanceId"] = owner_.runtime;
            }
            return response("200 OK", reply);
        }

        Impl& owner_;
    };

    Impl(std::string id, std::string run, Receive input, Display code, Display trust)
        : device(std::move(id)), runtime(std::move(run)), receive(std::move(input)),
          showCode(std::move(code)), showTrust(std::move(trust)) {}

    void connection(std::weak_ptr<ix::WebSocket> weak, const std::shared_ptr<ix::ConnectionState>& state) {
        auto socket = weak.lock();
        if (!socket) return;
        const auto id = state->getId();
        socket->setOnMessageCallback([this, weak, id](const ix::WebSocketMessagePtr& message) {
            bool okay = true;
            switch (message->type) {
            case ix::WebSocketMessageType::Open: {
                const auto token = senderToken(message->openInfo.uri, runtime);
                if (token) {
                    std::lock_guard<std::mutex> lock(mutex);
                    sockets.emplace(id, weak);
                }
                okay = token && receive({LanEvent::Kind::Open, id, *token});
                break;
            }
            case ix::WebSocketMessageType::Message:
                okay = !message->binary && message->str.size() <= kSignalLimit &&
                    receive({LanEvent::Kind::Message, id, message->str});
                break;
            case ix::WebSocketMessageType::Close:
            case ix::WebSocketMessageType::Error: {
                std::lock_guard<std::mutex> lock(mutex);
                if (sockets.erase(id)) receive({LanEvent::Kind::Closed, id, {}});
                break;
            }
            default:
                break;
            }
            if (!okay) if (auto peer = weak.lock()) peer->close(1008, "Invalid local signaling");
        });
    }

    std::string device, runtime, fingerprint;
    Receive receive;
    Display showCode, showTrust;
    CodeDisplayRateLimiter limiter{3, 10000};
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<ix::WebSocket>> sockets;
    std::unique_ptr<Server> server;
};

LanServer::LanServer(std::string device, std::string runtime, Receive receive, Display showCode, Display showTrust)
    : impl_(std::make_unique<Impl>(std::move(device), std::move(runtime), std::move(receive),
        std::move(showCode), std::move(showTrust))) {}

LanServer::~LanServer() {
    if (impl_->server) impl_->server->stop();
}

std::uint16_t LanServer::start(const std::filesystem::path& directory) {
    auto& state = *impl_;
    state.fingerprint = makeIdentity(directory);
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        const auto port = ephemeralPort();
        if (!port) continue;
        auto server = std::make_unique<Impl::Server>(port, state);
        ix::SocketTLSOptions tls;
        tls.tls = true;
        tls.keyFile = (directory / "local-key.pem").string();
        tls.certFile = (directory / "local-cert.pem").string();
        tls.caFile = "NONE"; // Incoming sender connections are admitted by the room code.
        server->setTLSOptions(tls);
        server->setOnConnectionCallback([&state](auto socket, auto connection) {
            state.connection(std::move(socket), std::move(connection));
        });
        if (server->listen().first) {
            server->start();
            state.server = std::move(server);
            return port;
        }
    }
    return 0;
}

void LanServer::send(const LocalSignalDelivery& delivery) {
    std::shared_ptr<ix::WebSocket> socket;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto entry = impl_->sockets.find(delivery.connection);
        if (entry != impl_->sockets.end()) socket = entry->second.lock();
    }
    if (!socket) return;
    if (delivery.close || socket->bufferedAmount() > 2 * kSignalLimit) {
        socket->close(1008, "Local signaling closed");
    } else {
        socket->sendText(delivery.message.dump());
    }
}

} // namespace nearcast::android
