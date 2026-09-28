
# ---------------------------------------------------------
# Dependencies
# ---------------------------------------------------------
find_package(simdjson CONFIG REQUIRED)


# ---------------------------------------------------------
# Source files
# ---------------------------------------------------------
set(DS_SOURCES
    streamingpipeline.cpp
    datastreamer.cpp    

    include/streamingpipeline.hpp
)


add_executable(datastreamer ${DS_SOURCES})

target_include_directories(datastreamer PRIVATE 
${CMAKE_CURRENT_SOURCE_DIR}/include
/usr/include
/usr/local/include
)

# ---------------------------------------------------------
# Link dependencies
# ---------------------------------------------------------
target_link_libraries(datastreamer PRIVATE simdjson::simdjson -luring -llz4)
