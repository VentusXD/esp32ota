#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in AP+STA mode and start the SoftAP.
 *
 * Must be called after nvs_flash_init(). The AP stays up so the
 * provisioning page remains reachable while the radio also scans
 * and connects to an external network as a station.
 */
void wifi_provisioning_init(void);

/**
 * @brief Start the HTTP server exposing:
 *   GET  /         -> provisioning web page
 *   GET  /scan     -> JSON list of nearby networks
 *   POST /connect  -> connect to a chosen network (ssid, password)
 *
 * @return handle to the httpd server, or NULL on failure
 */
httpd_handle_t wifi_provisioning_start_webserver(void);

/**
 * @brief Stop the HTTP server started with wifi_provisioning_start_webserver().
 */
void wifi_provisioning_stop_webserver(httpd_handle_t server);


/**
 * @brief Get the current provisioning status.
 *
 * @param out_info Struct para estados de conexión y detalles de la red.
 */
typedef enum {
    WIFI_PROV_STATUS_IDLE,
    WIFI_PROV_STATUS_CONNECTING,
    WIFI_PROV_STATUS_CONNECTED,
    WIFI_PROV_STATUS_FAILED,
} wifi_prov_status_t;

typedef struct {
    wifi_prov_status_t status;
    char ssid[33];
    char ip_str[16];
    char reason[48];
} wifi_prov_info_t;

void wifi_provisioning_get_status(wifi_prov_info_t *out_info);
void wifi_low_power_mode(void);


#ifdef __cplusplus
}
#endif
