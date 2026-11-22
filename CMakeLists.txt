cmake_minimum_required(VERSION 3.22)
project(hexed_radar LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# The AML mod: libHexedRadar.so
add_library(HexedRadar SHARED src/radar.cpp)
target_include_directories(HexedRadar PRIVATE third_party/aml)
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_definitions(HexedRadar PRIVATE AML32)
endif()
target_link_libraries(HexedRadar PRIVATE log dl)
target_link_options(HexedRadar PRIVATE -Wl,--gc-sections)
