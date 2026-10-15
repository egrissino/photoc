#!/bin/bash
# Builds PhotoC inside the dev container.
#   ./build.sh                  build for this machine's architecture
#   ./build.sh package          also make the .deb and the AppImage
#   ARCH=aarch64 ./build.sh     build for another architecture (x86_64 or aarch64); on a
#                               different host this needs qemu-user-static for emulation
set -eu

cd "$(dirname "$0")"

IMAGE=photoc-dev
PACKAGE_IMAGE=photoc-package
BUILD_DIR=build

docker build \
  --target dev \
  -t "$IMAGE" .

docker run --rm \
  --user "$(id -u):$(id -g)" \
  --volume "$PWD:/src" \
  --workdir /src \
  "$IMAGE" \
  sh -c "cmake -S . -B $BUILD_DIR -G Ninja && cmake --build $BUILD_DIR"

if [ -e "./$BUILD_DIR/src/apps/photoc/photoc" ]; then
  mkdir -p "./$BUILD_DIR/apps/"
  cp "./$BUILD_DIR/src/apps/photoc/photoc" "./$BUILD_DIR/apps"
fi

if [ -e "./$BUILD_DIR/src/apps/photoc-cli/photoc-cli" ]; then
  mkdir -p "./$BUILD_DIR/apps/"
  cp "./$BUILD_DIR/src/apps/photoc-cli/photoc-cli" "./$BUILD_DIR/apps"
fi

docker build \
  --target local \
  -t photoc:local .