cmake_minimum_required(VERSION 3.25)
project(photoc VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)

if(WIN32)
  enable_language(RC)
endif()

include(FetchContent)

# Windows has none of the libraries, so they are linked into the programs, which then need only
# the DLLs that come with Windows. Set before the libraries are looked up below.
if(WIN32)
  option(PHOTOC_STATIC "Link the libraries into the programs instead of shipping DLLs beside them" ON)
endif()
if(PHOTOC_STATIC)
  # The .a archives, not the .dll.a import libraries
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
  # pkg-config then also lists each library's own dependencies, and flags such as
  # FRIBIDI_LIB_STATIC that headers need to declare the functions as static instead of DLL imports
  set(PKG_CONFIG_ARGN --static)
  # libstdc++, libgcc, libgomp and winpthreads too
  add_link_options(-static)
  # libintl (glib's, through harfbuzz) uses iconv but has no pkg-config file to say so; the
  # standard libraries go last on the link line, after everything that needs them
  string(APPEND CMAKE_C_STANDARD_LIBRARIES " -liconv")
  string(APPEND CMAKE_CXX_STANDARD_LIBRARIES " -liconv")
endif()

find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(OpenMP REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(GIF REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenGL REQUIRED)

# The dev container installs SDL3 and Dear ImGui; elsewhere they are fetched at the same versions
find_package(SDL3 QUIET CONFIG COMPONENTS SDL3-static)
if(NOT SDL3_FOUND)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL3)
endif()

pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2)
pkg_check_modules(TIFF REQUIRED IMPORTED_TARGET libtiff-4)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(FRIBIDI REQUIRED IMPORTED_TARGET fribidi)
pkg_check_modules(UNIBREAK REQUIRED IMPORTED_TARGET libunibreak)

set(IMGUI_DIR /opt/imgui CACHE PATH "Dear ImGui sources; fetched when missing")
if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b-docking
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(imgui)
  set(IMGUI_DIR ${imgui_SOURCE_DIR})
endif()
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

# Windows programs get a manifest that sets the process code page to UTF-8, so fopen and the
# ANSI Win32 calls take the UTF-8 paths SDL and the command line hand them (needs the UCRT runtime),
# and the icon Explorer and the taskbar show for the .exe file (windows/photoc.ico, made from
# assets/photoc-icon.jpg with photoc-cli)
function(pc_windows_manifest target)
  if(WIN32)
    set(PC_MANIFEST ${PROJECT_SOURCE_DIR}/windows/photoc.manifest)
    set(PC_ICON ${PROJECT_SOURCE_DIR}/windows/photoc.ico)
    set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}-manifest.rc)
    configure_file(${PROJECT_SOURCE_DIR}/windows/manifest.rc.in ${rc} @ONLY)
    target_sources(${target} PRIVATE ${rc})
    # The resource compiler doesn't report the files it embeds, so they are named here
    set_source_files_properties(${rc} PROPERTIES OBJECT_DEPENDS "${PC_MANIFEST};${PC_ICON}")
  endif()
endfunction()

add_subdirectory(src)
