cmake_minimum_required(VERSION 3.10)
project(kvspace-c VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)

add_library(kvspace-c SHARED
    src/xvalue.c
    src/xvalue_head.c
    src/kvspace.c
    src/durable_abi.c
)

# SONAME 版本：libkvspace-c.so.1（dispatch 前端按此 dlopen 后端）
set_target_properties(kvspace-c PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# 无公共头：内部头在 src/（kvspace_shm.h、xvalue.h），权威 ABI 头在 kvspace 仓库。
# 两个依赖库走 header-only（src/kvspace.c 里 #define *_IMPLEMENTATION），故不链接 .so；
# 头的搜索路径由系统 include（/usr/include/blockmalloc、/usr/include/slotsboxmalloc）提供。

# 后端库固定装到 dispatch 前端 dlopen 的目录 <prefix>/lib/kvspace。
# 默认 prefix=/usr（对齐 ci/deps.sh 的 /usr/lib/kvspace），而非 /usr/local。
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "" FORCE)
endif()
install(TARGETS kvspace-c
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/kvspace
)

option(KVSPACE_BUILD_TESTS "Build tests (ctest)" OFF)
if(KVSPACE_BUILD_TESTS)
    enable_testing()
    add_executable(test_shm_split tests/test_shm_split.c)
    target_include_directories(test_shm_split PRIVATE src)
    target_link_libraries(test_shm_split PRIVATE kvspace-c)
    add_test(NAME shm_split COMMAND test_shm_split)

    add_executable(test_shm_resize tests/test_shm_resize.c)
    target_include_directories(test_shm_resize PRIVATE src)
    target_link_libraries(test_shm_resize PRIVATE kvspace-c)
    add_test(NAME shm_resize COMMAND test_shm_resize)

    add_executable(test_art_scan tests/test_art_scan.c)
    target_include_directories(test_art_scan PRIVATE src)
    target_link_libraries(test_art_scan PRIVATE kvspace-c)
    add_test(NAME art_scan COMMAND test_art_scan)

    add_executable(test_handle_cache tests/test_handle_cache.c)
    target_include_directories(test_handle_cache PRIVATE src)
    target_link_libraries(test_handle_cache PRIVATE kvspace-c)
    add_test(NAME handle_cache COMMAND test_handle_cache)

    add_executable(test_xvalue_head tests/test_xvalue_head.c src/xvalue_head.c)
    target_include_directories(test_xvalue_head PRIVATE src)
    add_test(NAME xvalue_head COMMAND test_xvalue_head)
endif()
