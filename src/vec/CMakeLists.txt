add_library(pc_vec STATIC
  vec.cpp
)
target_include_directories(pc_vec PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_vec PUBLIC
  
  pc_geom
  pc_color
  pc_raster
  pc_doc
  PkgConfig::FREETYPE
)
