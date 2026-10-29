# Use only the original TinyTest runtime; the Salts SDK is not a dependency of
# the application or its inference backend.
_sd_app_read_gitlink("${PROJECT_SOURCE_DIR}" "vendor/salts" SD_APP_SALTS_REVISION)
set(_sd_app_salts_source "${PROJECT_SOURCE_DIR}/vendor/salts")
_sd_app_require_checkout(
  "${_sd_app_salts_source}" "${SD_APP_SALTS_REVISION}" "TinyTest source")

add_library(sd_app_test_tinytest STATIC
  "${_sd_app_salts_source}/tinytest/src/tinytest.c")
target_include_directories(sd_app_test_tinytest PUBLIC
  "${_sd_app_salts_source}/tinytest/include")
set_target_properties(sd_app_test_tinytest PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(test_sd_upstream_api test_upstream_api.cpp)
target_link_libraries(test_sd_upstream_api PRIVATE stable-diffusion sd_app_test_tinytest)
target_compile_features(test_sd_upstream_api PRIVATE cxx_std_17)
target_compile_definitions(test_sd_upstream_api PRIVATE
  "SD_APP_SDCPP_EXPECTED_COMMIT=\"${SD_APP_SDCPP_REVISION}\""
  "SD_APP_EXPECT_CPU=$<BOOL:${GGML_CPU}>")
get_target_property(_sd_app_library_type stable-diffusion TYPE)
if(_sd_app_library_type STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(test_sd_upstream_api PRIVATE SD_BUILD_SHARED_LIB)
endif()
add_test(NAME test_sd_upstream_api COMMAND test_sd_upstream_api)
set_tests_properties(test_sd_upstream_api PROPERTIES
  LABELS "sdcpp;no-weights" TIMEOUT 60
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:test_sd_upstream_api>")

# Help and device-list commands exit before loading weights or opening a port.
foreach(_frontend IN ITEMS cli server)
  if(TARGET "sd-${_frontend}")
    add_test(NAME "test_sd_${_frontend}_help" COMMAND "sd-${_frontend}" --help)
    add_test(NAME "test_sd_${_frontend}_devices" COMMAND "sd-${_frontend}" --list-devices)
    set_tests_properties("test_sd_${_frontend}_help" "test_sd_${_frontend}_devices"
      PROPERTIES LABELS "sdcpp;no-weights" TIMEOUT 60
                 WORKING_DIRECTORY "$<TARGET_FILE_DIR:sd-${_frontend}>")
  endif()
endforeach()

if(TARGET sd-server)
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  add_test(NAME test_sd_http_no_model
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/http_smoke.py"
            "$<TARGET_FILE:sd-server>")
  set_tests_properties(test_sd_http_no_model PROPERTIES
    LABELS "sdcpp;http;no-weights" TIMEOUT 45
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:sd-server>")
endif()
