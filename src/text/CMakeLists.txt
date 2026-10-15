add_library(pc_text STATIC
  fonts.cpp
  layout.cpp
  render.cpp
)
target_include_directories(pc_text PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_text PUBLIC
  
  pc_geom
  pc_color
  pc_raster
  pc_doc
  PkgConfig::FREETYPE
  PkgConfig::HARFBUZZ
  PkgConfig::FONTCONFIG
  PkgConfig::FRIBIDI
  PkgConfig::UNIBREAK
)
