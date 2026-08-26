cmake_minimum_required(VERSION 3.22)

project(smallwindow_hyos LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "smallwindow_hyos must use the Android NDK toolchain")
endif()

if(NOT "${ANDROID_ABI}" STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported; got '${ANDROID_ABI}'")
endif()

if(NOT DEFINED SMALLWINDOW_DIAGNOSTIC)
    set(SMALLWINDOW_DIAGNOSTIC 0)
endif()

add_library(smallwindow_hyos SHARED smallwindow_native.cpp)

target_compile_features(smallwindow_hyos PRIVATE cxx_std_17)
target_compile_definitions(smallwindow_hyos PRIVATE
    SMALLWINDOW_DIAGNOSTIC=${SMALLWINDOW_DIAGNOSTIC}
)
target_compile_options(smallwindow_hyos PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections
    -mbranch-protection=standard
    -Wall
    -Wextra
    -Werror
)
target_link_options(smallwindow_hyos PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map"
    -Wl,--gc-sections
    -Wl,--no-undefined
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,max-page-size=16384
    -Wl,-z,common-page-size=16384
)
target_link_libraries(smallwindow_hyos PRIVATE android log dl)

set_target_properties(smallwindow_hyos PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "smallwindow_hyos"
)
