idf_component_register(SRCS "ota_github.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_client esp_https_ota esp-tls app_update esp_app_format esp_netif)
