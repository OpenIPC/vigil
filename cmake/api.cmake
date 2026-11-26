# The HTTP/WebSocket API (src/api, docs/api.md): CivetWeb (third_party/
# civetweb, MIT) built lean -- no TLS, CGI, Lua or file serving; just
# the callbacks and WebSockets -- and crypt(3) for the password check.
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/civetweb/src/civetweb.c)
  message(FATAL_ERROR "third_party/civetweb is empty: git submodule update --init")
endif()
add_library(civetweb STATIC ${CMAKE_SOURCE_DIR}/third_party/civetweb/src/civetweb.c)
target_include_directories(civetweb PUBLIC ${CMAKE_SOURCE_DIR}/third_party/civetweb/include)
target_compile_definitions(civetweb PRIVATE NO_SSL NO_CGI NO_FILES NO_CACHING
                           USE_WEBSOCKET)
target_compile_options(civetweb PRIVATE -w)
target_link_libraries(civetweb PUBLIC Threads::Threads)

# glibc 2.38+ and libxcrypt: crypt() in libcrypt, declared in <crypt.h>;
# musl: in libc, <crypt.h> too; uClibc: libcrypt, declared in <unistd.h>.
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckLibraryExists)
check_include_file(crypt.h VIGIL_HAVE_CRYPT_H)
check_function_exists(crypt VIGIL_CRYPT_IN_LIBC)
set(_crypt_lib "")
if(NOT VIGIL_CRYPT_IN_LIBC)
  check_library_exists(crypt crypt "" VIGIL_HAVE_LIBCRYPT)
  if(NOT VIGIL_HAVE_LIBCRYPT)
    message(FATAL_ERROR "VIGIL_API needs crypt(3) (libc or libcrypt); or configure with -DVIGIL_API=OFF")
  endif()
  set(_crypt_lib crypt)
endif()

add_library(vigil_api STATIC ${CMAKE_SOURCE_DIR}/src/api/api.c ${CMAKE_SOURCE_DIR}/src/api/api_disc.c
            ${CMAKE_SOURCE_DIR}/src/api/api_mgmt.c ${CMAKE_SOURCE_DIR}/src/api/api_perm.c
            ${CMAKE_SOURCE_DIR}/src/api/api_users.c ${CMAKE_SOURCE_DIR}/src/api/auth.c
            ${CMAKE_SOURCE_DIR}/src/api/users.c)
target_include_directories(vigil_api PUBLIC ${CMAKE_SOURCE_DIR}/src/api)
target_compile_definitions(vigil_api PRIVATE VIGIL_VERSION="${PROJECT_VERSION}"
                           $<$<BOOL:${VIGIL_HAVE_CRYPT_H}>:VIGIL_HAVE_CRYPT_H>)
target_link_libraries(vigil_api PUBLIC vigil_sys vigil_core civetweb ${_crypt_lib})
