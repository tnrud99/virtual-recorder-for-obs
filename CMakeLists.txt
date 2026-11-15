cmake_minimum_required(VERSION 3.28)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND
   NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
        "Minimum supported macOS version")
endif()

project(virtuedit-obs VERSION 1.0.0)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- libwebsockets (static, no SSL, localhost-only) ---
include(FetchContent)
FetchContent_Declare(libwebsockets
    GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
    GIT_TAG        v4.3.10
)
set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
set(LWS_WITH_SSL OFF CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_SERVER ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_PING ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_CLIENT ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libwebsockets)

# --- Paths ---
set(OBS_SOURCE_DIR "$ENV{OBS_SOURCE_DIR}" CACHE PATH "OBS source directory (headers)")
set(OBS_LIB_DIR "$ENV{OBS_LIB_DIR}" CACHE PATH "OBS library directory")

if(APPLE AND NOT OBS_SOURCE_DIR)
    if(EXISTS "/tmp/obs-studio-32.1.2/libobs/obs-module.h")
        set(OBS_SOURCE_DIR "/tmp/obs-studio-32.1.2" CACHE PATH "" FORCE)
    else()
        set(OBS_SOURCE_DIR "/tmp/obs-studio-headers" CACHE PATH "" FORCE)
    endif()
endif()
if(APPLE AND NOT OBS_LIB_DIR)
    set(OBS_LIB_DIR "/Applications/OBS.app/Contents/Frameworks" CACHE PATH "" FORCE)
endif()

if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs-module.h")
    message(FATAL_ERROR
        "OBS headers are incomplete: ${OBS_SOURCE_DIR}/libobs/obs-module.h was not found. "
        "Set OBS_SOURCE_DIR to an OBS Studio source checkout matching your installed OBS version.")
endif()
if(EXISTS "${OBS_SOURCE_DIR}/UI/obs-frontend-api/obs-frontend-api.h")
    set(OBS_FRONTEND_INCLUDE_DIR "${OBS_SOURCE_DIR}/UI/obs-frontend-api")
elseif(EXISTS "${OBS_SOURCE_DIR}/frontend/api/obs-frontend-api.h")
    set(OBS_FRONTEND_INCLUDE_DIR "${OBS_SOURCE_DIR}/frontend/api")
else()
    message(FATAL_ERROR
        "OBS frontend headers are incomplete: obs-frontend-api.h was not found under "
        "${OBS_SOURCE_DIR}/UI/obs-frontend-api or ${OBS_SOURCE_DIR}/frontend/api. "
        "Set OBS_SOURCE_DIR to an OBS Studio source checkout matching your installed OBS version.")
endif()

find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h
    HINTS "$ENV{SIMDE_INCLUDE_DIR}" /opt/homebrew/include /usr/local/include
)
if(NOT SIMDE_INCLUDE_DIR)
    message(FATAL_ERROR
        "simde headers were not found. Install simde (for example: brew install simde) "
        "or set SIMDE_INCLUDE_DIR to a directory containing simde/x86/sse2.h.")
endif()

set(OBS_DATA_PATH "data/obs-studio")
set(OBS_PLUGIN_PATH "obs-plugins")
set(OBS_PLUGIN_DESTINATION "obs-plugins")
set(OBS_RELEASE_CANDIDATE 0)
set(OBS_BETA 0)
configure_file(
    "${OBS_SOURCE_DIR}/libobs/obsconfig.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/obs-generated/obsconfig.h"
)

# --- Qt6 ---
# Compile against the same Qt minor release bundled with the supported OBS
# release. Qt is not packaged with this plugin; OBS supplies it at runtime.
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)
qt6_standard_project_setup()

# --- Plugin sources ---
add_library(${PROJECT_NAME} MODULE
    src/plugin-main.cpp
    src/dock-widget.cpp
    src/dock-widget.hpp
    src/recorder-update.cpp
    src/websocket-server.cpp
    src/websocket-server.hpp
    src/youtube-controller.cpp
    src/youtube-controller.hpp
    src/youtube-url.hpp
    src/scene-manager.cpp
    src/scene-manager.hpp
    src/json-export.cpp
    src/json-export.hpp
    src/ffmpeg-extract.cpp
    src/ffmpeg-extract.hpp
    src/webcam-recorder.cpp
    src/webcam-recorder.hpp
    src/reaction-encoder-settings.hpp
    src/audio-track-selection.hpp
    src/obs-utils.hpp
)

# OBS supplies Qt at runtime, so do not request a separate Qt deployment.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    QT_NO_VERSION_TAGGING
    QT_NO_KEYWORDS
    VIRTUEDIT_VERSION="${PROJECT_VERSION}"
)

# --- Includes ---
target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}/obs-generated
    ${OBS_SOURCE_DIR}/libobs
    ${OBS_FRONTEND_INCLUDE_DIR}
    ${OBS_SOURCE_DIR}/UI
    ${SIMDE_INCLUDE_DIR}
)

# --- Linking ---
if(APPLE)
    # Link OBS libs via linker flags to prevent -F propagation
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE
        Qt6::Widgets
        Qt6::Core
        Qt6::Network
        websockets
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        "${OBS_LIB_DIR}/libobs.framework/Versions/A/libobs"
        "${OBS_LIB_DIR}/obs-frontend-api.dylib"
    )
else()
    find_library(LIBOBS_LIB NAMES obs libobs PATHS "${OBS_LIB_DIR}" NO_DEFAULT_PATH)
    find_library(OBS_FRONTEND_LIB NAMES obs-frontend-api PATHS "${OBS_LIB_DIR}" NO_DEFAULT_PATH)

    if(NOT LIBOBS_LIB)
        message(FATAL_ERROR
            "OBS import library was not found in ${OBS_LIB_DIR}. "
            "Set OBS_LIB_DIR to a directory containing obs.lib.")
    endif()
    if(NOT OBS_FRONTEND_LIB)
        message(FATAL_ERROR
            "OBS frontend import library was not found in ${OBS_LIB_DIR}. "
            "Set OBS_LIB_DIR to a directory containing obs-frontend-api.lib.")
    endif()

    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${LIBOBS_LIB}
        ${OBS_FRONTEND_LIB}
        Qt6::Widgets
        Qt6::Core
        Qt6::Network
        websockets
    )

    if(WIN32)
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
        target_link_libraries(${PROJECT_NAME} PRIVATE
            ws2_32
            iphlpapi
        )
    endif()
endif()

# --- Output ---
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "virtuedit-obs"
)

install(TARGETS ${PROJECT_NAME} DESTINATION bin COMPONENT virtuedit)
install(DIRECTORY data/ DESTINATION data COMPONENT virtuedit)

if(BUILD_TESTING)
    add_executable(youtube-url-parser-test
        tests/youtube-url-parser.cpp
    )
    target_include_directories(youtube-url-parser-test PRIVATE src)
    set_target_properties(youtube-url-parser-test PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    add_test(NAME youtube-url-parser COMMAND youtube-url-parser-test)

    add_executable(audio-track-selection-test
        tests/audio-track-selection.cpp
    )
    target_include_directories(audio-track-selection-test PRIVATE src)
    set_target_properties(audio-track-selection-test PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    add_test(NAME audio-track-selection COMMAND audio-track-selection-test)
endif()
