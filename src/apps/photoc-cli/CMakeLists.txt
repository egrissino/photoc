add_executable(photoc-cli
  main.cpp
)
target_link_libraries(photoc-cli PRIVATE
  pc_engine
  pc_io
)
pc_windows_manifest(photoc-cli)

add_test(NAME cli_run_new COMMAND photoc-cli run --new "{\"width\":300,\"height\":200}"
  --cmd layer.new.layer --params "{\"mode\":\"Multiply\",\"fillNeutral\":true}"
  --cmd select.all --out cli_run_new.ppm)
add_test(NAME cli_info COMMAND photoc-cli info cli_run_new.ppm --compact)
set_tests_properties(cli_info PROPERTIES DEPENDS cli_run_new)
add_test(NAME cli_convert COMMAND photoc-cli convert cli_run_new.ppm cli_run_new.pam)
set_tests_properties(cli_convert PROPERTIES DEPENDS cli_run_new)
add_test(NAME cli_png COMMAND photoc-cli convert cli_run_new.ppm cli_run_new.png)
set_tests_properties(cli_png PROPERTIES DEPENDS cli_run_new)
add_test(NAME cli_png_back COMMAND photoc-cli convert cli_run_new.png cli_png_back.jpg --quality 85)
set_tests_properties(cli_png_back PROPERTIES DEPENDS cli_png)
add_test(NAME cli_jpg_info COMMAND photoc-cli info cli_png_back.jpg --compact)
set_tests_properties(cli_jpg_info PROPERTIES DEPENDS cli_png_back)

target_compile_options(photoc-cli PUBLIC -Wno-unused-parameter -Wno-missing-field-initializers -ffp-contract=off)
