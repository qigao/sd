cmake_minimum_required(VERSION 3.21)
project(sd_frontend LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(SD_APP_BUILD_TESTS "Build weight-free upstream integration tests" OFF)
option(SD_APP_BUILD_CLI "Build the locally maintained native CLI" ON)
option(SD_APP_BUILD_SERVER "Build the locally maintained native HTTP service" ON)
if(SD_APP_BUILD_TESTS)
  enable_testing()
endif()

# The engine retains its own options, targets, and installation rules. Service
# and workflow code belongs outside the upstream checkout.
include(cmake/UpstreamSdcpp.cmake)

if(SD_APP_BUILD_CLI OR SD_APP_BUILD_SERVER)
  add_subdirectory(apps)
endif()

if(SD_APP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
