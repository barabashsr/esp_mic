#include "wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"

#define MAX_SAVED_NETS   5
#define CONNECT_TIMEOUT_MS 8000
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define DEFAULT_SSID "Ya_Robot"
#define DEFAULT_PASS "de37945a0"

#define AP_SSID     "ESP32-Mic"
#define AP_PASS     "12345678"
#define AP_MAX_CONN 4

static const char *TAG = "wifi";

typedef struct {
    char ssid[33];
    char pass[65];
} saved_net_t;

static EventGroupHandle_t s_wifi_event_group;
static wifi_app_mode_t s_mode = WIFI_APP_MODE_OFFLINE;
static char s_ssid[33] = "";
static char s_ip[20] = "";
static int s_retry_num = 0;
static bool s_connecting = false;

// --- Event handler ---

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_connecting) esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connecting && s_retry_num < 3) {
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection (%d/3)", s_retry_num);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected! IP: %s", s_ip);
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- NVS helpers ---

static int load_saved_networks(saved_net_t *nets, int max)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return 0;

    uint8_t cnt = 0;
    nvs_get_u8(h, "wifi_cnt", &cnt);
    if (cnt > MAX_SAVED_NETS) cnt = MAX_SAVED_NETS;
    if (cnt > max) cnt = max;

    int loaded = 0;
    for (int i = 0; i < cnt; i++) {
        char sk[12], pk[12];
        snprintf(sk, sizeof(sk), "wifi_s%d", i);
        snprintf(pk, sizeof(pk), "wifi_p%d", i);

        size_t slen = sizeof(nets[loaded].ssid);
        size_t plen = sizeof(nets[loaded].pass);
        if (nvs_get_str(h, sk, nets[loaded].ssid, &slen) == ESP_OK &&
            nvs_get_str(h, pk, nets[loaded].pass, &plen) == ESP_OK) {
            loaded++;
        }
    }
    nvs_close(h);
    return loaded;
}

static void save_network_mru(const char *ssid, const char *pass)
{
    saved_net_t nets[MAX_SAVED_NETS];
    int cnt = load_saved_networks(nets, MAX_SAVED_NETS);

    // Remove duplicate if exists
    int dup = -1;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(nets[i].ssid, ssid) == 0) { dup = i; break; }
    }

    saved_net_t new_list[MAX_SAVED_NETS];
    int new_cnt = 0;

    // Slot 0: the new/promoted network
    strncpy(new_list[0].ssid, ssid, sizeof(new_list[0].ssid) - 1);
    new_list[0].ssid[sizeof(new_list[0].ssid) - 1] = '\0';
    strncpy(new_list[0].pass, pass, sizeof(new_list[0].pass) - 1);
    new_list[0].pass[sizeof(new_list[0].pass) - 1] = '\0';
    new_cnt = 1;

    // Copy the rest, skipping the duplicate
    for (int i = 0; i < cnt && new_cnt < MAX_SAVED_NETS; i++) {
        if (i == dup) continue;
        new_list[new_cnt++] = nets[i];
    }

    // Write to NVS
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, "wifi_cnt", (uint8_t)new_cnt);
    for (int i = 0; i < new_cnt; i++) {
        char sk[12], pk[12];
        snprintf(sk, sizeof(sk), "wifi_s%d", i);
        snprintf(pk, sizeof(pk), "wifi_p%d", i);
        nvs_set_str(h, sk, new_list[i].ssid);
        nvs_set_str(h, pk, new_list[i].pass);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d networks (MRU: %s)", new_cnt, ssid);
}

static void seed_default_if_empty(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        uint8_t cnt = 0;
        nvs_get_u8(h, "wifi_cnt", &cnt);
        nvs_close(h);
        if (cnt > 0) return; // already have networks
    }
    // Seed with default
    save_network_mru(DEFAULT_SSID, DEFAULT_PASS);
    ESP_LOGI(TAG, "Seeded default network: %s", DEFAULT_SSID);
}

// --- STA connect attempt ---

static bool try_sta_connect(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Trying STA: %s", ssid);

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    s_connecting = true;
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

    s_connecting = false;

    if (bits & WIFI_CONNECTED_BIT) {
        strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = '\0';
        return true;
    }

    // Failed — stop WiFi for next attempt
    esp_wifi_stop();
    ESP_LOGW(TAG, "Failed to connect to %s", ssid);
    return false;
}

// --- AP mode ---

static void start_ap(void)
{
    ESP_LOGI(TAG, "Starting AP: %s", AP_SSID);

    esp_wifi_stop();

    wifi_config_t cfg = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .ssid_len = strlen(AP_SSID),
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    esp_wifi_start();

    strncpy(s_ssid, AP_SSID, sizeof(s_ssid) - 1);
    strncpy(s_ip, "192.168.4.1", sizeof(s_ip) - 1);
    s_mode = WIFI_APP_MODE_AP;
    ESP_LOGI(TAG, "AP started: %s / %s  IP: %s", AP_SSID, AP_PASS, s_ip);
}

// --- Public API ---

void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    // Seed default credentials if NVS is empty
    seed_default_if_empty();

    // Step 1: Start in STA mode to do a scan
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    ESP_LOGI(TAG, "Scanning for networks...");
    esp_err_t scan_ret = esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_list = NULL;
    if (scan_ret == ESP_OK) {
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 20) ap_count = 20;
        ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);
        }
        ESP_LOGI(TAG, "Found %d networks", ap_count);
    } else {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(scan_ret));
    }

    esp_wifi_stop();

    // Step 2: Load saved credentials
    saved_net_t saved[MAX_SAVED_NETS];
    int saved_cnt = load_saved_networks(saved, MAX_SAVED_NETS);
    ESP_LOGI(TAG, "Loaded %d saved networks", saved_cnt);

    // Step 3: Try each saved network found in scan results (MRU order)
    for (int s = 0; s < saved_cnt; s++) {
        bool found_in_scan = false;
        if (ap_list) {
            for (int a = 0; a < ap_count; a++) {
                if (strcmp(saved[s].ssid, (const char *)ap_list[a].ssid) == 0) {
                    found_in_scan = true;
                    break;
                }
            }
        }
        if (!found_in_scan) {
            ESP_LOGI(TAG, "Saved net '%s' not in scan, skipping", saved[s].ssid);
            continue;
        }
        if (try_sta_connect(saved[s].ssid, saved[s].pass)) {
            // Success! Promote to MRU
            save_network_mru(saved[s].ssid, saved[s].pass);
            s_mode = WIFI_APP_MODE_STA;
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            ESP_LOGI(TAG, "WiFi modem sleep enabled");
            free(ap_list);
            return;
        }
    }

    free(ap_list);

    // Step 4: All failed — start AP
    start_ap();
}

wifi_app_mode_t wifi_get_mode(void)
{
    return s_mode;
}

const char *wifi_get_ssid(void)
{
    return s_ssid;
}

const char *wifi_get_ip(void)
{
    return s_ip;
}

uint16_t wifi_scan(wifi_ap_record_t *results, uint16_t max)
{
    // Must be in AP or STA mode with WiFi started
    wifi_scan_config_t cfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return 0;

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > max) count = max;
    esp_wifi_scan_get_ap_records(&count, results);
    return count;
}

void wifi_save_and_connect(const char *ssid, const char *pass)
{
    save_network_mru(ssid, pass);
    ESP_LOGI(TAG, "Credentials saved. Rebooting in 500ms...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

int wifi_get_saved_ssids(char ssids[][33], int max)
{
    saved_net_t nets[MAX_SAVED_NETS];
    int cnt = load_saved_networks(nets, max < MAX_SAVED_NETS ? max : MAX_SAVED_NETS);
    for (int i = 0; i < cnt; i++) {
        strncpy(ssids[i], nets[i].ssid, 33);
        ssids[i][32] = '\0';
    }
    return cnt;
}
