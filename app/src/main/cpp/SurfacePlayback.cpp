#include "SurfacePlayback.h"
#include "media/gstreamer/GStreamerAudioCaps.h"

#include <algorithm>
#include <android/log.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/videooverlay.h>

namespace nearcast::android {
namespace {
constexpr GstClockTime kLead = 100 * GST_MSECOND;

GstCaps* audioCaps(const media::EncodedMediaDescriptor& descriptor) {
    const auto spec = media::gstreamer::ResolveAudioInputCaps(descriptor);
    if (!spec.supported) return nullptr;
    auto* caps = gst_caps_new_simple(spec.mediaType.c_str(), "rate", G_TYPE_INT,
        static_cast<int>(spec.sampleRate), "channels", G_TYPE_INT, static_cast<int>(spec.channels), nullptr);
    if (spec.mpegVersion) gst_caps_set_simple(caps, "mpegversion", G_TYPE_INT, spec.mpegVersion, nullptr);
    if (!spec.pcmFormat.empty()) gst_caps_set_simple(caps, "format", G_TYPE_STRING,
        spec.pcmFormat.c_str(), "layout", G_TYPE_STRING, "interleaved", nullptr);
    if (!spec.streamFormat.empty() && spec.mpegVersion) gst_caps_set_simple(caps, "stream-format",
        G_TYPE_STRING, spec.streamFormat.c_str(), nullptr);
    if (!spec.codecData.empty()) {
        auto* bytes = gst_buffer_new_allocate(nullptr, spec.codecData.size(), nullptr);
        gst_buffer_fill(bytes, 0, spec.codecData.data(), spec.codecData.size());
        gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, bytes, nullptr);
        gst_buffer_unref(bytes);
    }
    return caps;
}
} // namespace

SurfacePlayback::SurfacePlayback(ANativeWindow* window, Status status)
    : window_(window), status_(std::move(status)) {}

SurfacePlayback::~SurfacePlayback() { reset(); }

bool SurfacePlayback::ready() const {
    for (const auto* name : {"appsrc", "decodebin", "h264parse", "glimagesink", "openslessink"}) {
        auto* factory = gst_element_factory_find(name);
        if (!factory) {
            status_("error", std::string("缺少 GStreamer 元件 ") + name);
            return false;
        }
        gst_object_unref(factory);
    }
    return true;
}

SurfacePlayback::Branch* SurfacePlayback::attach(const media::EncodedMediaDescriptor& descriptor) {
    const bool video = descriptor.kind == media::EncodedMediaKind::Video;
    auto& branch = video ? video_ : audio_;
    if (branch.bin && branch.generation == descriptor.key.generation && branch.codec == descriptor.codec) {
        return &branch;
    }
    detach(branch);
    const std::string src = "appsrc name=frames is-live=true format=time block=false "
        "max-buffers=24 max-bytes=4194304 leaky-type=downstream ! ";
    std::string launch;
    GstCaps* caps = nullptr;
    if (video) {
        const auto contract = media::ResolveVideoStreamContract(descriptor);
        if (!contract) return nullptr;
        const bool hevc = contract->codec == media::EncodedVideoCodec::H265;
        caps = gst_caps_new_simple(hevc ? "video/x-h265" : "video/x-h264",
            "stream-format", G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au", nullptr);
        launch = src + (hevc ? "h265parse" : "h264parse") +
            " config-interval=-1 ! decodebin ! glimagesink name=display async=false force-aspect-ratio=true";
    } else {
        caps = audioCaps(descriptor);
        if (!caps) return nullptr;
        launch = src + (descriptor.codec == "pcm" ? "" : "decodebin ! ") +
            "audioconvert ! audioresample ! openslessink name=display async=false";
    }
    GError* error = nullptr;
    auto* bin = gst_parse_bin_from_description(launch.c_str(), FALSE, &error);
    if (!bin || error) {
        if (error) {
            __android_log_print(ANDROID_LOG_ERROR, "NearCastRx", "Pipeline setup: %s", error->message);
            g_error_free(error);
        }
        if (bin) gst_object_unref(bin);
        gst_caps_unref(caps);
        status_("error", "无法创建 GStreamer 播放链路");
        return nullptr;
    }
    auto* source = gst_bin_get_by_name(GST_BIN(bin), "frames");
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    auto* sink = gst_bin_get_by_name(GST_BIN(bin), "display");
    if (video) gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), reinterpret_cast<guintptr>(window_));
    // Signal media only when a buffer reached the actual rendering branch.
    auto* pad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, +[](GstPad* pad, GstPadProbeInfo*, gpointer user) {
        auto& self = *static_cast<SurfacePlayback*>(user);
        self.status_("media", GST_PAD_PARENT(pad) &&
            GST_IS_VIDEO_OVERLAY(GST_PAD_PARENT(pad)) ? "video" : "audio");
        return GST_PAD_PROBE_REMOVE;
    }, this, nullptr);
    gst_object_unref(pad);
    gst_object_unref(sink);
    if (!pipeline_) pipeline_ = gst_pipeline_new(nullptr);
    gst_bin_add(GST_BIN(pipeline_), bin);
    branch = {bin, source, descriptor.key.generation, descriptor.codec};
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
        !gst_element_sync_state_with_parent(bin)) {
        detach(branch);
        status_("error", "GStreamer 播放链路启动失败");
        return nullptr;
    }
    return &branch;
}

GstClockTime SurfacePlayback::playoutTime(const media::EncodedMediaChunk& chunk) {
    const auto now = gst_element_get_current_running_time(pipeline_);
    if (!GST_CLOCK_TIME_IS_VALID(now)) return GST_CLOCK_TIME_NONE;
    const auto lead = kLead - std::min(kLead, chunk.recoveredPlayoutAgeValid ? chunk.recoveredPlayoutAgeNs : 0);
    if (!chunk.recoveredTimestampValid) return now + lead;
    const auto source = chunk.recoveredTimestampNs;
    if (!GST_CLOCK_TIME_IS_VALID(sourceStart_)) {
        sourceStart_ = source;
        runningStart_ = now + lead;
    }
    auto pts = source >= sourceStart_ ? runningStart_ + (source - sourceStart_) :
        runningStart_ - std::min(runningStart_, sourceStart_ - source);
    if ((pts > now && pts - now > 2 * GST_SECOND) || (now > pts && now - pts > 2 * GST_SECOND)) {
        runningStart_ = pts = now + lead;
        sourceStart_ = source;
    }
    return pts;
}

void SurfacePlayback::push(const media::EncodedMediaDescriptor& descriptor, const media::EncodedMediaChunk& chunk) {
    if (failed_ || chunk.data.empty()) return;
    auto* branch = attach(descriptor);
    if (!branch) {
        failed_ = true;
        return;
    }
    auto* buffer = gst_buffer_new_allocate(nullptr, chunk.data.size(), nullptr);
    if (!buffer) return;
    gst_buffer_fill(buffer, 0, chunk.data.data(), chunk.data.size());
    GST_BUFFER_PTS(buffer) = playoutTime(chunk);
    if (media::hasFlag(chunk.flags, media::EncodedMediaChunkFlag::Discontinuity)) {
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
    }
    if (descriptor.kind == media::EncodedMediaKind::Audio && descriptor.samplesPerFrame && descriptor.sampleRate) {
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(descriptor.samplesPerFrame, GST_SECOND, descriptor.sampleRate);
    }
    gst_app_src_push_buffer(GST_APP_SRC(branch->source), buffer);
}

void SurfacePlayback::detach(Branch& branch) {
    if (!branch.bin) return;
    gst_element_set_state(branch.bin, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(pipeline_), branch.bin);
    gst_object_unref(branch.source);
    branch = {};
}

void SurfacePlayback::closeTrack(media::EncodedMediaKind kind, std::uint64_t generation) {
    auto& branch = kind == media::EncodedMediaKind::Video ? video_ : audio_;
    if (branch.generation == generation) detach(branch);
}

void SurfacePlayback::poll() {
    if (!pipeline_ || failed_) return;
    auto* bus = gst_element_get_bus(pipeline_);
    auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (message) {
        GError* error = nullptr;
        gst_message_parse_error(message, &error, nullptr);
        __android_log_print(ANDROID_LOG_ERROR, "NearCastRx", "Playback: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        gst_message_unref(message);
        failed_ = true;
        status_("error", "媒体解码或渲染失败");
    }
}

void SurfacePlayback::reset() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        detach(video_);
        detach(audio_);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    sourceStart_ = runningStart_ = GST_CLOCK_TIME_NONE;
    failed_ = false;
}

} // namespace nearcast::android
