add_library(pc_io STATIC
  io.cpp
  smart.cpp
)
target_include_directories(pc_io PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_io PUBLIC
  
  pc_geom
  pc_color
  pc_raster
  pc_doc
  pc_codecs
  pc_compose
  pc_psd
  pc_algo
)
