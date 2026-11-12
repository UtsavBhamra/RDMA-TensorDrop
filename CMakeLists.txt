cmake_minimum_required(VERSION 3.16)
project(tensordrop VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h)
find_library(IBVERBS_LIBRARY ibverbs)

add_executable(tensordrop
  src/main.cpp
  src/simulator.cpp
  src/metrics.cpp
)
target_include_directories(tensordrop PRIVATE include)
target_compile_options(tensordrop PRIVATE -Wall -Wextra -Wpedantic)

if(IBVERBS_INCLUDE_DIR AND IBVERBS_LIBRARY)
  target_sources(tensordrop PRIVATE src/rdma.cpp)
  target_include_directories(tensordrop PRIVATE ${IBVERBS_INCLUDE_DIR})
  target_link_libraries(tensordrop PRIVATE ${IBVERBS_LIBRARY})
  target_compile_definitions(tensordrop PRIVATE TENSORDROP_HAVE_IBVERBS=1)
  message(STATUS "libibverbs found: building the real RDMA transfer path")
else()
  message(STATUS "libibverbs not found: building simulator-only mode")
endif()

enable_testing()
add_test(NAME simulator_file_transfer
  COMMAND ${CMAKE_COMMAND}
    -DPROGRAM=$<TARGET_FILE:tensordrop>
    -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/demo_checkpoint.bin
    -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/simulator-test
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/simulator_smoke.cmake
)
