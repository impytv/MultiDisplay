# fatfs (exFAT)

ESP-IDF's own `fatfs` component, built with exFAT switched on, so the
display reads and writes memory cards as they come (cards over 32 GB are
sold formatted exFAT). A component here takes the place of ESP-IDF's of the
same name.

ESP-IDF has no setting for exFAT: `src/ffconf.h` says `#define FF_FS_EXFAT 0`,
and `ff.h` includes `ffconf.h` by a quoted name, so the copy beside `ff.h`
is always the one used. `CMakeLists.txt` therefore copies FatFs's `src/`
files from the ESP-IDF in use into the build directory at configure time,
with that one line set to 1, and builds the rest (disk I/O, VFS, port) from
ESP-IDF's own files where they are. Nothing of FatFs is kept in this
repository, so it follows ESP-IDF upgrades; if the line is ever not there
the build stops with an error rather than going on without exFAT.

GPT partition tables are read too (`FF_LBA64` set to 1 the same way): cards
formatted on a Mac, among others, have one instead of an MBR. ESP-IDF's
disk drivers still take 32-bit sector numbers, enough for 2 TB; only
formatting from the display (`f_mkfs`, never used) would be affected.

exFAT needs FatFs's long file names (`CONFIG_FATFS_LFN_HEAP` in
`sdkconfig.defaults`), with names in UTF-8 (`CONFIG_FATFS_API_ENCODING_UTF_8`).
