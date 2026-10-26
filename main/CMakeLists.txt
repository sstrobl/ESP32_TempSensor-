idf_component_register(
    SRCS "main.c" "kty.c" "config_store.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash mqtt esp_adc esp_partition wear_levelling fatfs
    )
