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
# Test each configured frontend so owned applications can coexist with upstream
# examples without target-name or API routing collisions.
if(TARGET sd-server OR TARGET sd-app-server)
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
endif()
foreach(_prefix IN ITEMS sd sd-app)
  string(REPLACE "-" "_" _test_prefix "${_prefix}")
  foreach(_frontend IN ITEMS cli server)
    set(_target "${_prefix}-${_frontend}")
    if(TARGET "${_target}")
      add_test(NAME "test_${_test_prefix}_${_frontend}_help"
        COMMAND "${_target}" --help)
      add_test(NAME "test_${_test_prefix}_${_frontend}_devices"
        COMMAND "${_target}" --list-devices)
      set_tests_properties(
        "test_${_test_prefix}_${_frontend}_help" "test_${_test_prefix}_${_frontend}_devices"
        PROPERTIES LABELS "sdcpp;no-weights" TIMEOUT 60
                   WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_target}>")
    endif()
  endforeach()

  if(TARGET "${_prefix}-server")
    add_test(NAME "test_${_test_prefix}_http_no_model"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/http_smoke.py"
              "$<TARGET_FILE:${_prefix}-server>")
    set_tests_properties("test_${_test_prefix}_http_no_model" PROPERTIES
      LABELS "sdcpp;http;no-weights" TIMEOUT 45
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_prefix}-server>")
  endif()
endforeach()

# Pure plan/scheduler tests execute without model weights, devices or HTTP.
if(TARGET sd_app_workflow_v1)
  add_executable(test_sd_app_workflow_v1 test_workflow_v1.cpp)
  target_link_libraries(test_sd_app_workflow_v1 PRIVATE sd_app_workflow_v1 sd_app_test_tinytest)
  add_test(NAME test_sd_app_workflow_v1 COMMAND test_sd_app_workflow_v1)
  set_tests_properties(test_sd_app_workflow_v1 PROPERTIES
    LABELS "workflow;no-weights" TIMEOUT 60
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:test_sd_app_workflow_v1>")
endif()

# A test-only fixture may point at user-provided real weights. Never download,
# synthesize, skip or claim inference acceptance without that fixture.
set(SD_APP_WORKFLOW_TEST_MANIFEST "" CACHE FILEPATH
    "Local JSON manifest with pinned SHA-256 real weights for Workflow V1 qualification")
if(TARGET sd-app-server)
  add_test(NAME test_sd_app_workflow_qualifier_selftest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/workflow_real_model.py"
            --self-test)
  set_tests_properties(test_sd_app_workflow_qualifier_selftest PROPERTIES
    LABELS "workflow;no-weights;test-harness" TIMEOUT 30)
  if(SD_APP_WORKFLOW_TEST_MANIFEST)
    if(NOT EXISTS "${SD_APP_WORKFLOW_TEST_MANIFEST}")
      message(FATAL_ERROR "SD_APP_WORKFLOW_TEST_MANIFEST must refer to a local existing JSON file")
    endif()
    add_test(NAME test_sd_app_workflow_real_weights
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/workflow_real_model.py"
              "$<TARGET_FILE:sd-app-server>" "${SD_APP_WORKFLOW_TEST_MANIFEST}")
    set_tests_properties(test_sd_app_workflow_real_weights PROPERTIES
      LABELS "workflow;real-weights;native-inference" TIMEOUT 2400
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:sd-app-server>")
  endif()
endif()
