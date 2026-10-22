#!/bin/bash
set -eu

# TODO: Move folder destinations out of root owned areas and into temp or build folders

apt-get update
apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config nlohmann-json3-dev liblcms2-dev libpng-dev libjpeg-dev \
    git ca-certificates libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxkbcommon-dev libxtst-dev libgl-dev libegl-dev libdbus-1-dev \
    libfreetype-dev libharfbuzz-dev libfontconfig-dev libfribidi-dev libunibreak-dev libtiff-dev libwebp-dev libgif-dev \
    file desktop-file-utils

git clone --depth 1 --branch release-3.4.18 https://github.com/libsdl-org/SDL.git /tmp/SDL
cmake -S /tmp/SDL -B /tmp/SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_WAYLAND=OFF -DSDL_VULKAN=OFF -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build /tmp/SDL/build && cmake --install /tmp/SDL/build
rm -rf /tmp/SDL

git clone --depth 1 --branch v1.92.9b-docking https://github.com/ocornut/imgui.git /opt/imgui
