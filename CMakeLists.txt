cmake_minimum_required(VERSION 3.10)
project(zoom CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_ANDROID_STL_TYPE c++_shared)

add_library(zoom SHARED src/main.cpp)

target_compile_options(zoom PRIVATE -fno-exceptions -fno-rtti -Os -ffunction-sections -fdata-sections)
target_link_options(zoom PRIVATE -Wl,--gc-sections -Wl,-s)
target_link_libraries(zoom PRIVATE dl log GLESv2 EGL)
