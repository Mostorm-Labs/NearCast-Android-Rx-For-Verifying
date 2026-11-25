#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."

scripts/prepare-sources.sh
output=.deps/lan-room-test
"${CXX:-c++}" -std=c++17 -pthread -Iapp/src/main/cpp -I.deps/sources/rx/src \
  -I.deps/sources/json/single_include tests/lan_room_test.cpp app/src/main/cpp/LanRoom.cpp \
  .deps/sources/rx/src/webcast/{WebCastSignaling,LocalWebCastSignaling}.cpp -o "$output"
"$output"
"${CXX:-c++}" -std=c++17 -Iapp/src/main/cpp tests/dns_sd_test.cpp app/src/main/cpp/DnsSd.cpp -o .deps/dns-sd-test
.deps/dns-sd-test
printf 'LAN signaling and DNS-SD tests passed.\n'
