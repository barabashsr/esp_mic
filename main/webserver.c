#include "webserver.h"
#include "sdcard.h"
#include "audio.h"

#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"

static const char *TAG = "webserver";

#define MAX_WS_CLIENTS 4

// Embedded HTML
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;
static int s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;
static webserver_cmd_cb_t s_cmd_cb = NULL;

// --- WebSocket client tracking ---

static void ws_clients_init(void)
{
    s_ws_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_ws_fds[i] = -1;
    }
}

static void ws_client_add(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == -1) {
            s_ws_fds[i] = fd;
            ESP_LOGI(TAG, "WS client added: fd=%d slot=%d", fd, i);
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static void ws_client_remove(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = -1;
            ESP_LOGI(TAG, "WS client removed: fd=%d slot=%d", fd, i);
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static void ws_close_callback(httpd_handle_t hd, int fd)
{
    ws_client_remove(fd);
    close(fd);
}

// --- HTTP Handlers ---

static esp_err_t index_handler(httpd_req_t *req)
{
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, len);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // New WebSocket connection
        ws_client_add(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    // Receive WebSocket frame
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len == 0) return ESP_OK;

    uint8_t *buf = malloc(ws_pkt.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    buf[ws_pkt.len] = 0;

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && s_cmd_cb) {
        // Parse JSON command
        cJSON *json = cJSON_Parse((char *)buf);
        if (json) {
            cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
            if (cmd && cJSON_IsString(cmd)) {
                s_cmd_cb(cmd->valuestring);
            }
            cJSON_Delete(json);
        }
    }

    free(buf);
    return ESP_OK;
}

static esp_err_t api_files_handler(httpd_req_t *req)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open SD card");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_CreateArray();
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        // Only list .wav files
        char *ext = strrchr(ent->d_name, '.');
        if (!ext || strcasecmp(ext, ".wav") != 0) continue;

        char path[280];
        snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", ent->d_name);
        cJSON_AddNumberToObject(obj, "size", st.st_size);

        // Format modification time
        struct tm ti;
        localtime_r(&st.st_mtime, &ti);
        char timebuf[80];
        snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        cJSON_AddStringToObject(obj, "modified", timebuf);

        cJSON_AddItemToArray(arr, obj);
    }
    closedir(dir);

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

static esp_err_t api_file_download_handler(httpd_req_t *req)
{
    // URI: /api/files/<filename>
    const char *filename = req->uri + strlen("/api/files/");
    if (!filename || strlen(filename) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
        return ESP_FAIL;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "audio/wav");

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_file_delete_handler(httpd_req_t *req)
{
    const char *filename = req->uri + strlen("/api/files/");
    if (!filename || strlen(filename) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
        return ESP_FAIL;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, filename);

    if (unlink(path) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t api_auto_handler(httpd_req_t *req)
{
    extern void main_set_auto_mode(bool enabled);
    extern void main_set_auto_threshold(uint16_t thr);
    extern bool main_auto_mode(void);
    extern uint16_t main_auto_threshold(void);

    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (enabled && cJSON_IsBool(enabled)) {
        main_set_auto_mode(cJSON_IsTrue(enabled));
    }

    cJSON *threshold = cJSON_GetObjectItem(json, "threshold");
    if (threshold && cJSON_IsNumber(threshold)) {
        main_set_auto_threshold((uint16_t)threshold->valueint);
    }

    cJSON_Delete(json);

    // Return current state
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "auto_mode", main_auto_mode());
    cJSON_AddNumberToObject(resp, "auto_threshold", main_auto_threshold());
    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

static esp_err_t api_codec_handler(httpd_req_t *req)
{
    extern bool main_use_ulaw(void);
    extern void main_set_use_ulaw(bool v);

    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ulaw = cJSON_GetObjectItem(json, "ulaw");
    if (ulaw && cJSON_IsBool(ulaw)) {
        main_set_use_ulaw(cJSON_IsTrue(ulaw));
    }
    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ulaw", main_use_ulaw());
    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

static esp_err_t api_filter_handler(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    uint16_t hp = audio_get_hp_freq();
    uint16_t lp = audio_get_lp_freq();

    cJSON *jhp = cJSON_GetObjectItem(json, "hp");
    if (jhp && cJSON_IsNumber(jhp)) {
        int v = jhp->valueint;
        if (v == 0 || (v >= 50 && v <= 2000)) hp = (uint16_t)v;
    }
    cJSON *jlp = cJSON_GetObjectItem(json, "lp");
    if (jlp && cJSON_IsNumber(jlp)) {
        int v = jlp->valueint;
        if (v == 0 || (v >= 2000 && v <= 9500)) lp = (uint16_t)v;
    }
    cJSON_Delete(json);

    audio_set_filter(hp, lp);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "filter_hp", audio_get_hp_freq());
    cJSON_AddNumberToObject(resp, "filter_lp", audio_get_lp_freq());
    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

static esp_err_t api_rec_handler(httpd_req_t *req)
{
    if (s_cmd_cb) {
        // POST /api/rec/start or /api/rec/stop
        const char *action = req->uri + strlen("/api/rec/");
        if (strcmp(action, "start") == 0) {
            s_cmd_cb("start_rec");
        } else if (strcmp(action, "stop") == 0) {
            s_cmd_cb("stop_rec");
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();

    // SD free space
    uint64_t free_bytes = sdcard_free_bytes();
    cJSON_AddNumberToObject(obj, "sd_free_mb", (double)free_bytes / (1024 * 1024));

    // WiFi RSSI
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(obj, "rssi", ap_info.rssi);
    }

    // Recording state -- provided by main.c via getters
    extern bool main_is_recording(void);
    extern const char *main_rec_filename(void);
    extern const char *main_rec_start_time(void);
    extern const char *main_rec_source_str(void);
    extern uint16_t main_current_rms(void);
    extern bool main_auto_mode(void);
    extern uint16_t main_auto_threshold(void);
    extern bool main_use_ulaw(void);
    extern float main_current_zcr(void);

    bool rec = main_is_recording();
    cJSON_AddBoolToObject(obj, "recording", rec);
    if (rec) {
        cJSON_AddStringToObject(obj, "filename", main_rec_filename());
        const char *start_time = main_rec_start_time();
        if (start_time[0]) {
            cJSON_AddStringToObject(obj, "rec_started_at", start_time);
        }
        cJSON_AddStringToObject(obj, "rec_source", main_rec_source_str());
    }

    // ADC overflow count
    extern uint32_t audio_get_overflow_count(void);
    cJSON_AddNumberToObject(obj, "adc_overflows", audio_get_overflow_count());

    // Auto-record state
    cJSON_AddBoolToObject(obj, "auto_mode", main_auto_mode());
    cJSON_AddNumberToObject(obj, "auto_threshold", main_auto_threshold());
    cJSON_AddNumberToObject(obj, "current_rms", main_current_rms());
    cJSON_AddBoolToObject(obj, "ulaw", main_use_ulaw());
    cJSON_AddNumberToObject(obj, "current_zcr", (double)main_current_zcr());

    // Filter state
    cJSON_AddNumberToObject(obj, "filter_hp", audio_get_hp_freq());
    cJSON_AddNumberToObject(obj, "filter_lp", audio_get_lp_freq());

    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ESP_OK;
}

// --- Public API ---

esp_err_t webserver_start(webserver_cmd_cb_t cmd_cb)
{
    s_cmd_cb = cmd_cb;
    ws_clients_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.close_fn = ws_close_callback;
    config.max_uri_handlers = 16;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register handlers (order matters for wildcard matching)
    httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &uri_ws);

    httpd_uri_t uri_auto = {
        .uri = "/api/auto",
        .method = HTTP_POST,
        .handler = api_auto_handler,
    };
    httpd_register_uri_handler(s_server, &uri_auto);

    httpd_uri_t uri_codec = {
        .uri = "/api/codec",
        .method = HTTP_POST,
        .handler = api_codec_handler,
    };
    httpd_register_uri_handler(s_server, &uri_codec);

    httpd_uri_t uri_filter = {
        .uri = "/api/filter",
        .method = HTTP_POST,
        .handler = api_filter_handler,
    };
    httpd_register_uri_handler(s_server, &uri_filter);

    httpd_uri_t uri_rec = {
        .uri = "/api/rec/*",
        .method = HTTP_POST,
        .handler = api_rec_handler,
    };
    httpd_register_uri_handler(s_server, &uri_rec);

    httpd_uri_t uri_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_files = {
        .uri = "/api/files",
        .method = HTTP_GET,
        .handler = api_files_handler,
    };
    httpd_register_uri_handler(s_server, &uri_files);

    httpd_uri_t uri_file_download = {
        .uri = "/api/files/*",
        .method = HTTP_GET,
        .handler = api_file_download_handler,
    };
    httpd_register_uri_handler(s_server, &uri_file_download);

    httpd_uri_t uri_file_delete = {
        .uri = "/api/files/*",
        .method = HTTP_DELETE,
        .handler = api_file_delete_handler,
    };
    httpd_register_uri_handler(s_server, &uri_file_delete);

    httpd_uri_t uri_index = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(s_server, &uri_index);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

void webserver_broadcast_audio(const int16_t *samples, size_t num_samples)
{
    if (!s_server || num_samples == 0) return;

    httpd_ws_frame_t ws_pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)samples,
        .len = num_samples * sizeof(int16_t),
    };

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] != -1) {
            esp_err_t ret = httpd_ws_send_frame_async(s_server, s_ws_fds[i], &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send to fd=%d, removing", s_ws_fds[i]);
                s_ws_fds[i] = -1;
            }
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

bool webserver_has_clients(void)
{
    bool has = false;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] != -1) {
            has = true;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
    return has;
}
