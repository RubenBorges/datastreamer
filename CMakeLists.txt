cmake_minimum_required(VERSION 3.20)
project(datastreamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src)



install(TARGETS datastreamer
    RUNTIME DESTINATION .
)
