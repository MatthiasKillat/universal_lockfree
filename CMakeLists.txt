cmake_minimum_required(VERSION 3.10)
project(universal_lockfree )

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options("-O3")

include_directories( include )

add_executable(universal_lockfree
  main.cpp
)

add_executable(test_universal_lockfree_wrapper
  test_main.cpp
)

target_link_libraries(test_universal_lockfree_wrapper pthread rt)


