cmake_minimum_required(VERSION 3.20)
project(OpenChat VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Source files ─────────────────────────────────────────────────
set(SOURCES
    openchat.cpp
    chat_client.cpp
    chat_display.cpp
    chat_history.cpp
    app_state.cpp
    settings.cpp
    markdown_renderer.cpp
    theme.cpp
)

set(HEADERS
    chat_client.h
    chat_display.h
    chat_history.h
    app_state.h
    settings.h
    markdown_renderer.h
    theme.h
)

# ─── Find dependencies ───────────────────────────────────────────
# wxWidgets — requires: core, base, richtext, xml, html
find_package(wxWidgets REQUIRED COMPONENTS core base richtext xml html)
include(${wxWidgets_USE_FILE})

# Poco — requires: Foundation, Net, JSON, Util
find_package(Poco REQUIRED COMPONENTS Foundation Net JSON Util)

# ─── Executable ──────────────────────────────────────────────────
if(WIN32)
    # WIN32 flag creates a Windows GUI app (no console window)
    add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})
else()
    add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})
endif()

# ─── Link libraries ──────────────────────────────────────────────
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${wxWidgets_LIBRARIES}
    Poco::Foundation
    Poco::Net
    Poco::JSON
    Poco::Util
)

# ─── Version define ──────────────────────────────────────────────
target_compile_definitions(${PROJECT_NAME} PRIVATE
    OPENCHAT_VERSION="${PROJECT_VERSION}"
)

# ─── Windows-specific settings ───────────────────────────────────
if(WIN32)
    # Link ws2_32 and iphlpapi for Poco networking on Windows
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 iphlpapi)

    # Set the subsystem to Windows (suppresses console window)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    # Optional: embed an application icon via .rc resource file
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.rc")
        target_sources(${PROJECT_NAME} PRIVATE app_icon.rc)
    endif()
endif()

# ─── macOS bundle (optional) ─────────────────────────────────────
if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "OpenChat"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
endif()

# ─── Install rules ───────────────────────────────────────────────
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)
