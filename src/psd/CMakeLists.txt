add_library(pc_psd STATIC
  bytes.cpp
  descriptor.cpp
  engine_data.cpp
  effects.cpp
  type.cpp
  adjust_map.cpp
  vector.cpp
  fill.cpp
  smart.cpp
  psd.cpp
)
target_include_directories(pc_psd PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_psd PUBLIC
  
  pc_geom
  pc_color
  pc_raster
  pc_doc
  pc_compose
  pc_text
  pc_vec
  ZLIB::ZLIB
)
