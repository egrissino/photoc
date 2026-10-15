add_library(pc_geom STATIC
  geom.cpp
)
target_include_directories(pc_geom PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_geom PUBLIC
  
)
