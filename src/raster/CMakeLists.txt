add_library(pc_raster STATIC
  raster.cpp
  interrupt.cpp
)
target_include_directories(pc_raster PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_raster PUBLIC
  
  pc_geom
  pc_color
  OpenMP::OpenMP_CXX
)
