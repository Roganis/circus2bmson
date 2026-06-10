# Thin CLI wrapper around the core library.
add_executable(circus2bmson_cli main.cpp)
set_target_properties(circus2bmson_cli PROPERTIES OUTPUT_NAME circus2bmson)
target_link_libraries(circus2bmson_cli PRIVATE circus2bmson::circus2bmson)
