LOCAL_PATH := $(call my-dir)
DEPS := $(LOCAL_PATH)/../../../../.deps
RX := $(DEPS)/sources/rx/src
UX := $(DEPS)/sources/uxplay/lib
IX := $(DEPS)/sources/ixwebsocket
JSON := $(DEPS)/sources/json/single_include
PLIST := $(DEPS)/libplist/libplist-2.6.0

GSTREAMER_ROOT := $(GSTREAMER_ROOT_ANDROID)/arm64
GSTREAMER_PLUGINS := coreelements app playback typefindfunctions \
    videoparsersbad videoconvertscale androidmedia openh264 libav opengl \
    audioconvert audioresample audioparsers opensles opus \
    rtp rtpmanager udp dtls srtp sctp webrtc nice
# Link OpenSSL's component packages directly so UxPlay and IX can call APIs
# that GStreamer's own TLS plugins do not reference.
GSTREAMER_EXTRA_DEPS := gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libssl libcrypto
# The SDK's separate shared-library link also needs Android's 16 KiB alignment.
GSTREAMER_EXTRA_LIBS := -Wl,-z,max-page-size=16384
GSTREAMER_INCLUDE_FONTS := no
GSTREAMER_INCLUDE_CA_CERTIFICATES := no
GSTREAMER_JAVA_SRC_DIR := $(DEPS)/gstreamer/generated

include $(CLEAR_VARS)
LOCAL_MODULE := nearcast_plist
LOCAL_C_INCLUDES := $(PLIST)/include $(PLIST)/src $(PLIST)/libcnary/include
LOCAL_CFLAGS := -DLIBPLIST_STATIC -DPACKAGE_VERSION=\"2.6.0\" -DHAVE_STRNDUP -DHAVE_MEMMEM \
    -DHAVE_LOCALTIME_R -DHAVE_GMTIME_R -DHAVE_TIMEGM -DHAVE_STRPTIME -DHAVE_TM_TM_GMTOFF -DHAVE_TM_TM_ZONE
LOCAL_SRC_FILES := $(wildcard $(PLIST)/src/*.c) $(PLIST)/libcnary/node.c $(PLIST)/libcnary/node_list.c
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := nearcast_airplay
LOCAL_C_INCLUDES := $(LOCAL_PATH) $(UX) $(UX)/playfair $(UX)/llhttp $(PLIST)/include $(GSTREAMER_ROOT)/include
LOCAL_CFLAGS := -DPLIST_230 -DOPENSSL_API_COMPAT=0x10101000L
LOCAL_SRC_FILES := $(wildcard $(UX)/*.c) $(wildcard $(UX)/playfair/*.c) $(wildcard $(UX)/llhttp/*.c)
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := nearcast_ix
LOCAL_CPPFLAGS := -DIXWEBSOCKET_USE_TLS -DIXWEBSOCKET_USE_OPEN_SSL
LOCAL_C_INCLUDES := $(IX) $(GSTREAMER_ROOT)/include
LOCAL_SRC_FILES := $(wildcard $(IX)/ixwebsocket/*.cpp)
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := nearcast_rx
LOCAL_CPP_FEATURES := exceptions
LOCAL_CPPFLAGS := -DGST_USE_UNSTABLE_API
LOCAL_C_INCLUDES := $(RX) $(UX) $(IX) $(JSON)
LOCAL_SRC_FILES := $(wildcard $(LOCAL_PATH)/*.cpp) \
    $(RX)/airplay/AirPlayProtocolService.cpp \
    $(RX)/webcast/GstWebRtcReceiver.cpp $(RX)/webcast/WebCastSignaling.cpp \
    $(RX)/webcast/LocalWebCastSignaling.cpp $(RX)/webcast/LocalWebCastDiscovery.cpp \
    $(RX)/media/gstreamer/GStreamerAudioCaps.cpp
LOCAL_STATIC_LIBRARIES := nearcast_airplay nearcast_plist nearcast_ix
LOCAL_SHARED_LIBRARIES := gstreamer_android
LOCAL_LDLIBS := -landroid -llog
include $(BUILD_SHARED_LIBRARY)

include $(GSTREAMER_ROOT)/share/gst-android/ndk-build/gstreamer-1.0.mk
