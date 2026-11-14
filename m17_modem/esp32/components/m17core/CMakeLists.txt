# The M17 over-the-air layer shared with the PC tests (m17_modem/core) and the CRC / callsign code of m17_pc.
set(CORE "${CMAKE_CURRENT_LIST_DIR}/../../../core")
set(PCDIR "${CMAKE_CURRENT_LIST_DIR}/../../../../m17_pc")
idf_component_register(
    SRCS "${CORE}/m17_rf.c" "${CORE}/m17_modem.c" "${PCDIR}/m17.c"
    INCLUDE_DIRS "${CORE}" "${PCDIR}")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
