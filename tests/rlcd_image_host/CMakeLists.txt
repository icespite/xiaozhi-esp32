cmake_minimum_required(VERSION 3.16)
project(rlcd_image_test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_compile_options(-g -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/managed_components/lvgl__lvgl" lvgl)
add_executable(rlcd_image_test
    ../rlcd_web_image_test.cc
    "${REPO}/main/boards/waveshare-s3-rlcd-4.2/managers/web_image.cc"
    "${REPO}/main/boards/waveshare-s3-rlcd-4.2/managers/web_page_parser.cc"
    "${REPO}/main/display/lvgl_display/gif/lvgl_gif.cc"
    "${REPO}/main/display/lvgl_display/gif/gifdec.c")
target_include_directories(rlcd_image_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${REPO}/main/boards/waveshare-s3-rlcd-4.2/managers"
    "${REPO}/main/display/lvgl_display")
target_link_libraries(rlcd_image_test PRIVATE lvgl::lvgl m)
