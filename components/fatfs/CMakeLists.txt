# ESP-IDF's own fatfs component, built with exFAT and GPT on (see README.md
# here).
# Its FatFs configuration has exFAT off with a plain #define and no setting
# for it, and ff.h includes ffconf.h by a quoted name, so the one next to
# ff.h always wins: ff.c, ff.h, ffconf.h, ffunicode.c and diskio.h are
# copied from this ESP-IDF at configure time into the build directory, with
# that one line changed, and everything else is built from ESP-IDF's copy
# where it is. Nothing of FatFs is kept here, so an ESP-IDF upgrade brings
# its FatFs along; if the line isn't found the build stops rather than
# quietly going without exFAT.

idf_build_get_property(idf_path IDF_PATH)
set(idf_fatfs "${idf_path}/components/fatfs")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(gen "${CMAKE_CURRENT_BINARY_DIR}/fatfs_src")
    file(MAKE_DIRECTORY "${gen}")
    foreach(f ff.c ff.h ffunicode.c diskio.h)
        configure_file("${idf_fatfs}/src/${f}" "${gen}/${f}" COPYONLY)
    endforeach()
    file(READ "${idf_fatfs}/src/ffconf.h" conf)
    string(REGEX MATCH "\n#define FF_FS_EXFAT[ \t]+0\n" exfat_line "${conf}")
    if(NOT exfat_line)
        message(FATAL_ERROR "components/fatfs: FF_FS_EXFAT 0 not found in ${idf_fatfs}/src/ffconf.h")
    endif()
    # With exFAT, ff.c also uses FF_USE_LABEL in a C expression, not only
    # in #if, where ESP-IDF's CONFIG_FATFS_USE_LABEL is undefined when off.
    string(REPLACE "${exfat_line}"
           "\n#define FF_FS_EXFAT\t\t1 /* MultiDisplay: components/fatfs */\n#ifndef CONFIG_FATFS_USE_LABEL\n#define CONFIG_FATFS_USE_LABEL 0\n#endif\n"
           conf "${conf}")
    # GPT partition tables (a card formatted on a Mac, or by some cameras
    # and tools) are only read with 64-bit sector numbers on. ESP-IDF's
    # disk drivers take 32-bit ones, which reach 2 TB, more than any
    # microSD card; only formatting (f_mkfs/f_fdisk, never used here)
    # would read the card's size through them.
    string(REGEX MATCH "\n#define FF_LBA64[ \t]+0\n" lba_line "${conf}")
    if(NOT lba_line)
        message(FATAL_ERROR "components/fatfs: FF_LBA64 0 not found in ${idf_fatfs}/src/ffconf.h")
    endif()
    string(REPLACE "${lba_line}" "\n#define FF_LBA64\t\t1 /* MultiDisplay: components/fatfs */\n" conf "${conf}")
    file(CONFIGURE OUTPUT "${gen}/ffconf.h" CONTENT "${conf}" @ONLY)
    # Copied again when ESP-IDF's files change.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${idf_fatfs}/src/ffconf.h" "${idf_fatfs}/src/ff.c" "${idf_fatfs}/src/ff.h"
                 "${idf_fatfs}/src/ffunicode.c" "${idf_fatfs}/src/diskio.h")
else()
    set(gen "")
endif()

idf_component_register(SRCS "${gen}/ff.c"
                            "${gen}/ffunicode.c"
                            "${idf_fatfs}/diskio/diskio.c"
                            "${idf_fatfs}/diskio/diskio_rawflash.c"
                            "${idf_fatfs}/diskio/diskio_wl.c"
                            "${idf_fatfs}/diskio/diskio_sdmmc.c"
                            "${idf_fatfs}/port/freertos/ffsystem.c"
                            "${idf_fatfs}/vfs/vfs_fat.c"
                            "${idf_fatfs}/vfs/vfs_fat_sdmmc.c"
                            "${idf_fatfs}/vfs/vfs_fat_spiflash.c"
                       INCLUDE_DIRS "${gen}" "${idf_fatfs}/diskio" "${idf_fatfs}/vfs"
                       REQUIRES wear_levelling sdmmc esp_driver_sdmmc esp_driver_sdspi
                       PRIV_REQUIRES vfs esp_driver_gpio)

# The checks below need FatFs's long file names, which exFAT can't go
# without (sdkconfig.defaults has CONFIG_FATFS_LFN_HEAP).
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND CONFIG_FATFS_LFN_NONE)
    message(FATAL_ERROR "components/fatfs: exFAT needs long file names (CONFIG_FATFS_LFN_HEAP)")
endif()
