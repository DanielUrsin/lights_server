#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_http_server.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define WIFI_SSID "lights"
#define WIFI_PASS "123456789"
#define WIFI_CHANNEL 1

#define ESPNOW_LIGHT_STATE_REQUEST "GET_LIGHT_STATE"
#define ESPNOW_LIGHT_STATE_RESPONSE_PREFIX "LIGHT_STATE="

#define LIGHT_STATE_NVS_NAMESPACE "lights"
#define LIGHT_STATE_NVS_KEY "state"

#define BUTTON_GPIO GPIO_NUM_10
#define LED_GPIO    GPIO_NUM_8

static const char *TAG = "lights_server";

// Shared light state.
// Marked volatile because it is modified from both normal task context and ISR context.
static volatile uint32_t light_state = 0;

// Timestamp of the last accepted button press, used for debounce.
static volatile int64_t last_press_time_us = 0;

// Queue used to pass button events from ISR → task
static QueueHandle_t button_queue;

typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
} espnow_light_state_request_t;

// Queue used to pass ESP-NOW requests from the Wi-Fi callback to a task.
static QueueHandle_t espnow_request_queue;

static esp_err_t save_light_state(uint32_t state)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LIGHT_STATE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, LIGHT_STATE_NVS_KEY, state ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static void load_light_state(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LIGHT_STATE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open light state storage: %s", esp_err_to_name(err));
        light_state = 0;
        return;
    }

    uint8_t stored_state = 0;
    err = nvs_get_u8(handle, LIGHT_STATE_NVS_KEY, &stored_state);
    nvs_close(handle);

    if (err == ESP_OK) {
        light_state = stored_state ? 1 : 0;
        ESP_LOGI(TAG, "Loaded persisted light_state=%lu", (unsigned long)light_state);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        light_state = 0;
        ESP_LOGI(TAG, "No persisted light_state found, defaulting to 0");
    } else {
        light_state = 0;
        ESP_LOGW(TAG, "Failed to load light state: %s", esp_err_to_name(err));
    }
}

/*
 * GPIO interrupt handler.
 *
 * Runs in interrupt context → must be extremely fast and safe.
 * DO NOT:
 *   - log
 *   - allocate memory
 *   - call complex APIs
 *
 * Only signal the task that "something happened".
 */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t gpio_num = BUTTON_GPIO;

    // Send event to queue (ISR-safe version)
    xQueueSendFromISR(button_queue, &gpio_num, NULL);
}

/*
 * Button processing task.
 *
 * This is where we:
 *  - debounce the button
 *  - detect valid presses
 *  - toggle the state
 */
static void button_task(void *arg)
{
    uint32_t gpio_num;

    // Last accepted press time (for debounce)
    TickType_t last_press_tick = 0;

    while (true) {

        // Wait forever until ISR sends an event
        if (xQueueReceive(button_queue, &gpio_num, portMAX_DELAY)) {

            TickType_t now = xTaskGetTickCount();

            /*
             * STEP 1: Ignore rapid repeated triggers
             *
             * Mechanical buttons bounce → multiple interrupts
             * This filters out presses that happen too close together.
             */
            if ((now - last_press_tick) < pdMS_TO_TICKS(150)) {
                continue;
            }

            /*
             * STEP 2: Confirm the button is actually pressed
             *
             * Wait a short time to let bouncing settle,
             * then re-check the GPIO level.
             */
            vTaskDelay(pdMS_TO_TICKS(30));

            if (gpio_get_level(BUTTON_GPIO) == 0) {  // active LOW

                /*
                 * STEP 3: Valid press detected
                 * Toggle the light state
                 */
                light_state = light_state ? 0 : 1;
                esp_err_t save_err = save_light_state(light_state);
                if (save_err != ESP_OK) {
                    ESP_LOGW(TAG,
                             "Failed to persist button light_state=%lu: %s",
                             (unsigned long)light_state,
                             esp_err_to_name(save_err));
                }

                last_press_tick = now;

                ESP_LOGI(TAG,
                         "Button press OK → light_state=%lu",
                         (unsigned long)light_state);

                /*
                 * STEP 4: Wait for button release
                 *
                 * Prevents:
                 *   - long press generating multiple toggles
                 */
                while (gpio_get_level(BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

                /*
                 * STEP 5: Release debounce
                 *
                 * Prevents bounce on release from retriggering
                 */
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
}

/*
 * HTTP GET /lights
 *
 * Returns current light state as plain text:
 * "0" or "1"
 */
static esp_err_t get_light_state(httpd_req_t *req)
{
    char response[8];

    snprintf(response, sizeof(response), "%lu", (unsigned long)light_state);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, response);

    
    ESP_LOGI(TAG, "Responded with state=%lu", (unsigned long)light_state);

    return ESP_OK;
}

/*
 * HTTP POST /lights?state=0
 * HTTP POST /lights?state=1
 *
 * Updates the light state from the query parameter.
 */
static esp_err_t set_light_state(httpd_req_t *req)
{
    char query[64];
    char param[16];

    // Read the URL query string.
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query string");
        return ESP_FAIL;
    }

    // Extract the "state" parameter.
    if (httpd_query_key_value(query, "state", param, sizeof(param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing state");
        return ESP_FAIL;
    }

    // Normalize all non-zero values to 1.
    int value = atoi(param);
    light_state = value > 0 ? 1 : 0;

    esp_err_t save_err = save_light_state(light_state);
    if (save_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to persist light state");
        return save_err;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");

    return ESP_OK;
}

/*
 * HTTP GET /mac
 *
 * Returns the Wi-Fi SoftAP MAC address as plain text:
 * "aa:bb:cc:dd:ee:ff"
 */
static esp_err_t get_mac_address(httpd_req_t *req)
{
    uint8_t mac[6];
    char response[18];

    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read MAC address");
        return err;
    }

    snprintf(response, sizeof(response), MACSTR, MAC2STR(mac));

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, response);

    ESP_LOGI(TAG, "Responded with mac=%s", response);

    return ESP_OK;
}

/*
 * Start the built-in ESP-IDF HTTP server and register endpoints.
 */
static httpd_handle_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    // Start HTTP server.
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    // Register GET /lights.
    httpd_uri_t get_light_uri = {
        .uri = "/lights",
        .method = HTTP_GET,
        .handler = get_light_state,
        .user_ctx = NULL,
    };

    // Register POST /lights?state=...
    httpd_uri_t set_light_uri = {
        .uri = "/lights",
        .method = HTTP_POST,
        .handler = set_light_state,
        .user_ctx = NULL,
    };

    // Register GET /mac.
    httpd_uri_t get_mac_uri = {
        .uri = "/mac",
        .method = HTTP_GET,
        .handler = get_mac_address,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_light_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set_light_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_mac_uri));

    ESP_LOGI(TAG, "HTTP server started");

    return server;
}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data,
                           int data_len)
{
    if (info == NULL || info->src_addr == NULL || data == NULL ||
        espnow_request_queue == NULL) {
        return;
    }

    const size_t request_len = strlen(ESPNOW_LIGHT_STATE_REQUEST);
    if ((data_len != request_len && data_len != request_len + 1) ||
        memcmp(data, ESPNOW_LIGHT_STATE_REQUEST, request_len) != 0) {
        return;
    }

    espnow_light_state_request_t request = {};
    memcpy(request.mac, info->src_addr, sizeof(request.mac));

    xQueueSend(espnow_request_queue, &request, 0);
}

static void espnow_light_state_task(void *arg)
{
    espnow_light_state_request_t request;

    while (true) {
        if (!xQueueReceive(espnow_request_queue, &request, portMAX_DELAY)) {
            continue;
        }

        if (!esp_now_is_peer_exist(request.mac)) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, request.mac, sizeof(peer.peer_addr));
            peer.channel = WIFI_CHANNEL;
            peer.ifidx = WIFI_IF_AP;
            peer.encrypt = false;

            esp_err_t add_peer_err = esp_now_add_peer(&peer);
            if (add_peer_err != ESP_OK && add_peer_err != ESP_ERR_ESPNOW_EXIST) {
                ESP_LOGW(TAG,
                         "Failed to add ESP-NOW peer " MACSTR ": %s",
                         MAC2STR(request.mac),
                         esp_err_to_name(add_peer_err));
                continue;
            }
        }

        char response[16];
        snprintf(response,
                 sizeof(response),
                 ESPNOW_LIGHT_STATE_RESPONSE_PREFIX "%lu",
                 (unsigned long)light_state);

        esp_err_t send_err = esp_now_send(request.mac,
                                          (const uint8_t *)response,
                                          strlen(response));
        if (send_err == ESP_OK) {
            ESP_LOGI(TAG,
                     "Sent ESP-NOW light state to " MACSTR ": %s",
                     MAC2STR(request.mac),
                     response);
        } else {
            ESP_LOGW(TAG,
                     "Failed to send ESP-NOW light state to " MACSTR ": %s",
                     MAC2STR(request.mac),
                     esp_err_to_name(send_err));
        }
    }
}

static void init_espnow(void)
{
    espnow_request_queue = xQueueCreate(8, sizeof(espnow_light_state_request_t));
    assert(espnow_request_queue != NULL);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    xTaskCreate(
        espnow_light_state_task,
        "espnow_light_state",
        3072,
        NULL,
        8,
        NULL
    );

    ESP_LOGI(TAG, "ESP-NOW light-state endpoint ready");
}

/*
 * Wi-Fi AP event logger.
 *
 * Useful for debugging client joins/leaves and disconnect reasons.
 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "SoftAP started");
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event =
                (wifi_event_ap_staconnected_t *)event_data;

            ESP_LOGI(TAG,
                     "Station joined: " MACSTR ", AID=%d",
                     MAC2STR(event->mac),
                     event->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event =
                (wifi_event_ap_stadisconnected_t *)event_data;

            ESP_LOGW(TAG,
                     "Station left: " MACSTR ", AID=%d, reason=%d",
                     MAC2STR(event->mac),
                     event->aid,
                     event->reason);
            break;
        }

        default:
            ESP_LOGI(TAG, "WiFi event: %ld", event_id);
            break;
    }
}

/*
 * Initialize ESP32-C3 as a Wi-Fi SoftAP.
 *
 * Major steps:
 * 1. Initialize networking stack
 * 2. Create default AP network interface
 * 3. Initialize Wi-Fi driver
 * 4. Register Wi-Fi event handler
 * 5. Configure SSID/password/security
 * 6. Start AP
 * 7. Wait briefly before starting services
 */
static void init_wifi_ap(void)
{
    // Initialize TCP/IP networking stack.
    ESP_ERROR_CHECK(esp_netif_init());

    // Create the default event loop used by Wi-Fi and networking.
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create the default SoftAP network interface.
    // This also starts the default DHCP server for clients.
    esp_netif_create_default_wifi_ap();

    // Initialize Wi-Fi driver with default settings.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handler for AP start/client connect/client disconnect events.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL,
        NULL
    ));

    // Zero-initialize Wi-Fi config so no unused fields contain garbage.
    wifi_config_t wifi_config = {};

    // Configure AP SSID.
    strncpy((char *)wifi_config.ap.ssid,
            WIFI_SSID,
            sizeof(wifi_config.ap.ssid) - 1);

    // Configure AP password.
    strncpy((char *)wifi_config.ap.password,
            WIFI_PASS,
            sizeof(wifi_config.ap.password) - 1);

    // Configure AP behavior.
    wifi_config.ap.ssid_len = strlen(WIFI_SSID);
    wifi_config.ap.channel = WIFI_CHANNEL;
    wifi_config.ap.max_connection = 8;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    // Do not require Protected Management Frames.
    // This improves compatibility with simple ESP-to-ESP setups.
    wifi_config.ap.pmf_cfg.required = false;
    // wifi_config.ap.pmf_cfg.capable = false;

    int k = 0;

    // Put Wi-Fi into AP mode.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Apply AP configuration.
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Disable Wi-Fi power saving for better ESP-to-ESP stability.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Start SoftAP.
    ESP_ERROR_CHECK(esp_wifi_start());

    // Give SoftAP, WPA, beaconing, and DHCP time to stabilize
    // before starting HTTP services.
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG,
             "SoftAP ready. SSID=%s, password=%s, channel=%d",
             WIFI_SSID,
             WIFI_PASS,
             wifi_config.ap.channel);
}

/*
 * Initialize GPIOs for:
 *  - LED output
 *  - Button input with interrupt
 */
static void init_gpio(void)
{
    // --- LED setup ---
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    // --- Button setup ---
    gpio_config_t button_conf = {};

    button_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);

    // Input mode
    button_conf.mode = GPIO_MODE_INPUT;

    // Enable internal pull-up (button connects to GND when pressed)
    button_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    button_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    // Trigger interrupt on falling edge (press)
    button_conf.intr_type = GPIO_INTR_NEGEDGE;

    ESP_ERROR_CHECK(gpio_config(&button_conf));

    // --- Queue for ISR → task communication ---
    button_queue = xQueueCreate(8, sizeof(uint32_t));
    assert(button_queue != NULL);

    // --- Install ISR service ---
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    // Attach ISR handler to button pin
    ESP_ERROR_CHECK(gpio_isr_handler_add(
        BUTTON_GPIO,
        button_isr_handler,
        NULL
    ));

    // --- Start button task ---
    xTaskCreate(
        button_task,
        "button_task",
        2048,
        NULL,
        10,
        NULL
    );
}

/*
 * Application entry point.
 */
void app_main(void)
{
    // Initialize NVS.
    // Wi-Fi requires NVS for internal calibration/config data.
    esp_err_t ret = nvs_flash_init();

    // Recover if NVS partition is full or from an incompatible version.
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    // Restore persisted light state before starting hardware and APIs.
    load_light_state();

    // Initialize hardware GPIO first.
    init_gpio();

    // Start Wi-Fi SoftAP and wait for it to stabilize.
    init_wifi_ap();

    // Start ESP-NOW request/response API after Wi-Fi is ready.
    init_espnow();

    // Start HTTP API after AP is ready.
    start_http_server();

    // Main application loop.
    while (true) {
        gpio_set_level(LED_GPIO, light_state);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
