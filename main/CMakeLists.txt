set(priv_requires fatfs console )

if(CONFIG_EXAMPLE_STORAGE_MEDIA_SPIFLASH)
    list(APPEND priv_requires wear_levelling esp_partition)
else()
    list(APPEND priv_requires esp_partition app_update)
endif()

idf_component_register(
    SRCS tusb_msc_main.c spi_api.c ota_c6_sdcard.c custom_sdmmc_cmd.c
    INCLUDE_DIRS .
    PRIV_REQUIRES "${priv_requires}"
)
