cmake_minimum_required(VERSION 3.16)
project(rz-notebook C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Find Rizin ────────────────────────────────────────────────────────────
# Option A: Cutter SDK (provides Rizin libraries + headers).
#   cmake -S . -B build -DCUTTER_SDK_DIR="C:/path/to/Cutter-v2.4.1-Windows-x86_64"
#
# Option B: Build-tree mode (in-tree meson build).
#   cmake -S . -B build
#         -DRIZIN_SOURCE_DIR="C:/path/to/rizin"
#         -DRIZIN_BUILD_DIR="C:/path/to/rizin/build"
#
# Option C: Installed rizin — find_package(Rizin) just works.

set(CUTTER_SDK_DIR "" CACHE PATH "Path to Cutter SDK (contains lib/cmake/Rizin)")
set(RIZIN_SOURCE_DIR "" CACHE PATH "Path to rizin source tree")
set(RIZIN_BUILD_DIR ""  CACHE PATH "Path to rizin build directory")

if(CUTTER_SDK_DIR)
    # ── Cutter SDK mode ──
    message(STATUS "Using Cutter SDK: ${CUTTER_SDK_DIR}")
    set(CMAKE_PREFIX_PATH "${CUTTER_SDK_DIR}/lib/cmake/Rizin" ${CMAKE_PREFIX_PATH})
    find_package(Rizin COMPONENTS Core REQUIRED)
    set(RZ_LIBRARIES Rizin::Core)
    get_target_property(RZ_INCLUDE_DIRS Rizin::Core INTERFACE_INCLUDE_DIRECTORIES)
elseif(RIZIN_SOURCE_DIR AND RIZIN_BUILD_DIR)
    # ── Build-tree mode: use source/build directories directly ──
    message(STATUS "Using rizin build tree:")
    message(STATUS "  Source: ${RIZIN_SOURCE_DIR}")
    message(STATUS "  Build:  ${RIZIN_BUILD_DIR}")

    set(RZ_INCLUDE_DIRS
        "${RIZIN_SOURCE_DIR}/librz/include"
        "${RIZIN_SOURCE_DIR}/librz/util/sdb/src"
        "${RIZIN_SOURCE_DIR}/subprojects/rzheap"
        "${RIZIN_BUILD_DIR}"
        "${RIZIN_BUILD_DIR}/librz"
    )

    set(RZ_LIBRARY_DIRS
        "${RIZIN_BUILD_DIR}/librz/core"
        "${RIZIN_BUILD_DIR}/librz/util"
        "${RIZIN_BUILD_DIR}/librz/config"
        "${RIZIN_BUILD_DIR}/librz/cons"
        "${RIZIN_BUILD_DIR}/librz/io"
        "${RIZIN_BUILD_DIR}/librz/cmd"
    )

    find_library(RZ_CORE_LIB NAMES rz_core HINTS "${RIZIN_BUILD_DIR}/librz/core" REQUIRED)
    find_library(RZ_UTIL_LIB NAMES rz_util HINTS "${RIZIN_BUILD_DIR}/librz/util" REQUIRED)
    find_library(RZ_CONS_LIB NAMES rz_cons HINTS "${RIZIN_BUILD_DIR}/librz/cons" REQUIRED)
    find_library(RZ_CONFIG_LIB NAMES rz_config HINTS "${RIZIN_BUILD_DIR}/librz/config" REQUIRED)
    find_library(RZ_IO_LIB NAMES rz_io HINTS "${RIZIN_BUILD_DIR}/librz/io" REQUIRED)

    set(RZ_LIBRARIES ${RZ_CORE_LIB} ${RZ_UTIL_LIB} ${RZ_CONS_LIB} ${RZ_CONFIG_LIB} ${RZ_IO_LIB})
    message(STATUS "  Libraries: ${RZ_LIBRARIES}")
else()
    # ── Installed mode ──
    find_package(Rizin COMPONENTS Core REQUIRED)
    get_target_property(RZ_INCLUDE_DIRS Rizin::Core INTERFACE_INCLUDE_DIRECTORIES)
    set(RZ_LIBRARIES Rizin::Core)
endif()

# ── Find protobuf-c ──────────────────────────────────────────────────────
# Either via CMake config (vcpkg), pkg-config, or manual path.
find_package(protobuf-c CONFIG QUIET)
if(protobuf-c_FOUND)
    message(STATUS "Found protobuf-c via CMake config")
    set(PROTOBUF_C_LIBS protobuf-c::protobuf-c)
else()
    # Fallback: manual search.
    find_library(PROTOBUF_C_LIB NAMES protobuf-c REQUIRED)
    find_path(PROTOBUF_C_INC NAMES protobuf-c/protobuf-c.h REQUIRED)
    set(PROTOBUF_C_LIBS ${PROTOBUF_C_LIB})
    message(STATUS "Found protobuf-c: ${PROTOBUF_C_LIB}")
endif()

# Resolve protobuf-c runtime DLL path for Windows packaging/install.
set(PROTOBUF_C_RUNTIME_DLL "")
if(WIN32)
    if(TARGET protobuf-c::protobuf-c)
        get_target_property(_pb_dll_rel protobuf-c::protobuf-c IMPORTED_LOCATION_RELEASE)
        get_target_property(_pb_dll_dbg protobuf-c::protobuf-c IMPORTED_LOCATION_DEBUG)
        get_target_property(_pb_dll_any protobuf-c::protobuf-c IMPORTED_LOCATION)
        if(_pb_dll_rel)
            set(PROTOBUF_C_RUNTIME_DLL "${_pb_dll_rel}")
        elseif(_pb_dll_any)
            set(PROTOBUF_C_RUNTIME_DLL "${_pb_dll_any}")
        elseif(_pb_dll_dbg)
            set(PROTOBUF_C_RUNTIME_DLL "${_pb_dll_dbg}")
        endif()
    endif()

    # vcpkg fallback for common setup.
    if(NOT PROTOBUF_C_RUNTIME_DLL AND EXISTS "C:/vcpkg/installed/x64-windows/bin/protobuf-c.dll")
        set(PROTOBUF_C_RUNTIME_DLL "C:/vcpkg/installed/x64-windows/bin/protobuf-c.dll")
    endif()

    if(PROTOBUF_C_RUNTIME_DLL)
        message(STATUS "protobuf-c runtime DLL: ${PROTOBUF_C_RUNTIME_DLL}")
    else()
        message(WARNING "protobuf-c runtime DLL path not found; rz_notebook.dll may fail to load unless protobuf-c.dll is in PATH")
    endif()
endif()

# ── WinHTTP (Windows) or libcurl (Unix) ──────────────────────────────────
if(WIN32)
    set(HTTP_LIBS winhttp)
else()
    find_package(CURL REQUIRED)
    set(HTTP_LIBS CURL::libcurl)
endif()

# ── Plugin shared library ─────────────────────────────────────────────────
add_library(rz_notebook MODULE
    src/rz_notebook.c
    src/nb_http.c
    src/nb_http.h
    src/nb_server.c
    src/nb_server.h
    src/notebook.pb-c.c
    src/notebook.pb-c.h
)

target_link_libraries(rz_notebook PRIVATE
    ${RZ_LIBRARIES}
    ${PROTOBUF_C_LIBS}
    ${HTTP_LIBS}
)

target_include_directories(rz_notebook PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${RZ_INCLUDE_DIRS}
)

# On Windows with shared protobuf-c (DLL), PROTOBUF_C_USE_SHARED_LIB must be
# defined so the header provides a local static copy of protobuf_c_empty_string
# instead of referencing an unresolved extern symbol.
if(WIN32 AND NOT protobuf-c_FOUND)
    # Manual find — assume shared
    target_compile_definitions(rz_notebook PRIVATE PROTOBUF_C_USE_SHARED_LIB)
elseif(WIN32)
    # vcpkg shared libs: check if DLL exists
    if(EXISTS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/protobuf-c.dll"
       OR EXISTS "C:/vcpkg/installed/x64-windows/bin/protobuf-c.dll")
        target_compile_definitions(rz_notebook PRIVATE PROTOBUF_C_USE_SHARED_LIB)
    endif()
endif()

# Suppress warnings in generated protobuf-c code.
if(MSVC)
    set_source_files_properties(src/notebook.pb-c.c PROPERTIES
        COMPILE_FLAGS "/wd4146 /wd4267 /wd4244")
endif()

# ── Plugin install directory ─────────────────────────────────────────────
if(WIN32)
    file(TO_CMAKE_PATH "$ENV{APPDATA}" _appdata)
    set(RZ_INSTALL_PLUGDIR "${_appdata}/rizin/plugins"
        CACHE STRING "Rizin plugin install directory")
else()
    set(RZ_INSTALL_PLUGDIR "$ENV{HOME}/.local/lib/rizin/plugins"
        CACHE STRING "Rizin plugin install directory")
endif()

install(TARGETS rz_notebook
    LIBRARY DESTINATION "${RZ_INSTALL_PLUGDIR}"
    RUNTIME DESTINATION "${RZ_INSTALL_PLUGDIR}"
)

if(WIN32 AND PROTOBUF_C_RUNTIME_DLL)
    # Keep plugin self-contained at install destination.
    install(FILES "${PROTOBUF_C_RUNTIME_DLL}" DESTINATION "${RZ_INSTALL_PLUGDIR}")

    # Also place runtime next to build output DLL for manual copy workflows and CI artifacts.
    add_custom_command(TARGET rz_notebook POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROTOBUF_C_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:rz_notebook>/protobuf-c.dll"
        VERBATIM
    )
endif()

message(STATUS "Rizin plugin install dir: ${RZ_INSTALL_PLUGDIR}")
