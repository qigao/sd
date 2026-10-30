find_package(Threads REQUIRED)
include(GNUInstallDirs)
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/sd-frontend")

# These are frontend utilities shared by the two local executables. Inference
# and all model implementations continue to come from the upstream library.
add_library(sd_app_common STATIC
  common/common.cpp
  common/generation_metadata.cpp
  common/log.cpp
  common/media_io.cpp
  common/media_metadata.cpp)
target_include_directories(sd_app_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sd_app_common PUBLIC stable-diffusion Threads::Threads)
target_compile_features(sd_app_common PUBLIC c_std_11 cxx_std_17)

# Upstream declares its shared-library import macro only in its directory.
# Carry that public C ABI contract into the separately maintained frontends.
get_target_property(_sd_app_engine_type stable-diffusion TYPE)
if(_sd_app_engine_type STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(sd_app_common PUBLIC SD_BUILD_SHARED_LIB)
endif()
if(MSVC)
  target_compile_definitions(sd_app_common PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
  target_compile_options(sd_app_common PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

# Source-built codec targets are globally visible. System-package aliases can
# be confined to the upstream directory, so resolve those packages here when
# necessary instead of treating an invisible target as a bare linker name.
if(SD_WEBP)
  target_compile_definitions(sd_app_common PUBLIC SD_USE_WEBP)
  if(TARGET webp AND TARGET libwebpmux)
    target_link_libraries(sd_app_common PUBLIC webp libwebpmux)
  elseif(SD_USE_SYSTEM_WEBP)
    if(TARGET PkgConfig::WebP AND TARGET PkgConfig::WebPMux)
      target_link_libraries(sd_app_common PUBLIC PkgConfig::WebP PkgConfig::WebPMux)
    else()
      find_package(WebP QUIET)
      if(WebP_FOUND)
        if(NOT TARGET WebP::webp)
          message(FATAL_ERROR "The selected WebP package must expose WebP::webp.")
        endif()
        if(TARGET WebP::libwebpmux)
          set(_sd_app_webp_mux WebP::libwebpmux)
        elseif(TARGET WebP::webpmux)
          set(_sd_app_webp_mux WebP::webpmux)
        else()
          message(FATAL_ERROR "The selected WebP package must expose a webpmux target.")
        endif()
        target_link_libraries(sd_app_common PUBLIC WebP::webp "${_sd_app_webp_mux}")
      else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SD_APP_WEBP REQUIRED IMPORTED_TARGET libwebp)
        pkg_check_modules(SD_APP_WEBPMUX REQUIRED IMPORTED_TARGET libwebpmux)
        target_link_libraries(sd_app_common PUBLIC
          PkgConfig::SD_APP_WEBP PkgConfig::SD_APP_WEBPMUX)
      endif()
    endif()
  else()
    message(FATAL_ERROR "Enabled bundled WebP must provide webp and libwebpmux targets.")
  endif()
endif()

if(SD_WEBM)
  target_compile_definitions(sd_app_common PUBLIC SD_USE_WEBM)
  if(TARGET webm)
    target_link_libraries(sd_app_common PUBLIC webm)
  elseif(SD_USE_SYSTEM_WEBM)
    if(TARGET PkgConfig::WebM)
      target_link_libraries(sd_app_common PUBLIC PkgConfig::WebM)
    else()
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(SD_APP_WEBM QUIET IMPORTED_TARGET libwebm)
      endif()
      if(TARGET PkgConfig::SD_APP_WEBM)
        target_link_libraries(sd_app_common PUBLIC PkgConfig::SD_APP_WEBM)
      else()
        # Reuse the package paths found by upstream's non-pkg-config branch.
        # These cache entries also preserve an explicit caller-selected SDK.
        find_path(WEBM_INCLUDE_DIR
          NAMES mkvmuxer/mkvmuxer.h mkvparser/mkvparser.h common/webmids.h
          PATH_SUFFIXES webm REQUIRED)
        find_library(WEBM_LIBRARY NAMES webm libwebm REQUIRED)
        add_library(sd_app_system_webm UNKNOWN IMPORTED)
        set_target_properties(sd_app_system_webm PROPERTIES
          IMPORTED_LOCATION "${WEBM_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${WEBM_INCLUDE_DIR}")
        target_link_libraries(sd_app_common PUBLIC sd_app_system_webm)
      endif()
    endif()
  else()
    message(FATAL_ERROR "Enabled bundled WebM must provide the webm target.")
  endif()
endif()

if(SD_APP_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(SD_APP_BUILD_SERVER)
  add_subdirectory(server)
endif()
