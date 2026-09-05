# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.5.12")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_eth_router)

# This wires lwip_hooks.h into the lwip component's build so
# LWIP_HOOK_IP4_ROUTE_SRC resolves to our ip4_route_src_hook().
# Pattern taken from: https://esp32.com/viewtopic.php?t=30050
#
# This is the one piece of the whole port with no confirmed working
# precedent on IDF 5.5 - treat getting this to compile AND actually
# fire (see the STAGE 1 comment in lwip_hooks.c) as the first
# checkpoint before writing anything else.

idf_component_get_property(lwip_lib lwip COMPONENT_LIB)

message(STATUS "lwip_lib = ${lwip_lib}")

target_include_directories(${lwip_lib} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(${lwip_lib} PRIVATE
    "ESP_IDF_LWIP_HOOK_FILENAME=\"lwip_hooks.h\""
)

# Force esp_app_desc.c to recompile on every build so that
# __DATE__/__TIME__ in the app descriptor stay current.
set(APP_DESC_OBJ "${CMAKE_BINARY_DIR}/esp-idf/esp_app_format/CMakeFiles/__idf_esp_app_format.dir/esp_app_desc.c.obj")
add_custom_target(force_app_desc_rebuild ALL
    COMMAND ${CMAKE_COMMAND} -E remove -f "${APP_DESC_OBJ}"
    COMMENT "Ensuring app descriptor timestamp is current"
)
add_dependencies(${PROJECT_NAME}.elf force_app_desc_rebuild)
