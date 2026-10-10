# CMakeLists.txt — LlamaBoss macOS build
# The Windows build continues to use LlamaBoss.vcxproj.
#
#   cmake -S . -B build_macos -G Ninja
#   cmake --build build_macos

cmake_minimum_required(VERSION 3.20)
project(LlamaBoss VERSION 0.1.21 LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ── wxWidgets (via wx-config) ──
find_program(WX_CONFIG NAMES wx-config-3.2 wx-config
    HINTS /opt/homebrew/bin /usr/local/bin REQUIRED)
execute_process(COMMAND ${WX_CONFIG} --cxxflags
    OUTPUT_VARIABLE WX_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${WX_CONFIG} --libs std,aui,html,richtext
    OUTPUT_VARIABLE WX_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(WX_CXXFLAGS_LIST UNIX_COMMAND "${WX_CXXFLAGS}")
# Keep "-framework X" pairs together; CMake de-duplicates bare "-framework" tokens.
string(REGEX REPLACE "-framework ([^ ]+)" "-Wl,-framework,\\1" WX_LIBS "${WX_LIBS}")
separate_arguments(WX_LIBS_LIST UNIX_COMMAND "${WX_LIBS}")

# ── Poco ──
find_package(Poco REQUIRED COMPONENTS Foundation JSON Net NetSSL Crypto Util XML Zip)

# ── Platform detection ──
if(WIN32)
    set(LLAMABOSS_PLATFORM "windows")
elseif(APPLE)
    set(LLAMABOSS_PLATFORM "macos")
elseif(UNIX)
    set(LLAMABOSS_PLATFORM "linux")
endif()
message(STATUS "Building for platform: ${LLAMABOSS_PLATFORM}")

# ── Platform abstraction library (src/platform) ──
add_library(llama_platform STATIC)
target_include_directories(llama_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)

if(LLAMABOSS_PLATFORM STREQUAL "windows")
    target_sources(llama_platform PRIVATE
        src/platform/str_util_win.cpp
        src/platform/fs_util_win.cpp
        src/platform/dir_walker_win.cpp
        src/platform/platform_dirs_win.cpp
        src/platform/secrets_backend_win.cpp
        src/platform/atomic_file_win.cpp
        src/platform/desktop_open_win.cpp
        src/platform/module_path_win.cpp
        src/platform/process_executor_win.cpp
        src/platform/process_io_win.cpp
    )
elseif(LLAMABOSS_PLATFORM STREQUAL "macos")
    target_sources(llama_platform PRIVATE
        src/platform/str_util_posix.cpp
        src/platform/fs_util_posix.cpp
        src/platform/dir_walker_posix.cpp
        src/platform/atomic_file_posix.cpp
        src/platform/process_executor_posix.cpp
        src/platform/process_io_posix.cpp
        src/platform/platform_dirs_macos.cpp
        src/platform/lb_process_posix.cpp
        src/platform/secrets_backend_macos.cpp
        src/platform/desktop_open_macos.cpp
        src/platform/module_path_macos.cpp
    )
    target_link_libraries(llama_platform PUBLIC
        "-framework Cocoa" "-framework CoreFoundation" "-framework Security")
elseif(LLAMABOSS_PLATFORM STREQUAL "linux")
    target_sources(llama_platform PRIVATE
        src/platform/str_util_posix.cpp
        src/platform/fs_util_posix.cpp
        src/platform/dir_walker_posix.cpp
        src/platform/atomic_file_posix.cpp
        src/platform/process_executor_posix.cpp
        src/platform/process_io_posix.cpp
        src/platform/platform_dirs_linux.cpp
        src/platform/lb_process_posix.cpp
        src/platform/secrets_backend_linux.cpp
        src/platform/desktop_open_linux.cpp
        src/platform/module_path_linux.cpp
    )
    target_link_libraries(llama_platform PUBLIC pthread dl)
endif()

# ── App sources: every root-level .cpp except tests and the PCH stub ──
file(GLOB APP_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER APP_SRCS EXCLUDE REGEX ".*_tests\\.cpp$")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/pch\\.cpp$")

add_executable(LlamaBoss ${APP_SRCS})

# pch.h is force-included in every TU on Windows (/FI); mirror that here.
target_precompile_headers(LlamaBoss PRIVATE pch.h)

target_include_directories(LlamaBoss PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(LlamaBoss PRIVATE ${WX_CXXFLAGS_LIST})
target_link_libraries(LlamaBoss PRIVATE
    llama_platform
    ${WX_LIBS_LIST}
    Poco::Foundation Poco::JSON Poco::Net Poco::NetSSL Poco::Crypto
    Poco::Util Poco::XML Poco::Zip
)

# ── macOS app bundle ──
if(APPLE)
    set_target_properties(LlamaBoss PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist
        MACOSX_BUNDLE_BUNDLE_NAME "LlamaBoss"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.llamaboss.app"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026"
    )
endif()

install(TARGETS LlamaBoss RUNTIME DESTINATION bin BUNDLE DESTINATION .)
