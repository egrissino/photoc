add_library(pc_ops STATIC
  history.cpp
)
target_include_directories(pc_ops PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_ops PUBLIC
  
  pc_doc
)
