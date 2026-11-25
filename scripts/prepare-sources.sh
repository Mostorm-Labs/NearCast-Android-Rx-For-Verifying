#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

rx=${NEARCAST_RX_ROOT:-"$PWD/../NearCast"}
rx_ref=c6e12701c9ac6069a263084468db703e66118dfb
uxplay_ref=6e11c373308cfc104e291ee867ba7c640fe64182
ix_ref=2efe037c9cc96fd536774f17bdb5215161ee5087
json_ref=d8ebaf61d79512e3feea44c69128dea82eff59d9

mkdir -p .deps/sources

extract_git() {
  local repo=$1 ref=$2 dest=$3
  shift 3
  if ! git -C "$repo" cat-file -e "$ref^{commit}" 2>/dev/null; then
    printf 'Missing %s in %s. Populate the pinned RX checkout and submodules.\n' "$ref" "$repo" >&2
    return 1
  fi
  mkdir -p "$dest"
  git -C "$repo" archive "$ref" "$@" | tar -xf - -C "$dest"
}

extract_git "$rx" "$rx_ref" .deps/sources/rx LICENSE THIRD_PARTY_NOTICES.md \
  src/airplay/AirPlayProtocolService.cpp src/airplay/AirPlayProtocolService.h \
  src/media/EncodedMedia.h src/media/gstreamer/GStreamerAudioCaps.cpp src/media/gstreamer/GStreamerAudioCaps.h \
  src/webcast/GstWebRtcReceiver.cpp src/webcast/GstWebRtcReceiver.h \
  src/webcast/LocalWebCastDiscovery.cpp src/webcast/LocalWebCastDiscovery.h \
  src/webcast/LocalWebCastSignaling.cpp src/webcast/LocalWebCastSignaling.h \
  src/webcast/WebCastSignaling.cpp src/webcast/WebCastSignaling.h src/webcast/WebCastSourceInfo.h
patch -s -d .deps/sources/rx -p1 < patches/rx-webcast-android.patch

extract_git "$rx/third_party/uxplay" "$uxplay_ref" .deps/sources/uxplay LICENSE lib
extract_git "$rx/third_party/axent/third_party/IXWebSocket" "$ix_ref" \
  .deps/sources/ixwebsocket LICENSE.txt ixwebsocket
extract_git "$rx/third_party/axent/third_party/axtp-cpp-runtime/third_party/json" "$json_ref" \
  .deps/sources/json LICENSE.MIT single_include
