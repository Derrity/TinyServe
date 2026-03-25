cmake_minimum_required(VERSION 3.16)
project(tinyserve C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find libuv — try pkg-config first, fall back to find_path/find_library
# (fallback enables cross-compilation where pkg-config is unavailable)
set(LIBUV_FOUND OFF)

if(NOT LIBUV_INCLUDE_DIRS OR NOT LIBUV_LIBRARIES)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUV libuv)
    endif()
endif()

if(NOT LIBUV_FOUND)
    find_path(LIBUV_INCLUDE_DIRS uv.h
        HINTS ${CMAKE_PREFIX_PATH}/include /usr/include /usr/local/include)
    find_library(LIBUV_LIBRARIES NAMES uv uv_a
        HINTS ${CMAKE_PREFIX_PATH}/lib /usr/lib /usr/local/lib)
    if(LIBUV_INCLUDE_DIRS AND LIBUV_LIBRARIES)
        set(LIBUV_FOUND ON)
    endif()
endif()

if(NOT LIBUV_FOUND AND NOT LIBUV_INCLUDE_DIRS)
    message(FATAL_ERROR "libuv not found. Install libuv or set -DCMAKE_PREFIX_PATH=<libuv-install-dir>")
endif()

add_executable(tinyserve
    src/main.c
    src/log.c
    src/mime.c
    src/path_utils.c
    src/config.c
    src/http_parser.c
    src/http_response.c
    src/auth.c
    src/range.c
    src/file_serve.c
    src/route.c
    src/proxy.c
    src/server.c
)

target_include_directories(tinyserve PRIVATE
    ${LIBUV_INCLUDE_DIRS}
    src
)

target_link_libraries(tinyserve PRIVATE
    ${LIBUV_LIBRARIES}
)

if(LIBUV_LIBRARY_DIRS)
    target_link_directories(tinyserve PRIVATE
        ${LIBUV_LIBRARY_DIRS}
    )
endif()

# Compiler warnings
target_compile_options(tinyserve PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

# Platform-specific settings
if(APPLE)
    target_compile_definitions(tinyserve PRIVATE TS_PLATFORM_MACOS)
elseif(UNIX)
    target_compile_definitions(tinyserve PRIVATE TS_PLATFORM_LINUX)
    # Link pthread/dl/rt for static libuv on Linux
    target_link_libraries(tinyserve PRIVATE pthread dl)
endif()

install(TARGETS tinyserve RUNTIME DESTINATION bin)
