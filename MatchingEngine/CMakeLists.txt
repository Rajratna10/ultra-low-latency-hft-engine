cmake_minimum_required(VERSION 3.14)
project(MatchingEngineBench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable maximum Release optimizations
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")

# Fetch Google Benchmark directly via CMake FetchContent
include(FetchContent)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.8.3
)

# Disable Werror and Testing for Google Benchmark
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googlebenchmark)

add_executable(engine_bench main.cpp)
target_link_libraries(engine_bench PRIVATE benchmark::benchmark)