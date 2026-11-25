#include "LanRoom.h"
#include "LanServer.h"
#include "ServiceRecord.h"
#include "SurfacePlayback.h"
#include "airplay/AirPlayProtocolService.h"
#include "webcast/GstWebRtcReceiver.h"
#include "webcast/LocalWebCastDiscovery.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

namespace nearcast::android {
namespace {
using namespace airplay;
using namespace media;
using namespace webcast;
using namespace std::chrono_literals;

std::uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class AttachedEnv {
public:
    explicit AttachedEnv(JavaVM* vm) : vm_(vm) {
        if (vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_EDETACHED) {
            attached_ = vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK;
        }
    }
    ~AttachedEnv() { if (attached_) vm_->DetachCurrentThread(); }
    JNIEnv* get() const { return env_; }

private:
    JavaVM* vm_;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

EncodedMediaDescriptor airPlayAudio(const ProtocolAudioFormat& format) {
    // Wire caps follow AirPlayAudioDescriptor in the Windows RX provider.
    EncodedMediaDescriptor descriptor;
    descriptor.kind = EncodedMediaKind::Audio;
    descriptor.sampleRate = format.sampleRate;
    descriptor.channels = 2;
    descriptor.samplesPerFrame = format.samplesPerFrame;
    switch (format.compressionType) {
    case 1:
        descriptor.codec = "pcm";
        descriptor.transportFormat = "s16le";
        break;
    case 2:
        if (format.samplesPerFrame != 352) break;
        descriptor.codec = "alac";
        descriptor.transportFormat = "raw";
        descriptor.codecData = {0,0,0,0x24,0x61,0x6c,0x61,0x63,0,0,0,0,0,0,1,0x60,
            0,0x10,0x28,0x0a,0x0e,2,0,0xff,0,0,0,0,0,0,0,0,0,0,0xac,0x44};
        break;
    case 4:
        if (format.samplesPerFrame != 1024) break;
        descriptor.codec = "aac";
        descriptor.transportFormat = "adts";
        break;
    case 8:
        if (format.samplesPerFrame != 480) break;
        descriptor.codec = "aac";
        descriptor.transportFormat = "raw";
        descriptor.codecData = {0xf8,0xe8,0x50,0};
        break;
    default:
        break;
    }
    return descriptor;
}

class AirPlayInput final : public ProtocolObserver {
public:
    explicit AirPlayInput(Status status) : status_(std::move(status)) {}

    bool admit(const ProtocolSession& session) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_.id) return false;
        active_.id = session.id;
        active_.generation = session.generation;
        return true;
    }

    void closed(const ProtocolSession& session) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sameSession(session)) return;
        active_.id = 0;
        frames_.clear();
        bytes_ = 0;
        reset_ = true;
    }

    void passwordRequired(const ProtocolPasswordChallenge& challenge) noexcept override {
        try {
            status_("pin", challengeId(challenge.candidateId, challenge.challengeId) + ":" + challenge.code.data());
        } catch (...) {}
    }

    void passwordDismissed(std::uint64_t candidate, std::uint64_t challenge) noexcept override {
        try { status_("pinDismissed", challengeId(candidate, challenge)); } catch (...) {}
    }

    void onVideo(const ProtocolSession& session, const ProtocolVideoFormat& format,
        const ProtocolMediaUnit& unit) noexcept override {
        try {
            if (format.codec == ProtocolVideoFormat::Codec::Unknown) return;
            EncodedMediaDescriptor descriptor;
            descriptor.kind = EncodedMediaKind::Video;
            descriptor.codec = format.codec == ProtocolVideoFormat::Codec::H265 ? "h265" : "h264";
            push(session, std::move(descriptor), unit);
        } catch (...) {}
    }

    void onAudio(const ProtocolSession& session, const ProtocolAudioFormat& format,
        const ProtocolMediaUnit& unit) noexcept override {
        try {
            auto descriptor = airPlayAudio(format);
            if (!descriptor.codec.empty()) push(session, std::move(descriptor), unit);
        } catch (...) {}
    }

    void discontinuity(const ProtocolSession& session, const char*, int) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sameSession(session)) return;
        frames_.clear();
        bytes_ = 0;
        gap_ = true;
    }

    void poll(SurfacePlayback& playback, AirPlayProtocolService& service, bool rendered) {
        std::deque<Frame> frames;
        std::uint64_t id, generation;
        bool reset;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frames.swap(frames_);
            bytes_ = 0;
            reset = std::exchange(reset_, false);
            id = active_.id;
            generation = active_.generation;
        }
        if (reset) {
            playback.reset();
            status_("waiting", "");
        } else if (rendered && id) {
            service.presentationStarted(id, generation);
        }
        for (const auto& frame : frames) playback.push(frame.descriptor, frame.chunk);
    }

private:
    struct Frame { EncodedMediaDescriptor descriptor; EncodedMediaChunk chunk; };

    static std::string challengeId(std::uint64_t candidate, std::uint64_t challenge) {
        return std::to_string(candidate) + "/" + std::to_string(challenge);
    }

    bool sameSession(const ProtocolSession& session) const {
        return active_.id == session.id && active_.generation == session.generation;
    }

    void push(const ProtocolSession& session, EncodedMediaDescriptor descriptor, const ProtocolMediaUnit& unit) {
        constexpr std::size_t limit = 8 * 1024 * 1024;
        if (!unit.data || !unit.size || unit.size > limit) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sameSession(session)) return;
        while (frames_.size() >= 64 || bytes_ + unit.size > limit) {
            bytes_ -= frames_.front().chunk.data.size();
            frames_.pop_front();
            gap_ = true;
        }
        descriptor.key.generation = session.generation;
        descriptor.timingBasis = MediaTimingBasis::RecoveredLocalNtp;
        EncodedMediaChunk chunk;
        chunk.recoveredTimestampNs = unit.recoveredLocalNtpNs;
        chunk.recoveredTimestampValid = unit.recoveredTimeValid;
        if (std::exchange(gap_, false)) chunk.flags = EncodedMediaChunkFlag::Discontinuity;
        chunk.data.assign(unit.data, unit.data + unit.size);
        frames_.push_back({std::move(descriptor), std::move(chunk)});
        bytes_ += unit.size;
    }

    Status status_;
    std::mutex mutex_;
    ProtocolSession active_;
    std::deque<Frame> frames_;
    std::size_t bytes_ = 0;
    bool reset_ = false;
    bool gap_ = false;
};

class WebCastInput {
public:
    WebCastInput(std::string name, std::string device, SurfacePlayback& playback, Status status)
        : name_(std::move(name)), device_(std::move(device)), runtime_(randomHex(16)),
          playback_(playback), status_(std::move(status)),
          server_(device_, runtime_, [this](LanEvent event) { return enqueue(std::move(event)); },
              [this](const std::string&) {
                  if (!ready_.load()) return false;
                  status_("showCode", "");
                  return true;
              },
              [this](const std::string& fingerprint) {
                  if (!ready_.load()) return false;
                  status_("fingerprint", fingerprint);
                  return true;
              }),
          room_(name_, randomRoomValue, [this](const LocalSignalDelivery& delivery) { server_.send(delivery); }) {}

    std::optional<ServiceRecord> start(const std::filesystem::path& directory, const std::string& host) {
        const auto missing = GstWebRtcReceiver::RuntimeFailure();
        if (!missing.empty()) {
            status_("error", missing);
            return {};
        }
        const auto port = server_.start(directory);
        if (!port) {
            status_("error", "WebCast HTTPS 监听失败");
            return {};
        }
        apply(room_.start(nowMs()));
        if (room_.receiver().Snapshot().state != SignalingState::Ready) {
            status_("error", "WebCast 房间创建失败");
            return {};
        }
        auto txt = BuildNearCastServiceTxt(device_, name_, port, true, false, host, true);
        if (txt.empty()) return {};
        txt["capability"] += ",signaling-select-v1,code-display-v2";
        txt["runtime"] = runtime_;
        txt["status-rev"] = "1";
        txt["local-state"] = "ready";
        txt["cloud-state"] = "unavailable";
        ServiceRecord service{NearCastMdnsService, name_, port, {}};
        for (const auto& [key, value] : txt) service.txt.push_back(key + "=" + value);
        ready_ = true;
        return service;
    }

    void poll(bool disconnect) {
        std::deque<LanEvent> incoming;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            incoming.swap(incoming_);
            incomingBytes_ = 0;
        }
        const auto now = nowMs();
        if (disconnect && rtc_) apply(room_.receiver().Retire(peer_));
        for (const auto& event : incoming) {
            switch (event.kind) {
            case LanEvent::Kind::Open: apply(room_.open(event.connection, event.text, now)); break;
            case LanEvent::Kind::Message: apply(room_.receive(event.connection, event.text, now)); break;
            case LanEvent::Kind::Closed: apply(room_.close(event.connection, now)); break;
            }
        }
        apply(room_.tick(now));
        if (!rtc_) return;
        const auto key = peer_;
        for (const auto& event : rtc_->Poll()) {
            if (!rtc_ || !(peer_ == key)) break;
            switch (event.kind) {
            case RtcEventKind::Signal:
                apply(room_.receiver().SendSignal(key, event.type, event.payload));
                break;
            case RtcEventKind::ChannelOpened: room_.receiver().ChannelState(key, true); break;
            case RtcEventKind::ChannelClosed: room_.receiver().ChannelState(key, false); break;
            case RtcEventKind::Data: apply(room_.receiver().ReceiveData(key, event.type)); break;
            case RtcEventKind::Media:
                if (rtc_->AcceptsMedia(event.frame)) playback_.push(event.frame.descriptor, event.frame.chunk);
                break;
            case RtcEventKind::TrackClosed:
                playback_.closeTrack(event.frame.descriptor.kind, event.frame.descriptor.key.generation);
                break;
            case RtcEventKind::Fault:
                apply(room_.receiver().Retire(key));
                status_("error", event.type);
                break;
            default:
                break;
            }
        }
    }

private:
    bool enqueue(LanEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (incoming_.size() >= 128 || incomingBytes_ + event.text.size() > 4 * 1024 * 1024) return false;
        const auto size = event.text.size();
        incoming_.push_back(std::move(event));
        incomingBytes_ += size;
        return true;
    }

    void apply(Effects effects) {
        for (const auto& effect : room_.route(std::move(effects), nowMs())) {
            const bool current = rtc_ && effect.peer == peer_;
            switch (effect.kind) {
            case EffectKind::PeerOpened:
                peer_ = effect.peer;
                rtc_ = std::make_unique<GstWebRtcReceiver>();
                if (!rtc_->Start("")) {
                    apply(room_.receiver().Retire(peer_));
                    status_("error", "WebRTC 会话启动失败");
                } else status_("connecting", "");
                break;
            case EffectKind::PeerClosed:
                if (current) {
                    rtc_.reset();
                    playback_.reset();
                    status_("waiting", "");
                }
                break;
            case EffectKind::RemoteSignal:
                if (current && !rtc_->ReceiveSignal(effect.payload["type"], effect.payload["payload"])) {
                    apply(room_.receiver().Retire(peer_));
                    status_("error", "WebRTC 协商失败");
                }
                break;
            case EffectKind::DataSend:
                if (current && !rtc_->SendData(effect.payload.dump())) apply(room_.receiver().DataSendFailed(effect));
                break;
            case EffectKind::ShareState:
                if (current) {
                    const bool sharing = effect.payload.at("sharing").get<bool>();
                    rtc_->SetSharing(sharing);
                    if (!sharing) {
                        playback_.reset();
                        status_("waiting", "");
                    }
                }
                break;
            case EffectKind::CodeChanged:
                status_("code", room_.receiver().PrivatePairCode());
                break;
            default:
                break;
            }
        }
    }

    std::string name_, device_, runtime_;
    SurfacePlayback& playback_;
    Status status_;
    std::atomic<bool> ready_ = false;
    std::mutex mutex_;
    std::deque<LanEvent> incoming_;
    std::size_t incomingBytes_ = 0;
    LanServer server_;
    LanRoom room_;
    PeerKey peer_;
    std::unique_ptr<GstWebRtcReceiver> rtc_;
};

class Session {
public:
    Session(JNIEnv* env, jobject activity, ANativeWindow* window, jlong generation, bool web,
        std::string directory, std::string name, std::string id, std::string airPlayId, std::string host)
        : window_(window), generation_(generation), web_(web), directory_(std::move(directory)),
          name_(std::move(name)), id_(std::move(id)), airPlayId_(std::move(airPlayId)), host_(std::move(host)) {
        env->GetJavaVM(&vm_);
        auto type = env->GetObjectClass(activity);
        event_ = env->GetMethodID(type, "onNativeEvent", "(JLjava/lang/String;Ljava/lang/String;)V");
        service_ = env->GetMethodID(type, "onNativeService", "(JLjava/lang/String;Ljava/lang/String;I[Ljava/lang/String;)V");
        env->DeleteLocalRef(type);
        activity_ = env->NewGlobalRef(activity);
    }

    ~Session() {
        stopping_ = true;
        if (worker_.joinable()) worker_.join();
        AttachedEnv env(vm_);
        if (env.get() && activity_) env.get()->DeleteGlobalRef(activity_);
        ANativeWindow_release(window_);
    }

    void start() { worker_ = std::thread(&Session::run, this); }
    void disconnect() { disconnect_ = true; }

private:
    void emit(const char* kind, const std::string& value) {
        if (stopping_) return;
        AttachedEnv attached(vm_);
        auto* env = attached.get();
        if (!env || env->PushLocalFrame(4) != JNI_OK) return;
        auto event = env->NewStringUTF(kind);
        auto data = env->NewStringUTF(value.c_str());
        if (event && data) env->CallVoidMethod(activity_, event_, generation_, event, data);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->PopLocalFrame(nullptr);
    }

    void publish(const ServiceRecord& record) {
        if (stopping_) return;
        AttachedEnv attached(vm_);
        auto* env = attached.get();
        if (!env || env->PushLocalFrame(8) != JNI_OK) return;
        auto type = env->NewStringUTF(record.type.c_str());
        auto name = env->NewStringUTF(record.name.c_str());
        auto strings = env->FindClass("java/lang/String");
        auto txt = env->NewObjectArray(static_cast<jsize>(record.txt.size()), strings, nullptr);
        for (std::size_t i = 0; i < record.txt.size(); ++i) {
            auto entry = env->NewStringUTF(record.txt[i].c_str());
            env->SetObjectArrayElement(txt, static_cast<jsize>(i), entry);
            env->DeleteLocalRef(entry);
        }
        env->CallVoidMethod(activity_, service_, generation_, type, name, static_cast<jint>(record.port), txt);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->PopLocalFrame(nullptr);
    }

    void run() {
        try {
            Status status = [this](const char* kind, const std::string& value) {
                if (std::string_view(kind) == "media" && value == "video") rendered_ = true;
                emit(kind, value);
            };
            SurfacePlayback playback(window_, status);
            if (!playback.ready()) return;
            if (web_) {
                WebCastInput web(name_, "nearcast-" + id_, playback, status);
                auto record = web.start(std::filesystem::path(directory_) / "webcast", host_);
                if (!record) return;
                publish(*record);
                while (!stopping_) {
                    web.poll(disconnect_.exchange(false));
                    playback.poll();
                    std::this_thread::sleep_for(10ms);
                }
            } else {
                AirPlayInput input(status);
                AirPlayProtocolService service(input);
                AirPlayProtocolService::Config config;
                config.name = name_;
                config.receiverDeviceId = airPlayId_;
                config.keyFile = (std::filesystem::path(directory_) / "airplay.pem").string();
                config.advertise = true;
                config.randomPassword = true;
                std::vector<ServiceRecord> records;
                std::string error;
                {
                    CaptureDnsSdRecords capture(records);
                    if (!service.start(config, error)) {
                        emit("error", error);
                        return;
                    }
                }
                if (records.size() != 2) {
                    emit("error", "AirPlay 发现记录不完整");
                    return;
                }
                for (const auto& record : records) publish(record);
                while (!stopping_) {
                    if (disconnect_.exchange(false)) service.requestDisconnectAll();
                    service.poll();
                    input.poll(playback, service, rendered_.exchange(false));
                    playback.poll();
                    std::this_thread::sleep_for(10ms);
                }
            }
        } catch (const std::exception& error) {
            __android_log_print(ANDROID_LOG_ERROR, "NearCastRx", "%s", error.what());
            emit("error", "接收服务异常退出");
        }
    }

    JavaVM* vm_ = nullptr;
    jobject activity_ = nullptr;
    jmethodID event_ = nullptr, service_ = nullptr;
    ANativeWindow* window_;
    jlong generation_;
    bool web_;
    std::string directory_, name_, id_, airPlayId_, host_;
    std::atomic<bool> stopping_ = false, disconnect_ = false, rendered_ = false;
    std::thread worker_;
};

std::string fromJava(JNIEnv* env, jstring text) {
    const auto* value = env->GetStringUTFChars(text, nullptr);
    if (!value) throw std::runtime_error("Invalid native receiver argument");
    std::string result(value);
    env->ReleaseStringUTFChars(text, value);
    return result;
}
} // namespace
} // namespace nearcast::android

extern "C" JNIEXPORT jlong JNICALL
Java_com_mostorm_nearcast_rx_ReceiverActivity_nativeStart(JNIEnv* env, jobject activity, jobject surface,
    jlong generation, jboolean web, jstring directory, jstring name, jstring id, jstring airPlayId, jstring host) {
    using namespace nearcast::android;
    auto* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return 0;
    try {
        auto session = std::make_unique<Session>(env, activity, window, generation, web,
            fromJava(env, directory), fromJava(env, name), fromJava(env, id), fromJava(env, airPlayId), fromJava(env, host));
        window = nullptr; // Session owns the acquired reference.
        session->start();
        return reinterpret_cast<jlong>(session.release());
    } catch (...) {
        if (window) ANativeWindow_release(window);
        return 0;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mostorm_nearcast_rx_ReceiverActivity_nativeStop(JNIEnv*, jobject, jlong handle) {
    delete reinterpret_cast<nearcast::android::Session*>(handle);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mostorm_nearcast_rx_ReceiverActivity_nativeDisconnect(JNIEnv*, jobject, jlong handle) {
    if (handle) reinterpret_cast<nearcast::android::Session*>(handle)->disconnect();
}
