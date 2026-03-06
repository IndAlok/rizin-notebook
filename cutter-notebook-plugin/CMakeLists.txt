cmake_minimum_required(VERSION 3.16)
project(CutterNotebookPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── Locate the Cutter installation ──────────────────────────────────────────
# Usage:
#   cmake -S . -B build -DCutter_DIR="C:/path/to/Cutter-v2.4.1-Windows-x86_64"
# Also pass -DCMAKE_PREFIX_PATH="C:/Qt/6.7.2/msvc2019_64" for Qt6 SDK.

if(NOT Cutter_DIR AND NOT DEFINED ENV{Cutter_DIR})
    message(FATAL_ERROR
        "Set Cutter_DIR to your Cutter installation root, e.g.:\n"
        "  cmake -S . -B build "
        "-DCutter_DIR=\"C:/path/to/Cutter-v2.4.1-Windows-x86_64\" "
        "-DCMAKE_PREFIX_PATH=\"C:/Qt/6.7.2/msvc2019_64\"\n"
    )
endif()

if(NOT Cutter_DIR)
    set(Cutter_DIR "$ENV{Cutter_DIR}")
endif()

file(TO_CMAKE_PATH "${Cutter_DIR}" CUTTER_ROOT)

# Normalise: if the user passed the cmake package dir, walk up.
if(EXISTS "${CUTTER_ROOT}/CutterTargets.cmake")
    get_filename_component(CUTTER_ROOT "${CUTTER_ROOT}/../../.." ABSOLUTE)
endif()

# Validate.
if(NOT EXISTS "${CUTTER_ROOT}/include/cutter/plugins/CutterPlugin.h")
    message(FATAL_ERROR
        "Cannot find CutterPlugin.h in '${CUTTER_ROOT}/include/cutter/plugins'.\n"
        "Set Cutter_DIR to the root of an extracted Cutter binary release.\n"
    )
endif()

message(STATUS "Cutter SDK root: ${CUTTER_ROOT}")

set(CUTTER_LIB_DIR    "${CUTTER_ROOT}/lib")
set(CUTTER_INCLUDE_DIR "${CUTTER_ROOT}/include")

# ── Find Qt6 from the system / CMAKE_PREFIX_PATH ───────────────────────────
# We need Qt6 headers, MOC, and import libraries from a proper SDK install.
# The Cutter binary release only ships runtime DLLs, not development files.
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets Gui Network)

# ── Manually create Cutter::Cutter imported target ──────────────────────────
# We bypass CutterConfig.cmake because it calls find_dependency(Qt6 …) for
# components (Svg, Core5Compat, etc.) that we don't need, causing failures.
add_library(Cutter::Cutter SHARED IMPORTED)
if(WIN32)
    set_target_properties(Cutter::Cutter PROPERTIES
        IMPORTED_IMPLIB "${CUTTER_LIB_DIR}/cutter.lib"
    )
elseif(APPLE)
    find_library(_cutter_lib NAMES cutter PATHS "${CUTTER_LIB_DIR}" NO_DEFAULT_PATH)
    set_target_properties(Cutter::Cutter PROPERTIES
        IMPORTED_LOCATION "${_cutter_lib}"
    )
else()
    find_library(_cutter_lib NAMES cutter PATHS "${CUTTER_LIB_DIR}" NO_DEFAULT_PATH)
    set_target_properties(Cutter::Cutter PROPERTIES
        IMPORTED_LOCATION "${_cutter_lib}"
    )
endif()

set_target_properties(Cutter::Cutter PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES
        "${CUTTER_INCLUDE_DIR}/cutter;${CUTTER_INCLUDE_DIR}/cutter/core;${CUTTER_INCLUDE_DIR}/cutter/widgets;${CUTTER_INCLUDE_DIR}/cutter/common;${CUTTER_INCLUDE_DIR}/cutter/plugins;${CUTTER_INCLUDE_DIR}/cutter/menus;${CUTTER_INCLUDE_DIR}/cutter/dialogs;${CUTTER_INCLUDE_DIR}/cutter/tools"
    INTERFACE_LINK_LIBRARIES
        "Qt6::Core;Qt6::Widgets;Qt6::Gui;Qt6::Network"
)

# ── Rizin (include paths for rz_core.h etc.) ───────────────────────────────
add_library(Cutter::Rizin INTERFACE IMPORTED)
set_target_properties(Cutter::Rizin PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CUTTER_INCLUDE_DIR}/librz;${CUTTER_INCLUDE_DIR}/librz/sdb"
)

# ── Plugin install directory ────────────────────────────────────────────────
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{APPDATA}" _appdata)
    set(CUTTER_INSTALL_PLUGDIR "${_appdata}/rizin/cutter/plugins/native"
        CACHE STRING "Cutter native plugin install directory")
elseif(APPLE)
    set(CUTTER_INSTALL_PLUGDIR "$ENV{HOME}/Library/Application Support/rizin/cutter/plugins/native"
        CACHE STRING "Cutter native plugin install directory")
else()
    set(CUTTER_INSTALL_PLUGDIR "$ENV{HOME}/.local/share/rizin/cutter/plugins/native"
        CACHE STRING "Cutter native plugin install directory")
endif()

# ── Plugin shared library ──────────────────────────────────────────────────
add_library(CutterNotebookPlugin MODULE
    src/CutterNotebookPlugin.cpp
    src/CutterNotebookPlugin.h
)

target_link_libraries(CutterNotebookPlugin PRIVATE
    Cutter::Cutter
    Cutter::Rizin
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
)

install(TARGETS CutterNotebookPlugin DESTINATION "${CUTTER_INSTALL_PLUGDIR}")
