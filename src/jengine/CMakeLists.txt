cmake_minimum_required(VERSION 3.16)
project(jengine C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(jengine_demo src/jengine.c demo/demo.c)

target_include_directories(jengine_demo PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(UNIX AND NOT APPLE)
    target_link_libraries(jengine_demo PRIVATE m)
endif()

enable_testing()
add_test(NAME jengine_test COMMAND jengine_demo)