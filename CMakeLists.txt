cmake_minimum_required(VERSION 3.16)
project(hyprule LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find GTK4 via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)

add_executable(hyprule src/main.cpp)

target_include_directories(hyprule PRIVATE ${GTK4_INCLUDE_DIRS})
target_link_libraries(hyprule PRIVATE ${GTK4_LIBRARIES})
target_compile_options(hyprule PRIVATE ${GTK4_CFLAGS_OTHER})
target_link_directories(hyprule PRIVATE ${GTK4_LIBRARY_DIRS})
