cmake_minimum_required(VERSION 3.20)
project(kvspace VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(kvspace_shm_lifetime OBJECT
    src/shm_lifetime.cpp
)

add_library(kvspace STATIC
    src/client.cpp
    src/xvalue.cpp
    src/shm_box_allocators.cpp
    src/shm_art_bump_store.cpp
    src/shm_art_bump_index.cpp
    src/shm_art_box_store.cpp
    src/shm_trie_node_store.cpp
    src/shm_trie_index.cpp
    src/shm_region.cpp
    src/shm.cpp
    src/kshm.cpp
    $<TARGET_OBJECTS:kvspace_shm_lifetime>
)

target_include_directories(kvspace
    PUBLIC include
    PRIVATE src
)
target_link_libraries(kvspace PUBLIC Threads::Threads)
target_compile_definitions(kvspace PRIVATE _GNU_SOURCE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Keep the implicit-lifetime barrier in a separately optimized, non-LTO
    # translation unit. Consumers remain external calls, while the self-copy
    # itself can compile to no memory traffic.
    target_compile_options(kvspace_shm_lifetime PRIVATE
        -O2 -fno-lto
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
    )
    target_compile_options(kvspace PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
    )
    set_source_files_properties(
        src/shm_region.cpp
        src/shm_art_bump_store.cpp
        src/shm_trie_index.cpp
        PROPERTIES COMPILE_OPTIONS -fno-strict-aliasing
    )
endif()

option(KVSPACE_BUILD_TESTS "Build kvspace tests" ON)
if(KVSPACE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS kvspace ARCHIVE DESTINATION lib)
install(DIRECTORY include/kvspace DESTINATION include)
