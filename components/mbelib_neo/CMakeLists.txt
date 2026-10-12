# ESP-IDF wrapper around mbelib-neo (GPL-2.0-or-later, fetched by tools/get_mbelib.sh, NOT bundled).
#
# Differences from v0.1:
#  * sources are collected automatically (no hand-written list that breaks when upstream renames files)
#  * mbelib-neo's public header includes <mbelib-neo/version.h>, which upstream generates at CMake time;
#    this wrapper generates it as well
#  * vendor code is built with -w so ESP-IDF's default -Werror=all cannot fail the build on third-party warnings

set(MBELIB_ROOT "${CMAKE_CURRENT_LIST_DIR}/vendor/mbelib-neo")

if(NOT EXISTS "${MBELIB_ROOT}/include/mbelib-neo/mbelib.h")
    message(FATAL_ERROR
        "mbelib-neo is missing. Run tools/get_mbelib.sh (or tools/get_mbelib.ps1) from the project root.")
endif()

# ---- sources ---------------------------------------------------------------------------------
file(GLOB_RECURSE MBELIB_ALL_C "${MBELIB_ROOT}/src/*.c")
set(MBELIB_SOURCES "")
foreach(f ${MBELIB_ALL_C})
    if(f MATCHES "/(tests?|bench|benchmarks?|examples?|fuzz)/"
       OR f MATCHES "_(double|test|bench|sse2?|avx2?|neon)\\.c$")
        continue()
    endif()
    list(APPEND MBELIB_SOURCES "${f}")
endforeach()
if(NOT MBELIB_SOURCES)
    message(FATAL_ERROR "No mbelib-neo sources found under ${MBELIB_ROOT}/src")
endif()

# ---- generated <mbelib-neo/version.h> ----------------------------------------------------------
set(MBELIB_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MBELIB_GEN_HDR "${MBELIB_GEN_DIR}/mbelib-neo/version.h")

if(NOT EXISTS "${MBELIB_ROOT}/include/mbelib-neo/version.h")
    set(_maj 2) 
    set(_min 2)
    set(_pat 0)
    file(READ "${MBELIB_ROOT}/CMakeLists.txt" _top)
    if(_top MATCHES "project\\(mbelib-neo[^)]*VERSION[ \t\r\n]+([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(_maj ${CMAKE_MATCH_1})
        set(_min ${CMAKE_MATCH_2})
        set(_pat ${CMAKE_MATCH_3})
    endif()
    set(PROJECT_VERSION "${_maj}.${_min}.${_pat}")
    set(PROJECT_VERSION_MAJOR ${_maj})
    set(PROJECT_VERSION_MINOR ${_min})
    set(PROJECT_VERSION_PATCH ${_pat})
    set(MBELIB_VERSION "${PROJECT_VERSION}")
    set(MBELIB_VERSION_MAJOR ${_maj})
    set(MBELIB_VERSION_MINOR ${_min})
    set(MBELIB_VERSION_PATCH ${_pat})

    file(GLOB_RECURSE _vin "${MBELIB_ROOT}/*version.h.in")
    file(MAKE_DIRECTORY "${MBELIB_GEN_DIR}/mbelib-neo")
    if(_vin)
        list(GET _vin 0 _vin0)
        configure_file("${_vin0}" "${MBELIB_GEN_HDR}")
    else()
        file(WRITE "${MBELIB_GEN_HDR}"
"#pragma once
#define MBELIB_VERSION \"${PROJECT_VERSION}\"
#define MBELIB_VERSION_MAJOR ${_maj}
#define MBELIB_VERSION_MINOR ${_min}
#define MBELIB_VERSION_PATCH ${_pat}
")
    endif()
endif()

idf_component_register(
    SRCS ${MBELIB_SOURCES}
    INCLUDE_DIRS
        "${MBELIB_ROOT}/include"
        "${MBELIB_GEN_DIR}"
    PRIV_INCLUDE_DIRS
        "${MBELIB_ROOT}"
        "${MBELIB_ROOT}/src"
        "${MBELIB_ROOT}/src/internal"
        "${MBELIB_ROOT}/src/external/pffft"
)

# Scalar PFFFT: no x86/ARM SIMD assumptions on Xtensa.
target_compile_definitions(${COMPONENT_LIB} PRIVATE PFFFT_SIMD_DISABLE=1 MBE_BUILDING=1)

# No thread-local storage: ESP-IDF reserves the whole TLS block (~52 KB of mbelib DCT caches) inside
# EVERY task's stack, so even the 1 KB IPC task cannot be created and the chip reboots at start-up
# (assert esp_ipc_init). The codec is only used from one task, so plain statics are fine.
# Needs the "#if defined(MBE_THREAD_LOCAL)" guard in vendor/.../src/internal/mbe_compiler.h.
target_compile_definitions(${COMPONENT_LIB} PRIVATE "MBE_THREAD_LOCAL=")
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
