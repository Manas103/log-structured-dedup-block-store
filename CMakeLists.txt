cmake_minimum_required(VERSION 3.16)
project(log_structured_dedup_block_store CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(dedupstore STATIC
  src/crc32c.cpp
  src/backing_file.cpp
  src/block_store.cpp
)
target_include_directories(dedupstore PUBLIC src)
find_library(URING_LIB uring REQUIRED)
target_link_libraries(dedupstore PUBLIC ${URING_LIB})

add_executable(nbd_server src/nbd_server.cpp src/main_nbd_server.cpp)
target_link_libraries(nbd_server PRIVATE dedupstore pthread)

option(BUILD_TESTS "Build the GoogleTest suite" ON)
if(BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(dedupstore_tests
    tests/test_dedup_correctness.cpp
    tests/test_hash_collision.cpp
    tests/test_gc_liveness.cpp
    tests/test_recovery.cpp
    tests/test_concurrency.cpp
  )
  target_link_libraries(dedupstore_tests PRIVATE dedupstore gtest_main pthread)
  target_include_directories(dedupstore_tests PRIVATE src)
endif()
