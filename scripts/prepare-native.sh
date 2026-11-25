#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

download() {
  local url=$1 file=$2 hash=$3
  if [ ! -f "$file" ]; then
    mkdir -p "$(dirname "$file")"
    curl --fail --location --retry 2 --continue-at - --output "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  printf '%s  %s\n' "$hash" "$file" | shasum -a 256 -c -
}

scripts/prepare-sources.sh
plist=.deps/libplist/libplist-2.6.0.tar.gz
download 'https://codeload.github.com/libimobiledevice/libplist/tar.gz/refs/tags/2.6.0' "$plist" \
  e6491c2fa3370e556ac41b8705dd7f8f0e772c8f78641c3878cabd45bd84d950
tar -xzf "$plist" -C .deps/libplist

if [ -z "${GSTREAMER_ROOT_ANDROID:-}" ] && [ ! -f .deps/gstreamer/sdk/.verified-1.28.4 ]; then
  gst=gstreamer-1.0-android-universal-1.28.4.tar.xz
  download "https://gstreamer.freedesktop.org/pkg/android/1.28.4/$gst" ".deps/gstreamer/$gst" \
    a48aeb1b4fbae67a2fe0a0daa55af4a6af22b48f15a9c09f09dcd9ab3e8e943e
  mkdir -p .deps/gstreamer/sdk
  tar -xJf ".deps/gstreamer/$gst" -C .deps/gstreamer/sdk arm64
  touch .deps/gstreamer/sdk/.verified-1.28.4
fi
