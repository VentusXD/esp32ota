idf_component_register(
    SRCS "src/wifi_provisioning.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "webpage/index.html"
    REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash
)
