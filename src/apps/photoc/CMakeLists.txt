add_executable(photoc
  main.cpp
)
target_link_libraries(photoc PRIVATE
  pc_ui
  pc_io
  pc_codecs
  SDL3::SDL3-static
  imgui
  OpenGL::GL
)

# A GUI program on Windows: no console window opens beside it
set_target_properties(photoc PROPERTIES WIN32_EXECUTABLE TRUE)
pc_windows_manifest(photoc)

target_compile_options(photoc PUBLIC -Wno-unused-parameter -Wno-missing-field-initializers -ffp-contract=off)
