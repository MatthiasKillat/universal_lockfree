cmake_minimum_required(VERSION 3.10)
project(universal_lockfree )

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories( include )

add_executable(universal_lockfree
  main.cpp
)

target_link_libraries(universal_lockfree pthread rt)


