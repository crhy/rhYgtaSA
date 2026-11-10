cmake_minimum_required(VERSION 3.16)
project(rhyremix CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(rhyremix SHARED
  src/main.cpp
  src/log.cpp
  src/remix.cpp)

target_include_directories(rhyremix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/remix")

target_compile_options(rhyremix PRIVATE -Wall)

# Static MinGW runtime: the .asi must not need libgcc/libstdc++/libwinpthread DLLs.
target_link_options(rhyremix PRIVATE -static -static-libgcc -static-libstdc++)

set_target_properties(rhyremix PROPERTIES
  OUTPUT_NAME "rhyremix"
  PREFIX ""
  SUFFIX ".asi")
