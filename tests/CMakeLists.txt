add_executable(test_xvalue test_xvalue.cpp)
target_link_libraries(test_xvalue PRIVATE kvspace)
add_test(NAME xvalue COMMAND test_xvalue)

add_executable(test_shm test_shm.cpp)
target_include_directories(test_shm PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_shm PRIVATE kvspace)
add_test(NAME shm COMMAND test_shm)

add_executable(test_clear_preflight test_clear_preflight.cpp)
target_include_directories(
    test_clear_preflight PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_clear_preflight PRIVATE kvspace)
add_test(NAME clear_preflight COMMAND test_clear_preflight)

add_executable(test_allocators test_allocators.cpp)
target_include_directories(test_allocators PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_allocators PRIVATE kvspace)
add_test(NAME allocators COMMAND test_allocators)

add_executable(test_process test_process.cpp)
target_include_directories(test_process PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_process PRIVATE kvspace)
target_link_options(test_process PRIVATE
    -Wl,--wrap=pthread_cond_broadcast
    -Wl,--wrap=ftruncate
)
add_test(NAME process COMMAND test_process)

add_executable(test_limit_recovery test_limit_recovery.cpp)
target_include_directories(
    test_limit_recovery PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_limit_recovery PRIVATE kvspace)
add_test(NAME limit_recovery COMMAND test_limit_recovery)

add_executable(test_bootstrap_contract test_bootstrap_contract.cpp)
target_link_libraries(test_bootstrap_contract PRIVATE kvspace)
target_link_options(test_bootstrap_contract PRIVATE
    -Wl,--wrap=ftruncate
    -Wl,--wrap=mmap
    -Wl,--wrap=pread
)
add_test(NAME bootstrap_contract COMMAND test_bootstrap_contract)

add_executable(test_owner_death_protocol test_owner_death_protocol.cpp)
target_include_directories(
    test_owner_death_protocol PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_owner_death_protocol PRIVATE kvspace)
target_link_options(test_owner_death_protocol PRIVATE
    -Wl,--wrap=pthread_mutex_lock
    -Wl,--wrap=pthread_cond_broadcast
)
add_test(NAME owner_death_protocol COMMAND test_owner_death_protocol)

add_executable(test_trie_node_store test_trie_node_store.cpp)
target_include_directories(
    test_trie_node_store PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_trie_node_store PRIVATE kvspace)
add_test(NAME trie_node_store COMMAND test_trie_node_store)

add_executable(test_art_box_store test_art_box_store.cpp)
target_include_directories(
    test_art_box_store PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_art_box_store PRIVATE kvspace)
add_test(NAME art_box_store COMMAND test_art_box_store)

add_executable(test_art_bump_store test_art_bump_store.cpp)
target_include_directories(
    test_art_bump_store PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_art_bump_store PRIVATE kvspace)
add_test(NAME art_bump_store COMMAND test_art_bump_store)

add_executable(test_art_bump_engine test_art_bump_engine.cpp)
target_include_directories(
    test_art_bump_engine PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_art_bump_engine PRIVATE kvspace)
add_test(NAME art_bump_engine COMMAND test_art_bump_engine)

add_executable(test_art_bump_recovery test_art_bump_recovery.cpp)
target_include_directories(
    test_art_bump_recovery PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_art_bump_recovery PRIVATE kvspace)
add_test(NAME art_bump_recovery COMMAND test_art_bump_recovery)

add_executable(test_art_box_engine test_art_box_engine.cpp)
target_include_directories(
    test_art_box_engine PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_art_box_engine PRIVATE kvspace)
add_test(NAME art_box_engine COMMAND test_art_box_engine)

add_executable(test_trie_recovery_faults test_trie_recovery_faults.cpp)
target_include_directories(
    test_trie_recovery_faults PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_trie_recovery_faults PRIVATE kvspace)
add_test(NAME trie_recovery_faults COMMAND test_trie_recovery_faults)

add_executable(test_trie_cow_recovery test_trie_cow_recovery.cpp)
target_include_directories(
    test_trie_cow_recovery PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_trie_cow_recovery PRIVATE kvspace)
add_test(NAME trie_cow_recovery COMMAND test_trie_cow_recovery)

add_executable(test_trie_index test_trie_index.cpp)
target_include_directories(
    test_trie_index PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_trie_index PRIVATE kvspace)
add_test(NAME trie_index COMMAND test_trie_index)

add_executable(test_trie_engine test_trie_engine.cpp)
target_include_directories(
    test_trie_engine PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_trie_engine PRIVATE kvspace)
add_test(NAME trie_engine COMMAND test_trie_engine)

add_executable(test_hash_engine test_hash_engine.cpp)
target_include_directories(
    test_hash_engine PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_hash_engine PRIVATE kvspace)
add_test(NAME hash_engine COMMAND test_hash_engine)

add_executable(test_c_api test_c_api.c)
target_link_libraries(test_c_api PRIVATE kvspace)
set_target_properties(test_c_api PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME c_api COMMAND test_c_api)

add_executable(test_c_api_alloc_failure test_c_api_alloc_failure.cpp)
target_link_libraries(test_c_api_alloc_failure PRIVATE kvspace)
target_link_options(test_c_api_alloc_failure PRIVATE -Wl,--wrap=__cxa_throw)
add_test(NAME c_api_alloc_failure COMMAND test_c_api_alloc_failure)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(
        target
        test_xvalue
        test_shm
        test_clear_preflight
        test_allocators
        test_process
        test_limit_recovery
        test_bootstrap_contract
        test_owner_death_protocol
        test_trie_node_store
        test_art_bump_store
        test_art_bump_engine
        test_art_bump_recovery
        test_art_box_store
        test_art_box_engine
        test_trie_recovery_faults
        test_trie_cow_recovery
        test_trie_index
        test_hash_engine
        test_c_api_alloc_failure)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
    endforeach()
    target_compile_options(test_trie_engine PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # This test deliberately replaces the global new/delete pair with
    # malloc/free to inject allocation failures. GCC 11 diagnoses that
    # matched replacement after Release inlining as mismatched-new-delete.
    target_compile_options(test_art_bump_engine PRIVATE
        -Wno-mismatched-new-delete)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_c_api PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
    )
endif()
