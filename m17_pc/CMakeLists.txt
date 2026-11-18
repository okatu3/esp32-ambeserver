# m17_pc: a software-only M17 station for M17 reflectors, see README.md. Codec 2 (LGPL-2.1, built separately as a
# static library) does the voice; nothing else of this repository is used. Build with m17_pc\build.bat.
cmake_minimum_required(VERSION 3.16)
project(m17_pc C RC)

set(CODEC2_DIR "$ENV{USERPROFILE}/tools/codec2" CACHE PATH "Codec 2 source tree (built in build_w64)")
set(CODEC2_BUILD "${CODEC2_DIR}/build_w64" CACHE PATH "Codec 2 build tree with src/libcodec2.a")
if(NOT EXISTS "${CODEC2_BUILD}/src/libcodec2.a")
    message(FATAL_ERROR "libcodec2.a not found in ${CODEC2_BUILD}/src: build Codec 2 first (see README.md)")
endif()
add_library(codec2 STATIC IMPORTED)
set_target_properties(codec2 PROPERTIES IMPORTED_LOCATION "${CODEC2_BUILD}/src/libcodec2.a"
                      INTERFACE_INCLUDE_DIRECTORIES "${CODEC2_DIR}/src;${CODEC2_BUILD}")

# radio mode: the M17 over-the-air layer and 4FSK modem of m17_modem/core (also used by the ESP32-A1S modem)
set(RF_CORE "${CMAKE_CURRENT_LIST_DIR}/../m17_modem/core")
add_library(m17_core STATIC m17.c m17_engine.c m17_parrot.c license_check.c audio_win.c mic_win.c wav.c radio_win.c fm_path.c fm_eq.c
            "${RF_CORE}/m17_rf.c" "${RF_CORE}/m17_modem.c")
target_include_directories(m17_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${RF_CORE}")
target_compile_definitions(m17_core PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
target_link_libraries(m17_core PUBLIC codec2 ws2_32 winmm)

add_executable(m17_cli m17_cli.c)
target_link_libraries(m17_cli PRIVATE m17_core)

add_executable(m17_test m17_test.c)
target_link_libraries(m17_test PRIVATE m17_core)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/m17_gui.c")
    add_executable(m17_pc WIN32 m17_gui.c m17_pc.rc)
    target_link_libraries(m17_pc PRIVATE m17_core comctl32 shell32)
    add_custom_command(TARGET m17_pc POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
                       "${CMAKE_CURRENT_LIST_DIR}/hosts" "$<TARGET_FILE_DIR:m17_pc>/hosts")
    target_link_options(m17_pc PRIVATE -municode)
endif()

foreach(t m17_core m17_cli m17_test m17_pc)
    if(TARGET ${t})
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
        if(NOT t STREQUAL "m17_core")
            target_link_options(${t} PRIVATE -static)
        endif()
    endif()
endforeach()
