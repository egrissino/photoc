cmake_minimum_required(VERSION 3.25)
project(photoc VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)

find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(OpenMP REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(GIF REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenGL REQUIRED)
find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-static)

pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2)
pkg_check_modules(TIFF REQUIRED IMPORTED_TARGET libtiff-4)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(FRIBIDI REQUIRED IMPORTED_TARGET fribidi)
pkg_check_modules(UNIBREAK REQUIRED IMPORTED_TARGET libunibreak)

set(IMGUI_DIR /opt/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static OpenGL::GL)

add_subdirectory(src)
