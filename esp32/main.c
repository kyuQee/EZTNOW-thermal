#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

// Override default I2C pins for the i2cdev library (before including its header)
#define I2C_MASTER_SCL_IO          22
#define I2C_MASTER_SDA_IO          21
#define I2C_MASTER_NUM             0

#include "sht4x.h"          // esp-idf-lib SHT4x driver
#include "i2cdev.h"         // I2C bus abstraction

static const char *TAG = "ESP32_TELE";

#define WIFI_SSID     "MAWMAW"
#define WIFI_PASS     "helloworld"
#define SERVER_PORT   9000
#define MAX_SCAN_AP   10

static esp_netif_t *sta_netif = NULL;
static char dynamic_server_ip[16] = {0};
static volatile bool wifi_connected = false;
static volatile bool wifi_started = false;
static volatile bool need_reconnect = false;

// Device descriptor for the SHT40
static sht4x_t sht40_dev;

// Generate device ID from MAC
static void get_device_id(char *buffer, size_t len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buffer, len, "ESP32_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Scans for the target SSID and returns the BSSID with the best RSSI
static bool scan_and_select_best_ap(uint8_t *best_bssid) {
    ESP_LOGI(TAG, "Scanning for SSID: %s...", WIFI_SSID);
    
    wifi_scan_config_t scan_config = {
        .ssid = (uint8_t *)WIFI_SSID,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE
    };

    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed");
        return false;
    }

    uint16_t ap_count = MAX_SCAN_AP;
    wifi_ap_record_t ap_info[MAX_SCAN_AP];
    memset(ap_info, 0, sizeof(ap_info));

    if (esp_wifi_scan_get_ap_records(&ap_count, ap_info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records");
        return false;
    }

    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found matching SSID: %s", WIFI_SSID);
        return false;
    }

    int best_idx = -1;
    int32_t max_rssi = -127;

    for (int i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "Found AP: [" MACSTR "] RSSI: %d", MAC2STR(ap_info[i].bssid), (int)ap_info[i].rssi);
        if (ap_info[i].rssi > max_rssi) {
            max_rssi = ap_info[i].rssi;
            best_idx = i;
        }
    }

    if (best_idx != -1) {
        memcpy(best_bssid, ap_info[best_idx].bssid, 6);
        ESP_LOGI(TAG, "Selected best AP: [" MACSTR "] with RSSI: %d", MAC2STR(best_bssid), (int)max_rssi);
        return true;
    }

    return false;
}

// Wi‑Fi event handler – only sets flags, does NOT scan
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_started = true;
        need_reconnect = true;  // trigger initial connection attempt
        ESP_LOGI(TAG, "Wi-Fi started, requesting connection");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        need_reconnect = true;
        ESP_LOGI(TAG, "Wi-Fi disconnected, will retry");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Dynamically fetch gateway IP (the server/host)
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.gw, dynamic_server_ip, sizeof(dynamic_server_ip));
            ESP_LOGI(TAG, "Dynamic Server (Gateway) IP: %s", dynamic_server_ip);
            wifi_connected = true;
            need_reconnect = false; // successful connection
        }
    }
}

// Task that manages Wi‑Fi connection: scans, configures, and reconnects as needed
static void wifi_manager_task(void *pvParameters) {
    uint8_t best_bssid[6];
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .bssid_set = false,
        },
    };

    while (1) {
        // Wait until Wi-Fi stack is started
        if (!wifi_started) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // If we are disconnected or need to reconnect, perform a scan and connect
        if (need_reconnect && !wifi_connected) {
            ESP_LOGI(TAG, "Starting scan to find best AP...");
            if (scan_and_select_best_ap(best_bssid)) {
                memcpy(wifi_config.sta.bssid, best_bssid, 6);
                wifi_config.sta.bssid_set = true;
            } else {
                // Fallback: connect without BSSID (any AP)
                wifi_config.sta.bssid_set = false;
                ESP_LOGW(TAG, "No suitable AP found, connecting without BSSID");
            }

            // Apply configuration and connect
            esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set Wi-Fi config: %s", esp_err_to_name(err));
            } else {
                err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to connect: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "Connection attempt started");
                }
            }
            need_reconnect = false; // avoid repeated attempts until next disconnect
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // check periodically
    }
}

// Initialize Wi‑Fi in station mode
static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers (no scanning inside!)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ------------------- SHT40 using esp-idf-lib -------------------
static esp_err_t sht40_init(void) {
    // Initialize the I2C bus via i2cdev (uses pins defined above)
    esp_err_t err = i2cdev_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2cdev_init failed: %s", esp_err_to_name(err));
        return err;
    }
    // Initialize the SHT4x device descriptor (only port and GPIOs)
    err = sht4x_init_desc(&sht40_dev, I2C_MASTER_NUM, I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sht4x_init_desc failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t sht40_read(float *temp, float *hum) {
    // Use the library's measure function (high precision)
    esp_err_t err = sht4x_measure(&sht40_dev, temp, hum);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHT4x measure failed: %s", esp_err_to_name(err));
    }
    return err;
}

// ------------------- TCP Client Task (uses SHT40 library) -------------------
void tcp_client_task(void *pvParameters) {
    char device_id[18];
    get_device_id(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    int sock = -1;
    struct sockaddr_in server_addr;

    while (1) {
        if (!wifi_connected || strlen(dynamic_server_ip) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, dynamic_server_ip, &server_addr.sin_addr);

        ESP_LOGI(TAG, "Connecting to server %s:%d ...", dynamic_server_ip, SERVER_PORT);
        int err = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Connect failed, errno=%d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(TAG, "Connected to server");

        while (wifi_connected) {
            // Read real data from SHT40
            float temperature = 0.0f, humidity = 0.0f;
            esp_err_t ret = sht40_read(&temperature, &humidity);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "SHT40 read error, using placeholder values");
                temperature = -99.0f;
                humidity = -99.0f;
            }

            char json_msg[256];
            snprintf(json_msg, sizeof(json_msg),
                     "{\"device_id\":\"%s\",\"temperature\":%.2f,\"humidity\":%.2f,\"timestamp\":%lu}\n",
                     device_id, temperature, humidity, (unsigned long)time(NULL));

            int len = strlen(json_msg);
            int sent = send(sock, json_msg, len, 0);
            if (sent < 0) {
                ESP_LOGE(TAG, "Send failed, disconnecting");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000)); // send every second
        }

        close(sock);
        sock = -1;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize SHT40 using the esp-idf-lib component
    ESP_ERROR_CHECK(sht40_init());

    wifi_init_sta();

    // Create the Wi‑Fi manager task
    xTaskCreate(wifi_manager_task, "wifi_manager", 4096, NULL, 5, NULL);

    srand((unsigned)time(NULL));

    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
}
