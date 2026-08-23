cmake_minimum_required(VERSION 3.16)
project(openweb LANGUAGES C CXX)

include(FetchContent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo pangocairo)

FetchContent_Declare(
    litehtml
    GIT_REPOSITORY https://github.com/litehtml/litehtml.git
    GIT_TAG master
)
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LITEHTML_ENABLE_LINT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(litehtml)

add_library(litehtml_cairo STATIC
    ${litehtml_SOURCE_DIR}/containers/cairo/cairo_borders.cpp
    ${litehtml_SOURCE_DIR}/containers/cairo/conic_gradient.cpp
    ${litehtml_SOURCE_DIR}/containers/cairo/container_cairo.cpp
    ${litehtml_SOURCE_DIR}/containers/cairo/container_cairo_pango.cpp
)
target_include_directories(litehtml_cairo PRIVATE
    ${litehtml_SOURCE_DIR}/containers/cairo
)
target_link_libraries(litehtml_cairo PUBLIC litehtml PkgConfig::CAIRO)
target_compile_features(litehtml_cairo PUBLIC cxx_std_17)

add_executable(openweb
    src/openweb.c
    src/litehtml_renderer.cpp
)
target_include_directories(openweb PRIVATE
    ${litehtml_SOURCE_DIR}/include
    ${litehtml_SOURCE_DIR}/containers/cairo
)
target_link_libraries(openweb PRIVATE litehtml_cairo)
set_target_properties(openweb PROPERTIES CXX_STANDARD 17 C_STANDARD 11)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(openweb PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
