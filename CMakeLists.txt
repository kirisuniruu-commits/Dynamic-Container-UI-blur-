cmake_minimum_required(VERSION 3.22)
project(dynamic_container_ui_blur LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "Android-only target. Use the NDK toolchain (see .github/workflows/build.yml).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.2.2)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${ANDROID_ABI}")
FetchContent_MakeAvailable(preloader_android)

find_library(LOG_LIB log REQUIRED)
find_library(EGL_LIB EGL REQUIRED)
find_library(GLES_LIB GLESv3 REQUIRED)

add_library(dynamic_container_ui_blur SHARED
    src/BlurMod.cpp
    src/BlurRenderer.cpp
    src/ScreenDetector.cpp)

set_target_properties(dynamic_container_ui_blur PROPERTIES
    OUTPUT_NAME "dynamic_container_ui_blur")

target_include_directories(dynamic_container_ui_blur PRIVATE src)
target_compile_options(dynamic_container_ui_blur PRIVATE
    -Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(dynamic_container_ui_blur PRIVATE
    preloader
    nlohmann_json::nlohmann_json
    Boost::pfr
    magic_enum::magic_enum
    fmt::fmt
    ${LOG_LIB} ${EGL_LIB} ${GLES_LIB})
