#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "wifi_provisioning.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"

/* NOTE: on some ESP-IDF versions MACSTR/MAC2STR live in "esp_mac.h"
 * instead of esp_wifi.h. If the build complains, add:
 *   #include "esp_mac.h"
 */

#define WIFI_AP_SSID     "esp_tutorial"
#define WIFI_AP_PASS     "test_esp"
#define WIFI_AP_CHANNEL  1
#define WIFI_AP_MAX_CONN 4

static const char *TAG = "wifi_provisioning";

/* Embedded webpage/index.html (see EMBED_TXTFILES in CMakeLists.txt) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ---------------------------------------------------------------- */
/* Connection status shared between the wifi event handler and the  */
/* /status HTTP endpoint, so the web page can poll for the result.  */
/* ---------------------------------------------------------------- */

typedef enum {
    PROV_STATE_IDLE,
    PROV_STATE_CONNECTING,
    PROV_STATE_CONNECTED,
    PROV_STATE_FAILED,
} prov_state_t;

static volatile prov_state_t s_state = PROV_STATE_IDLE; // AUTOMATA de estado
static char s_last_ip[16] = {0};
static char s_current_ssid[33] = {0};
static int  s_last_reason = 0;

static const char *reason_to_str(int reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:        return "Red no encontrada";
        case WIFI_REASON_AUTH_FAIL:          return "Contraseña incorrecta";
        case WIFI_REASON_AUTH_EXPIRE:        return "Fallo de autenticación (revisa la contraseña)";
        case WIFI_REASON_ASSOC_FAIL:         return "Fallo de asociación con el punto de acceso";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:  return "Tiempo de espera agotado (contraseña probablemente incorrecta)";
        case WIFI_REASON_AP_TSF_RESET:       return "Reinicio del AP";
        case WIFI_REASON_BEACON_TIMEOUT:     return "Señal perdida (fuera de rango)";
        default:                             return "Fallo desconocido";
    }
}

/* ---------------------------------------------------------------- */
/* WiFi                                                              */
/* ---------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *) event_data;
                ESP_LOGI(TAG, "Client joined AP, AID=%d", ev->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *) event_data;
                ESP_LOGI(TAG, "Client left AP, AID=%d", ev->aid);
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *) event_data;
                ESP_LOGW(TAG, "STA disconnected, reason=%d (%s)", ev->reason, reason_to_str(ev->reason));
                s_last_reason = ev->reason;
                /* Only mark as "failed" if we were actively trying to connect;
                 * otherwise this fires at boot too and would be misleading. */
                if (s_state == PROV_STATE_CONNECTING || s_state == PROV_STATE_CONNECTED) {
                    s_state = PROV_STATE_FAILED;
                }
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(s_last_ip, sizeof(s_last_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_state = PROV_STATE_CONNECTED;

        wifi_low_power_mode(); //Ahorro
    }
}

void wifi_provisioning_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(  
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));   // IMPORTANTE HANDLER PART
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

        

    wifi_config_t ap_config = { //SSID PSSWD router AP
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .password = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = true },
        },
    };
    if (strlen(WIFI_AP_PASS) < 8) {
        ESP_LOGW(TAG, "AP password shorter than 8 chars, falling back to OPEN network");
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        memset(ap_config.ap.password, 0, sizeof(ap_config.ap.password));
    }

    /* AP+STA: keep the portal reachable while also being able to scan
     * and join an external network. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH)); // persist STA creds across resets 
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started. SSID:%s password:%s channel:%d",
             WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL);

    /* Auto-reconnect STA*/
    wifi_config_t saved_sta_config = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved_sta_config) == ESP_OK &&
        strlen((char *) saved_sta_config.sta.ssid) > 0) {
        ESP_LOGI(TAG, "Credenciales guardadas encontradas para '%s', reconectando...",
                 saved_sta_config.sta.ssid);
        strlcpy(s_current_ssid, (char *) saved_sta_config.sta.ssid, sizeof(s_current_ssid));
        s_state = PROV_STATE_CONNECTING;
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "No hay credenciales STA guardadas todavia");
    }
}

/* ---------------------------------------------------------------- */
/* HTTP handlers                                                     */
/* ---------------------------------------------------------------- */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *) index_html_start, len);
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    /* Blocking scan. Keep it short - it briefly pauses AP beaconing. */
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"scan_failed\"}");
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"no_memory\"}");
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    /* Small payload -> build JSON by hand, no cJSON dependency needed. */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < ap_count; i++) {
        char safe_ssid[32 * 2 + 1] = {0};
        int j = 0;
        for (int k = 0; k < 32 && ap_records[i].ssid[k] != '\0' && j < (int) sizeof(safe_ssid) - 2; k++) {
            char c = ap_records[i].ssid[k];
            if (c == '"' || c == '\\') {
                safe_ssid[j++] = '\\';
            }
            safe_ssid[j++] = c;
        }
        safe_ssid[j] = '\0';

        char entry[160];
        snprintf(entry, sizeof(entry),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"authmode\":%d}",
                 (i == 0) ? "" : ",", safe_ssid, ap_records[i].rssi, ap_records[i].authmode);
        httpd_resp_sendstr_chunk(req, entry);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL); /* end chunked response */

    free(ap_records);
    return ESP_OK;
}

/* Minimal x-www-form-urlencoded decoder for the values pulled out
 * with httpd_query_key_value(). */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di < dst_size - 1; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = { src[si + 1], src[si + 2], 0 };
            dst[di++] = (char) strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char buf[256];
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= (int) sizeof(buf)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid body\"}");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    char ssid_raw[64] = {0};
    char pass_raw[64] = {0};
    char ssid[33] = {0};
    char pass[65] = {0};

    if (httpd_query_key_value(buf, "ssid", ssid_raw, sizeof(ssid_raw)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"missing ssid\"}");
        return ESP_FAIL;
    }
    /* password is optional (open networks) */
    httpd_query_key_value(buf, "password", pass_raw, sizeof(pass_raw));

    url_decode(ssid, ssid_raw, sizeof(ssid));
    url_decode(pass, pass_raw, sizeof(pass));

    wifi_config_t sta_config = {0};
    strlcpy((char *) sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *) sta_config.sta.password, pass, sizeof(sta_config.sta.password));

    s_state = PROV_STATE_CONNECTING;
    s_last_ip[0] = '\0';
    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    esp_err_t err = esp_wifi_connect();

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"connecting\"}");
    } else {
        s_state = PROV_STATE_FAILED;
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"connect_failed\"}");
    }
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char resp[160];
    httpd_resp_set_type(req, "application/json");

    switch (s_state) {
        case PROV_STATE_CONNECTED:
            snprintf(resp, sizeof(resp), "{\"status\":\"connected\",\"ip\":\"%s\"}", s_last_ip);
            break;
        case PROV_STATE_CONNECTING:
            snprintf(resp, sizeof(resp), "{\"status\":\"connecting\"}");
            break;
        case PROV_STATE_FAILED:
            snprintf(resp, sizeof(resp), "{\"status\":\"failed\",\"reason\":\"%s\"}", reason_to_str(s_last_reason));
            break;
        default:
            snprintf(resp, sizeof(resp), "{\"status\":\"idle\"}");
            break;
    }
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL
};
static const httpd_uri_t scan_uri = {
    .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL
};
static const httpd_uri_t connect_uri = {
    .uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler, .user_ctx = NULL
};
static const httpd_uri_t status_uri = {
    .uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL
};

httpd_handle_t wifi_provisioning_start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting HTTP server");
        return NULL;
    }

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &scan_uri);
    httpd_register_uri_handler(server, &connect_uri);
    httpd_register_uri_handler(server, &status_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return server;
}

void wifi_provisioning_stop_webserver(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}

void wifi_provisioning_get_status(wifi_prov_info_t *out_info)
{
    if (!out_info) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    strlcpy(out_info->ssid, s_current_ssid, sizeof(out_info->ssid));

    switch (s_state) {
        case PROV_STATE_CONNECTED:
            out_info->status = WIFI_PROV_STATUS_CONNECTED;
            strlcpy(out_info->ip_str, s_last_ip, sizeof(out_info->ip_str));
            break;
        case PROV_STATE_CONNECTING:
            out_info->status = WIFI_PROV_STATUS_CONNECTING;
            break;
        case PROV_STATE_FAILED:
            out_info->status = WIFI_PROV_STATUS_FAILED;
            strlcpy(out_info->reason, reason_to_str(s_last_reason), sizeof(out_info->reason));
            break;
        default:
            out_info->status = WIFI_PROV_STATUS_IDLE;
            break;
    }

    
}


void wifi_low_power_mode(void)
{
    ESP_LOGI(TAG, "Setting WiFi to low power mode");
    esp_wifi_set_mode(WIFI_MODE_STA); // Set WiFi to station mode (AP+STA -> STA)
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // modem power saving

}
