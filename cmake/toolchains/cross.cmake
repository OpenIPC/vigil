# Cross-compile with a GCC toolchain given by its prefix, e.g.
#   cmake -B build-hi3536 --toolchain cmake/toolchains/cross.cmake \
#         -DCROSS_PREFIX=<toolchain>/bin/arm-openipc-linux-gnueabi- \
#         -DVIGIL_PLATFORM=hi3536dv100
set(CMAKE_SYSTEM_NAME Linux)
if(NOT CROSS_PREFIX)
  message(FATAL_ERROR "set CROSS_PREFIX to the toolchain prefix (…/bin/<triplet>-)")
endif()
string(REGEX MATCH "[^/]*-$" _triplet "${CROSS_PREFIX}")
if(_triplet MATCHES "^aarch64")
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
else()
  set(CMAKE_SYSTEM_PROCESSOR arm)
endif()
set(CMAKE_C_COMPILER ${CROSS_PREFIX}gcc)
set(CMAKE_AR ${CROSS_PREFIX}ar CACHE FILEPATH "")
set(CMAKE_STRIP ${CROSS_PREFIX}strip CACHE FILEPATH "")
# Libraries the build looks for (libyaml) are searched in the toolchain's
# own sysroot, plus whatever -DCMAKE_FIND_ROOT_PATH adds (a staging dir).
execute_process(COMMAND ${CROSS_PREFIX}gcc -print-sysroot
                OUTPUT_VARIABLE _sysroot OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_sysroot AND IS_DIRECTORY "${_sysroot}")
  list(APPEND CMAKE_FIND_ROOT_PATH "${_sysroot}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Keep the prefix visible to try_compile projects.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CROSS_PREFIX)
