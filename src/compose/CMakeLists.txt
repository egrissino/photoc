add_library(pc_compose STATIC
  psblend.cpp
  compose.cpp
  adjust.cpp
  masks.cpp
  damage.cpp
  effects.cpp
)
target_include_directories(pc_compose PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_compose PUBLIC
  
  pc_geom
  pc_color
  pc_raster
  pc_doc
  OpenMP::OpenMP_CXX
)
