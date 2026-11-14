# Codec 2 (LGPL-2.1, https://github.com/drowe67/codec2 v1.2.0) built from the source tree in %USERPROFILE%\tools\codec2
# (the codebooks are the generated ones of its PC build, build_w64\src). Only what the 3200 and 1600 modes need.
file(TO_CMAKE_PATH "$ENV{USERPROFILE}/tools/codec2" C2)
set(C2S "${C2}/src")
set(C2G "${C2}/build_w64/src")
idf_component_register(
    SRCS "${C2S}/codec2.c" "${C2S}/codec2_fft.c" "${C2S}/kiss_fft.c" "${C2S}/kiss_fftr.c" "${C2S}/lpc.c"
         "${C2S}/lsp.c" "${C2S}/nlp.c" "${C2S}/phase.c" "${C2S}/postfilter.c" "${C2S}/quantise.c" "${C2S}/sine.c"
         "${C2S}/interp.c" "${C2S}/pack.c" "${C2S}/mbest.c" "${C2S}/newamp1.c" "${C2S}/lpcnet_freq.c"
         "${C2G}/codebook.c" "${C2G}/codebookd.c" "${C2G}/codebookge.c" "${C2G}/codebookjmv.c"
         "${C2G}/codebooknewamp1.c" "${C2G}/codebooknewamp1_energy.c"
         "${C2G}/codebooknewamp2.c" "${C2G}/codebooknewamp2_energy.c"
    INCLUDE_DIRS "${C2S}" "${C2}/build_w64")
target_compile_options(${COMPONENT_LIB} PRIVATE -w -O2)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CODEC2_MODE_EN_DEFAULT=0 CODEC2_MODE_3200_EN=1 CODEC2_MODE_1600_EN=1)
