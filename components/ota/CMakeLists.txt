idf_component_register(SRCS "ota.c"
    "FWTP/fwtp.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bootloader_support app_update)
