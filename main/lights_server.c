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
#include "nvs_flash.h"

#include "esp_http_server.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define WIFI_SSID "lights"
#define WIFI_PASS "123456789"

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

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");

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

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_light_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set_light_uri));

    ESP_LOGI(TAG, "HTTP server started");

    return server;
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
    wifi_config.ap.channel = 1;
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

    // Initialize hardware GPIO first.
    init_gpio();

    // Start Wi-Fi SoftAP and wait for it to stabilize.
    init_wifi_ap();

    // Start HTTP API after AP is ready.
    start_http_server();

    // Main application loop.
    while (true) {
        // GPIO8 onboard LED is commonly active-low on many ESP32-C3 boards.
        // If your LED behaves inverted, remove the "!".
        gpio_set_level(LED_GPIO, !light_state);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
