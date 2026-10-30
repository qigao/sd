add_library(sd_app_workflow_v1 STATIC workflow_v1.cpp)
target_link_libraries(sd_app_workflow_v1 PUBLIC sd_app_common)
target_include_directories(sd_app_workflow_v1 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(sd_app_async_state STATIC async_job_state.cpp)
target_link_libraries(sd_app_async_state PUBLIC sd_app_common)
target_include_directories(sd_app_async_state PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(sd-app-server
  main.cpp
  runtime.cpp
  model_catalog.cpp
  routes_models.cpp
  async_jobs.cpp
  routes_index.cpp
  routes_openai.cpp
  routes_sdapi.cpp
  routes_sdcpp.cpp
  routes_workflows.cpp
  workflow_execution.cpp)
target_link_libraries(sd-app-server PRIVATE sd_app_common sd_app_workflow_v1 sd_app_async_state)

# Keep upstream's model catalog as the single builtin recipe source. Local
# recipe extensions already use the service's model-root/recipes directory.
get_target_property(_sd_app_engine_source stable-diffusion SOURCE_DIR)
file(GLOB _sd_app_builtin_recipes CONFIGURE_DEPENDS
  "${_sd_app_engine_source}/examples/server/recipes/*.json")
if(NOT _sd_app_builtin_recipes)
  message(FATAL_ERROR "The selected sd.cpp checkout has no builtin server recipes.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sd_app_builtin_recipes})
set(SD_BUILTIN_RECIPES "")
foreach(_recipe IN LISTS _sd_app_builtin_recipes)
  file(READ "${_recipe}" _recipe_json)
  string(APPEND SD_BUILTIN_RECIPES "R\"recipes(${_recipe_json})recipes\",\n")
endforeach()
configure_file(recipes_builtin.h.in "${CMAKE_CURRENT_BINARY_DIR}/recipes_builtin.h" @ONLY)
target_include_directories(sd-app-server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

if(APPLE)
  sd_set_macos_rpaths(sd-app-server)
endif()
if(WIN32 AND NOT MSVC)
  target_link_libraries(sd-app-server PRIVATE ws2_32)
endif()
install(TARGETS sd-app-server RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
