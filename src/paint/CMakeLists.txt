add_library(pc_paint STATIC
  paint.cpp
  retouch.cpp
)
target_include_directories(pc_paint PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_paint PUBLIC
  
  pc_geom
  pc_color
  pc_raster
)
