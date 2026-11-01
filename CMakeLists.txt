cmake_minimum_required(VERSION 3.10)
project(kvspace-c VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)

add_library(kvspace-c SHARED
    src/coord.c
    src/xvalue_head.c
    src/kvspace.c
    src/durable_abi.c
)

# SONAME 版本：libkvspace-c.so.1（dispatch 前端按此 dlopen 后端）
set_target_properties(kvspace-c PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
)

# Internal headers are in src/; the public ABI header is in kvspace.
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
    # 测试构建启用内部观测/注入钩子（生产库不含）。
    target_compile_definitions(kvspace-c PRIVATE P2_TEST_HOOKS)
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
    add_executable(test_codec_abi tests/test_codec_abi.c)
    target_include_directories(test_codec_abi PRIVATE src)
    target_link_libraries(test_codec_abi PRIVATE kvspace-c)
    add_test(NAME codec_abi COMMAND test_codec_abi)
    add_executable(test_abi_contract tests/test_abi_contract.c)
    target_include_directories(test_abi_contract PRIVATE src)
    target_link_libraries(test_abi_contract PRIVATE kvspace-c)
    add_test(NAME abi_contract COMMAND test_abi_contract)
    add_executable(test_inplace_body_only tests/test_inplace_body_only.c)
    target_include_directories(test_inplace_body_only PRIVATE src)
    target_link_libraries(test_inplace_body_only PRIVATE kvspace-c)
    add_test(NAME inplace_body_only COMMAND test_inplace_body_only)
    add_executable(test_set_part_fastpath tests/test_set_part_fastpath.c)
    target_include_directories(test_set_part_fastpath PRIVATE src)
    target_link_libraries(test_set_part_fastpath PRIVATE kvspace-c)
    add_test(NAME set_part_fastpath COMMAND test_set_part_fastpath)

    add_executable(test_new_wire_shm tests/test_new_wire_shm.c)
    target_include_directories(test_new_wire_shm PRIVATE src)
    target_link_libraries(test_new_wire_shm PRIVATE kvspace-c)
    add_test(NAME new_wire_shm COMMAND test_new_wire_shm)

    add_executable(test_slot_view tests/test_slot_view.c)
    target_include_directories(test_slot_view PRIVATE src)
    target_link_libraries(test_slot_view PRIVATE kvspace-c)
    add_test(NAME slot_view COMMAND test_slot_view)

    add_executable(test_art_alignment tests/test_art_alignment.c)
    target_include_directories(test_art_alignment PRIVATE src)
    target_compile_definitions(test_art_alignment PRIVATE P2_TEST_HOOKS)
    target_link_libraries(test_art_alignment PRIVATE kvspace-c)
    add_test(NAME art_alignment COMMAND test_art_alignment)

    add_executable(test_crash_recovery tests/test_crash_recovery.c)
    target_include_directories(test_crash_recovery PRIVATE src)
    target_link_libraries(test_crash_recovery PRIVATE kvspace-c)
    add_test(NAME crash_recovery COMMAND test_crash_recovery)
endif()
