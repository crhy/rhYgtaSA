cmake_minimum_required(VERSION 3.16)
project(rhyremix CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(rhyremix SHARED
  src/main.cpp
  src/log.cpp
  src/config.cpp
  src/remix.cpp
  src/delayed.cpp
  src/game.cpp
  src/probe.cpp
  src/streetlights.cpp
  src/sun.cpp
  src/trafficlights.cpp)

target_include_directories(rhyremix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/remix")

# The plugin is 32-bit (the game is); the Remix API structs are fixed-size and
# identical, so silence its x64-only guard and skip its built-in library loader.
target_compile_definitions(rhyremix PRIVATE REMIX_ALLOW_X86 REMIX_WINAPI_NO_LIBRARY_LOADER)

target_compile_options(rhyremix PRIVATE -Wall)

# Static MinGW runtime: the .asi must not need libgcc/libstdc++/libwinpthread DLLs.
target_link_options(rhyremix PRIVATE -static -static-libgcc -static-libstdc++)

set_target_properties(rhyremix PROPERTIES
  OUTPUT_NAME "rhyremix"
  PREFIX ""
  SUFFIX ".asi")
