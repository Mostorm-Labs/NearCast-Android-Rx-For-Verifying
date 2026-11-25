#pragma once

#include "media/EncodedMedia.h"

#include <android/native_window.h>
#include <gst/gst.h>

namespace nearcast::android {

using Status = std::function<void(const char*, const std::string&)>;

// One Android surface and one audio output, on the receiver's serial worker.
class SurfacePlayback {
public:
    SurfacePlayback(ANativeWindow* window, Status status);
    ~SurfacePlayback();
    bool ready() const;
    void push(const media::EncodedMediaDescriptor& descriptor, const media::EncodedMediaChunk& chunk);
    void closeTrack(media::EncodedMediaKind kind, std::uint64_t generation);
    void poll();
    void reset();

private:
    struct Branch {
        GstElement* bin = nullptr;
        GstElement* source = nullptr;
        std::uint64_t generation = 0;
        std::string codec;
    };
    Branch* attach(const media::EncodedMediaDescriptor& descriptor);
    void detach(Branch& branch);
    GstClockTime playoutTime(const media::EncodedMediaChunk& chunk);

    ANativeWindow* window_;
    Status status_;
    GstElement* pipeline_ = nullptr;
    Branch video_;
    Branch audio_;
    GstClockTime sourceStart_ = GST_CLOCK_TIME_NONE;
    GstClockTime runningStart_ = GST_CLOCK_TIME_NONE;
    bool failed_ = false;
};

} // namespace nearcast::android
