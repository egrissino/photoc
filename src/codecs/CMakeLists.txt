add_library(pc_codecs STATIC
  image.cpp
  codecs.cpp
  pnm.cpp
  png.cpp
  jpeg.cpp
  tiff.cpp
  webp.cpp
  gif.cpp
  bmp.cpp
  tga.cpp
  qoi.cpp
  hdr.cpp
  pcx.cpp
)
target_include_directories(pc_codecs PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_codecs PUBLIC
  
  PNG::PNG
  JPEG::JPEG
  PkgConfig::TIFF
  PkgConfig::WEBP
  GIF::GIF
)
