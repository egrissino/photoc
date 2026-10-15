add_library(pc_color STATIC
  color.cpp
  convert.cpp
  blend.cpp
)
target_include_directories(pc_color PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_color PUBLIC
  
  PkgConfig::LCMS2
)
