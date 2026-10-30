# Deployment presets select upstream SD_*/GGML_* options. This boundary checks
# dependency identity without rewriting options or maintaining a feature list.
get_filename_component(_sd_app_repository "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
set(SD_APP_SDCPP_SOURCE_DIR
    "${_sd_app_repository}/vendor/stable-diffusion.cpp" CACHE PATH
    "Path to the recorded stable-diffusion.cpp checkout")

find_package(Git REQUIRED)

# Shared by the engine boundary and test-only dependencies. A resolved staged
# gitlink permits verifying an upstream update before committing the frontend.
function(_sd_app_read_gitlink repository path out_revision)
  file(REAL_PATH "${repository}" _repository)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_repository}" rev-parse --show-toplevel
    RESULT_VARIABLE _root_result
    OUTPUT_VARIABLE _git_root
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT _root_result EQUAL 0)
    message(FATAL_ERROR "Dependency identity requires the frontend Git checkout and its recorded gitlinks.")
  endif()
  file(REAL_PATH "${_git_root}" _git_root)
  if(NOT _git_root STREQUAL _repository)
    message(FATAL_ERROR "Dependency gitlinks must come from the frontend checkout, not an ancestor repository.")
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_repository}" ls-files --stage -- "${path}"
    RESULT_VARIABLE _gitlink_result
    OUTPUT_VARIABLE _gitlink
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT _gitlink_result EQUAL 0 OR
     NOT _gitlink MATCHES "^160000 ([0-9a-f]+) 0\t(.+)$")
    message(FATAL_ERROR "The frontend must record one resolved '${path}' gitlink in its index.")
  endif()
  set(_revision "${CMAKE_MATCH_1}")
  set(_recorded_path "${CMAKE_MATCH_2}")
  if(NOT _recorded_path STREQUAL path)
    message(FATAL_ERROR "The frontend must record exactly one resolved '${path}' gitlink in its index.")
  endif()
  set(${out_revision} "${_revision}" PARENT_SCOPE)
endfunction()

function(_sd_app_require_checkout source_dir revision label)
  if(NOT IS_DIRECTORY "${source_dir}")
    message(FATAL_ERROR
      "${label} is missing at '${source_dir}'. Initialize its checkout "
      "at revision ${revision} before configuring sd_frontend.")
  endif()

  file(REAL_PATH "${source_dir}" _source_root)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_source_root}" rev-parse --show-toplevel
    RESULT_VARIABLE _root_result
    OUTPUT_VARIABLE _git_root
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT _root_result EQUAL 0)
    message(FATAL_ERROR "${label} must be a Git checkout at revision ${revision}.")
  endif()
  file(REAL_PATH "${_git_root}" _git_root)
  if(NOT _git_root STREQUAL _source_root)
    message(FATAL_ERROR
      "${label} is not an initialized checkout at '${_source_root}'. "
      "An ancestor repository cannot supply its revision identity.")
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_source_root}" rev-parse --verify "HEAD^{commit}"
    RESULT_VARIABLE _revision_result
    OUTPUT_VARIABLE _actual_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT _revision_result EQUAL 0 OR NOT _actual_revision STREQUAL revision)
    message(FATAL_ERROR
      "${label} must use revision ${revision}; found '${_actual_revision}'.")
  endif()

  # Upstream discovers sources with GLOB. Untracked files must not silently
  # enter the build while the checkout still reports the recorded HEAD.
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_source_root}" status
      --porcelain --untracked-files=all --ignore-submodules=none
    RESULT_VARIABLE _status_result
    OUTPUT_VARIABLE _checkout_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT _status_result EQUAL 0 OR NOT _checkout_status STREQUAL "")
    message(FATAL_ERROR
      "${label} must be a clean checkout of its recorded revision. "
      "Build outside the source tree and retain upstream changes separately.")
  endif()
endfunction()

_sd_app_read_gitlink(
  "${_sd_app_repository}" "vendor/stable-diffusion.cpp" SD_APP_SDCPP_REVISION)
_sd_app_require_checkout(
  "${SD_APP_SDCPP_SOURCE_DIR}" "${SD_APP_SDCPP_REVISION}" "stable-diffusion.cpp")
file(REAL_PATH "${SD_APP_SDCPP_SOURCE_DIR}" _sd_app_sdcpp_source)

# Avoid accidental reuse of unrelated targets. Explicit upstream system/custom
# GGML selection remains available and resolves its own private headers.
foreach(_target IN ITEMS stable-diffusion ggml ggml::ggml ggml-base
                         ggml::ggml-base ggml-cpu ggml-cuda)
  if(TARGET ${_target})
    message(FATAL_ERROR
      "sd_frontend requires exclusive ownership of '${_target}'. "
      "Select sd.cpp's GGML dependency through its upstream options instead.")
  endif()
endforeach()

# Preserve the complete upstream build graph and install rules, including
# selected examples, shared libraries, and dynamically loaded GGML backends.
add_subdirectory(
  "${_sd_app_sdcpp_source}"
  "${CMAKE_CURRENT_BINARY_DIR}/upstream")

# Inspect the target upstream actually selected, without reinterpreting its
# source options. System/custom GGML retains upstream dependency/private-header
# checks and does not inherit the bundled dependency's qualification.
if(TARGET ggml)
  get_target_property(_sd_app_ggml_imported ggml IMPORTED)
endif()
# Imported targets may be visible only within the upstream subdirectory.
# Source-built targets are global and can be inspected here.
if(TARGET ggml AND NOT _sd_app_ggml_imported)
  get_target_property(_sd_app_ggml_target_source ggml SOURCE_DIR)
  file(REAL_PATH "${_sd_app_ggml_target_source}" _sd_app_ggml_target_source)
  file(REAL_PATH "${_sd_app_sdcpp_source}/ggml" _sd_app_bundled_ggml)
  file(REAL_PATH "${_sd_app_bundled_ggml}/src" _sd_app_bundled_ggml_src)
  if(_sd_app_ggml_target_source STREQUAL _sd_app_bundled_ggml_src)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${_sd_app_sdcpp_source}" ls-tree HEAD -- ggml
      RESULT_VARIABLE _ggml_gitlink_result
      OUTPUT_VARIABLE _ggml_gitlink
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(NOT _ggml_gitlink_result EQUAL 0 OR
       NOT _ggml_gitlink MATCHES "^160000 commit ([0-9a-f]+)\tggml$")
      message(FATAL_ERROR "sd.cpp must record its bundled ggml gitlink.")
    endif()
    set(_sd_app_ggml_revision "${CMAKE_MATCH_1}")
    _sd_app_require_checkout(
      "${_sd_app_bundled_ggml}" "${_sd_app_ggml_revision}" "sd.cpp bundled GGML")
  endif()
endif()
