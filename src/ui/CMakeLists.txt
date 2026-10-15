add_library(pc_ui STATIC
  app.cpp
  adjust_ui.cpp
  transform_ui.cpp
  warp_ui.cpp
  calc_ui.cpp
  sam_ui.cpp
  mode_ui.cpp
  fill_layer_ui.cpp
  select_ui.cpp
  fill_ui.cpp
  image_ui.cpp
  retouch_ui.cpp
  theme.cpp
  toolbar.cpp
  layers_ui.cpp
  color_ui.cpp
  channels_ui.cpp
  props_ui.cpp
  picker_ui.cpp
  edit_ui.cpp
  filter_ui.cpp
  eyedropper_ui.cpp
  style_ui.cpp
  type_ui.cpp
  path_ui.cpp
  actions_ui.cpp
  heal_ui.cpp
  view_ui.cpp
  icons.cpp
  icon_data.cpp
  embedded.S
)
target_include_directories(pc_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(pc_ui PUBLIC
  
  pc_engine
  pc_io
  pc_compose
  pc_paint
  imgui
  SDL3::SDL3-static
  OpenGL::GL
)

# embedded.S includes the fonts with .incbin, found relative to the project root.
set_source_files_properties(embedded.S PROPERTIES
  COMPILE_OPTIONS "-Wa,-I${PROJECT_SOURCE_DIR}"
  OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf;${PROJECT_SOURCE_DIR}/assets/fonts/Inter-Medium.ttf;${PROJECT_SOURCE_DIR}/assets/fonts/Inter-SemiBold.ttf;${PROJECT_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf;${PROJECT_SOURCE_DIR}/assets/photoc-icon.jpg"
)
