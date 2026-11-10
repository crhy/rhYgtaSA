# CMake resolves a relative -DCMAKE_TOOLCHAIN_FILE against the top-level source
# directory (here: plugin/), so the path `plugin/toolchain-mingw32.cmake` used by
# plugin/build.sh and scripts/check.sh only resolves at this nested location.
# The real toolchain lives one level up; this stub just includes it.
include("${CMAKE_CURRENT_LIST_DIR}/../toolchain-mingw32.cmake")
