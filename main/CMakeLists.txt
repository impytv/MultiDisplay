idf_component_register(
    SRCS
        "main.c"
        "../components/waveshare_rgb_lcd_port.c"
        "../components/app_config.c"
        "../components/wifi_provision.c"
        "../components/yr_client.c"
        "../components/adsb_client.c"
    INCLUDE_DIRS
        "."
        "../components"
    # Airport + runway table for the radar (see scripts/build_airports.py).
    EMBED_FILES
        "../components/airports.bin"
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_client
        esp_http_server
        esp_driver_gpio
        lwip
        esp-tls
        mbedtls
        json)

idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
elseif("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
endif()

if(lvgl_lib)
    target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
endif()

# Package the FreeType font (see fonts/OFL.txt for its license) and the
# MET Norway weather icons (fonts/*.png, from github.com/metno/weathericons,
# resized to 64x64) into the "fonts" SPIFFS partition. Mounted at runtime as
# an esp_lv_fs drive ("F:") rather than linked into the app image: LVGL v9's
# FreeType binding only accepts a file path, and the icons are then loaded
# the same way (F:<symbol_code>.png) via esp_lv_decoder.
spiffs_create_partition_assets(fonts "${CMAKE_CURRENT_SOURCE_DIR}/fonts"
    FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".ttf,.png"
    IMPORT_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(${COMPONENT_LIB} assets_fonts_bin)
# add_dependencies() alone only orders the library's link step; main.c
# #includes the generated header directly, so its compile step needs an
# explicit file-level dependency on the asset build's actual (ninja-tracked)
# output - the header itself is an untracked side effect of that same
# command, so depending on it directly gives ninja "no known rule" errors.
set_source_files_properties(main.c PROPERTIES
    OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/mmap_build/fonts/fonts/fonts.bin")
