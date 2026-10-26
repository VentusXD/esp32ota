#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_provisioning.h"
#include "ota_github.h"
#include "esp_app_desc.h"

static const char *TAG = "TB_MAIN ";

void app_main(void){
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_provisioning_init();
    //ble_simple_init();  // Blue muerto
    wifi_provisioning_start_webserver();

    ota_github_start("VentusXD", "esp32ota");   // <-- la línea de la OTA
    /*
    CONFIG_PARTITION_TABLE_CUSTOM=y
    CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
    CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
    CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y*/

    // Tu aplicación
    for (;;) {
        ESP_LOGI(TAG, "App corriendo, versión %s", esp_app_get_description()->version);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

}