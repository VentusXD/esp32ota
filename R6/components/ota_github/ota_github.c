#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "ota_github.h"

#include <time.h> // Periodico
#include "esp_netif_sntp.h"
//https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/system_time.html

static const char *TAG = "ota_github";

#define CHECK_INTERVAL_MS  (60 * 60 * 1000)   // Actualicion de prueba
#define RETRY_INTERVAL_MS  (30 * 1000)        // Rollback


#define CHECK_HOUR     3                    // hora del día (0-23)
#define CHECK_MINUTE   0                    // minuto (0-59)
#define RETRY_MS       (5 * 60 * 1000)      // si falla algo, reintentar en x min
#define PERIOD_TICKS   (24UL * 60 * 60 * configTICK_RATE_HZ)   // 24 h 8 640 000  Unsigend Long 4 Byte

// Zona horaria 
//#define TIMEZONE       "CET-1CEST,M3.5.0,M10.5.0/3"

#define OTA_DAILY 0 // 0 prueba 1 periodico

static char s_base_url[160];

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    char *buf = (char *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA &&
        esp_http_client_get_status_code(evt->client) == 200) {
        size_t len = strlen(buf);
        if (len + evt->data_len < 31) {
            memcpy(buf + len, evt->data, evt->data_len);
            buf[len + evt->data_len] = '\0';
        }
    }
    return ESP_OK;
}

/* Descarga version.txt y lo deja en out (1.0.1) */
static bool get_remote_version(char *out)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/version.txt", s_base_url); 
    out[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = out,
        .timeout_ms = 15000,
        .buffer_size = 4096,      
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || out[0] == '\0') {
        ESP_LOGE(TAG, "No se pudo leer version.txt (err=%s, http=%d)",
                 esp_err_to_name(err), status);
        return false;
    }
    out[strcspn(out, "\r\n ")] = '\0';   // quitar salto de línea
    return true;
}

/* Compara "X.Y.Z" */
static bool is_newer(const char *remote, const char *local)
{
    int r[3] = {0}, l[3] = {0};
    sscanf(remote, "%d.%d.%d", &r[0], &r[1], &r[2]); // Lectura en formato
    sscanf(local,  "%d.%d.%d", &l[0], &l[1], &l[2]);
    return (r[0] * 1000000 + r[1] * 1000 + r[2]) >(l[0] * 1000000 + l[1] * 1000 + l[2]); 
    // l=1.0.0 r=1.0.1 101>001   
}

static void ota_task(void *pvParameter)
{
    const char *current = esp_app_get_description()->version;
    char remote[32];
    char fw_url[256];

    TickType_t last_wake = 0; // Time
    bool first=false;

    snprintf(fw_url, sizeof(fw_url), "%s/firmware.bin", s_base_url);

    TickType_t xLastWakeTime = 0;

    esp_http_client_config_t config = {
        .url = fw_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        };
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        };

    while(1) {
        ESP_LOGI(TAG, "Versión actual: %s", current);

        if (!get_remote_version(remote)) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
            continue;
        }
        ESP_LOGI(TAG, "Versión remota: %s", remote);

        if (is_newer(remote, current)) {
            ESP_LOGI(TAG, "Descargando actualización...");
            esp_err_t ret = esp_https_ota(&ota_config);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "OTA exitosa, reiniciando...");
                esp_restart();
            }
            ESP_LOGE(TAG, "OTA falló: %s", esp_err_to_name(ret)); // Si hay fallo hasta el siguiente ciclo
            vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
            continue;
        }
        ESP_LOGI(TAG, "Firmware al día");
        
#if OTA_DAILY
        if(!first){   
            first=time_sincro(&last_wake);
        }else{
            vTaskDelayUntil(&last_wake,PERIOD_TICKS);
        }
#else
        vTaskDelay(CHECK_INTERVAL_MS);
#endif

    }
}



void ota_github_start(const char *owner, const char *repo)
{
    // Si esta versión acaba de instalarse por OTA dar por buena
    // activar el rollback en menuconfig 
    esp_ota_mark_app_valid_cancel_rollback();

    snprintf(s_base_url, sizeof(s_base_url),
             "https://github.com/%s/%s/releases/latest/download", owner, repo);

    xTaskCreate(ota_task, "ota_github", 8192, NULL, 5, NULL);
}


static bool time_sincro(TickType_t *last_wake){
      // Primera vez: esperar a tener hora real y calcular cuánto falta
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "Esperando sincronización de hora (SNTP)...");
    }
    time_t now;
    struct tm t;
    time(&now); // Tiempo ACT
    localtime_r(&now, &t);  

    t.tm_hour = CHECK_HOUR; t.tm_min  = CHECK_MINUTE;t.tm_sec  = 0; // Hora Target
    t.tm_isdst = -1;
    time_t target = mktime(&t);

    if (target <= now) {   // ya paso hoy => mañana
        t.tm_mday += 1;
        t.tm_isdst = -1;
        target = mktime(&t);
    }
    uint32_t wait_s = (uint32_t)(target - now);
    ESP_LOGI(TAG, "Próxima comprobación a las %02d:%02d (en %lu min)",
             CHECK_HOUR, CHECK_MINUTE, (unsigned long)(wait_s / 60));   
    *last_wake = xTaskGetTickCount();
    vTaskDelayUntil(last_wake, wait_s * configTICK_RATE_HZ);
    return true;
}