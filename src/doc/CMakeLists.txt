add_library(pc_doc STATIC
  doc.cpp
  adjust.cpp
  text.cpp
  vector.cpp
)
target_include_directories(pc_doc PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_doc PUBLIC
  
  pc_geom
  pc_color
  pc_raster
)
