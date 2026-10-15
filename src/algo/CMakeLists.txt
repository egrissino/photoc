add_library(pc_algo STATIC
  selection.cpp
  resample.cpp
  transform.cpp
  select_ops.cpp
  paint_fill.cpp
  retouch.cpp
  poisson.cpp
  maxflow.cpp
  quick_select.cpp
  matting.cpp
  quantize.cpp
  inpaint.cpp
  deform.cpp
)
target_include_directories(pc_algo PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_algo PUBLIC
  
  pc_geom
  pc_color
  pc_raster
  OpenMP::OpenMP_CXX
)
