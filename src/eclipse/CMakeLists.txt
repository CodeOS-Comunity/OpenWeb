cmake_minimum_required(VERSION 3.16)
project(eclipse LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)

# litehtml provides the HTML/CSS layout core.
add_subdirectory(litehtml)

# Cairo drawing container shipped inside litehtml.
set(ECLIPSE_CONTAINER_SOURCES
    litehtml/containers/cairo/container_cairo.cpp
    litehtml/containers/cairo/cairo_borders.cpp
    litehtml/containers/cairo/conic_gradient.cpp
)

add_library(eclipse STATIC
    src/eclipse.cpp
    ${ECLIPSE_CONTAINER_SOURCES}
)

target_include_directories(eclipse PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(eclipse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(eclipse PRIVATE
    litehtml
    PkgConfig::CAIRO
)

# Give eclipse's public header a home so demo/ can #include <eclipse/eclipse.h>.
# (include dir is already PUBLIC above.)

# ---- demo: renders an HTML string to out.png ----
add_executable(eclipse_demo demo/demo.c)
target_link_libraries(eclipse_demo PRIVATE eclipse PkgConfig::CAIRO)

enable_testing()
add_test(NAME eclipse_demo COMMAND eclipse_demo)
