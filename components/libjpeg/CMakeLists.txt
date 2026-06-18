# Vendored IJG libjpeg v9f, decoder build. Memory is routed to PSRAM in
# jmemnobs.c. Provides baseline and progressive JPEG decoding via the standard
# libjpeg API (jpeglib.h). Consumed by cdc_image.

idf_component_register(
    SRCS
        "jaricom.c" "jcapimin.c" "jcapistd.c" "jcarith.c" "jccoefct.c" "jccolor.c"
        "jcdctmgr.c" "jchuff.c" "jcinit.c" "jcmainct.c" "jcmarker.c" "jcmaster.c"
        "jcomapi.c" "jcparam.c" "jcprepct.c" "jcsample.c" "jctrans.c" "jdapimin.c"
        "jdapistd.c" "jdarith.c" "jdatadst.c" "jdatasrc.c" "jdcoefct.c" "jdcolor.c"
        "jddctmgr.c" "jdhuff.c" "jdinput.c" "jdmainct.c" "jdmarker.c" "jdmaster.c"
        "jdmerge.c" "jdpostct.c" "jdsample.c" "jdtrans.c" "jerror.c" "jfdctflt.c"
        "jfdctfst.c" "jfdctint.c" "jidctflt.c" "jidctfst.c" "jidctint.c" "jquant1.c"
        "jquant2.c" "jutils.c" "jmemmgr.c" "jmem_psram.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        heap
)

# libjpeg is legacy C; silence its warnings so a strict project build is clean.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
