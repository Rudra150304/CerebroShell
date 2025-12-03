cmake_minimum_required(VERSION 3.10)
project(CerebroShell LANGUAGES C CXX)

# Use clang++
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenGL preference
set(OpenGL_GL_PREFERENCE GLVND)

# Ensure glad.c is compiled as C
set_source_files_properties(glad.c PROPERTIES LANGUAGE C)

# Sources
set(SOURCES
    CerebroShell.cc
    glad.c
)

# Include dirs
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    /usr/include/freetype2
)

# Packages
find_package(Freetype REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(CerebroShell ${SOURCES})

target_link_libraries(CerebroShell
    ${FREETYPE_LIBRARIES}
    OpenGL::GL
    glfw
    dl
    pthread
    X11
    Xrandr
    Xi
)

