# Called after registering main, when managed components have been resolved. Only the
# RLCD board replaces the portal source and embeds its extended HTML page.
idf_component_get_property(wifi_dir 78__esp-wifi-connect COMPONENT_DIR)
idf_component_get_property(wifi_lib 78__esp-wifi-connect COMPONENT_LIB)
set(portal_dir "${CMAKE_CURRENT_LIST_DIR}")
set(portal_output "${CMAKE_BINARY_DIR}/rlcd_wifi_portal")
set(portal_generator "${PROJECT_DIR}/scripts/generate_rlcd_wifi_portal.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${portal_generator}" "${portal_dir}/wifi_portal.html"
    "${wifi_dir}/wifi_configuration_ap.cc" "${wifi_dir}/assets/wifi_configuration.html")
execute_process(
    COMMAND "${PYTHON}" "${portal_generator}" --component "${wifi_dir}"
        --fragment "${portal_dir}/wifi_portal.html" --output "${portal_output}"
    RESULT_VARIABLE portal_result
)
if(NOT portal_result EQUAL 0)
    message(FATAL_ERROR "Failed to extend the RLCD Wi-Fi provisioning page")
endif()
get_target_property(wifi_sources ${wifi_lib} SOURCES)
list(FILTER wifi_sources EXCLUDE REGEX "(^|/)wifi_configuration_ap\\.cc$")
set_property(TARGET ${wifi_lib} PROPERTY SOURCES ${wifi_sources} "${portal_output}/wifi_configuration_ap.cc")
target_include_directories(${wifi_lib} PRIVATE "${portal_dir}/managers")
target_add_binary_data(${COMPONENT_LIB} "${portal_output}/rlcd_wifi_configuration.html" TEXT)
