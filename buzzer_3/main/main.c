
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

// ===== CONFIG =====
#define WIFI_SSID "Noobs"
#define WIFI_PASS "flashno11"
#define BUZZER_PIN GPIO_NUM_10
#define BUTTON_PIN GPIO_NUM_3
#define PORT 8080
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "BUZZER_SERVER";
static EventGroupHandle_t s_wifi_event_group;
static int retry_num = 0;

// Biến trạng thái còi
static volatile bool buzzer_on = false;
static volatile bool auto_buzzer_mode = false;
static volatile TickType_t auto_buzzer_start_tick = 0;

// Queue để xử lý button event
static QueueHandle_t button_event_queue = NULL;

// ===== THÊM: Biến đếm request để debug =====
static uint32_t total_requests = 0;
static uint32_t buzzer_triggers = 0;

// HTML response
static const char *html_response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<title>ESP32-C3 Buzzer Debug</title>"
    "<style>body{font-family:Arial;background:#1a1a2e;color:#fff;padding:40px;text-align:center}"
    ".box{background:#16213e;padding:30px;border-radius:15px;max-width:600px;margin:0 auto}"
    "h1{color:#4caf50}"
    ".ip{background:#0f3460;padding:15px;border-radius:8px;margin:20px 0;font-size:20px;font-weight:bold}"
    ".stats{background:#2c3e50;padding:15px;border-radius:8px;margin:20px 0}"
    "button{background:#ff9800;color:#fff;border:none;padding:15px 30px;border-radius:8px;font-size:16px;cursor:pointer;margin:10px}"
    "button:hover{background:#f57c00}"
    ".status{margin-top:20px;padding:10px;background:#2c3e50;border-radius:5px}"
    ".endpoint{background:#34495e;padding:10px;margin:10px 0;border-radius:5px;font-family:monospace;font-size:14px}</style>"
    "</head><body>"
    "<div class='box'>"
    "<h1>🔊 ESP32-C3 Buzzer Server</h1>"
    "<div class='ip'>IP: %s</div>"
    "<div class='stats'>"
    "<div>Total Requests: %" PRIu32 "</div>"
    "<div>Buzzer Triggers: %" PRIu32 "</div>"
    "<div>Status: %s</div>"
    "</div>"
    "<p><strong>📡 Supported Endpoints:</strong></p>"
    "<div class='endpoint'>GET /buzzer</div>"
    "<div class='endpoint'>POST /buzzer</div>"
    "<div class='endpoint'>POST /notify</div>"
    "<div class='endpoint'>GET /trigger (NEW)</div>"
    "<div class='endpoint'>POST /trigger (NEW)</div>"
    "<p style='margin-top:30px'><strong>🎯 How to trigger from ESP32-CAM:</strong></p>"
    "<div style='text-align:left;background:#34495e;padding:15px;border-radius:8px;margin:20px 0'>"
    "<code style='color:#4caf50'>curl http://%s:8080/buzzer</code><br>"
    "<code style='color:#4caf50'>curl -X POST http://%s:8080/notify</code>"
    "</div>"
    "<button onclick='testBuzzer()'>🔔 Test Buzzer</button>"
    "<div class='status' id='st'>Ready</div>"
    "</div>"
    "<script>"
    "function testBuzzer(){"
    "document.getElementById('st').textContent='Testing...';"
    "fetch('/buzzer').then(r=>r.text()).then(t=>{"
    "document.getElementById('st').textContent=' Buzzer activated! Reloading...';"
    "setTimeout(()=>location.reload(),2000);"
    "}).catch(e=>{"
    "document.getElementById('st').textContent=' Error: '+e.message"
    "})"
    "}"
    "</script>"
    "</body></html>";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (retry_num < 40)
        {
            esp_wifi_connect();
            retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void auto_buzzer_task(void *arg)
{
    ESP_LOGI(TAG, "[AUTO BUZZER] Task started");

    while (1)
    {
        if (auto_buzzer_mode && buzzer_on)
        {
            TickType_t current_tick = xTaskGetTickCount();
            TickType_t elapsed_tick = current_tick - auto_buzzer_start_tick;
            uint32_t elapsed_ms = elapsed_tick * portTICK_PERIOD_MS;

            if (elapsed_ms >= 20000)
            {
                buzzer_on = false;
                gpio_set_level(BUZZER_PIN, 0);
                auto_buzzer_mode = false;
                ESP_LOGI(TAG, "[AUTO BUZZER] Auto-off after 20 seconds");
            }
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void activate_buzzer_auto(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  🚨 BUZZER ACTIVATED FROM ESP32-CAM  🚨  ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    buzzer_on = true;
    auto_buzzer_mode = true;
    auto_buzzer_start_tick = xTaskGetTickCount();
    gpio_set_level(BUZZER_PIN, 1);

    buzzer_triggers++;
    // FIX: Dùng PRIu32 cho uint32_t
    ESP_LOGI(TAG, "[AUTO BUZZER] ON (will auto-off after 20s) - Trigger #%" PRIu32, buzzer_triggers);
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t gpio_num = BUTTON_PIN;
    xQueueSendFromISR(button_event_queue, &gpio_num, NULL);
}

static void button_task(void *arg)
{
    uint32_t io_num;
    uint32_t last_press_time = 0;
    const uint32_t DEBOUNCE_MS = 500;

    ESP_LOGI(TAG, "[BUTTON] Task started");

    while (1)
    {
        if (xQueueReceive(button_event_queue, &io_num, portMAX_DELAY))
        {
            uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            vTaskDelay(50 / portTICK_PERIOD_MS);
            int level = gpio_get_level(BUTTON_PIN);

            ESP_LOGW(TAG, "[BUTTON] ISR triggered! Level after 50ms=%d", level);

            if ((current_time - last_press_time) > DEBOUNCE_MS && level == 0)
            {
                last_press_time = current_time;
                buzzer_on = !buzzer_on;

                if (buzzer_on)
                {
                    auto_buzzer_mode = false;
                    gpio_set_level(BUZZER_PIN, 1);
                    ESP_LOGI(TAG, ">>> [BUTTON] Buzzer turned ON (manual mode) 🔊 <<<");
                }
                else
                {
                    auto_buzzer_mode = false;
                    gpio_set_level(BUZZER_PIN, 0);
                    ESP_LOGI(TAG, ">>> [BUTTON] Buzzer turned OFF 🔇 <<<");

                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    gpio_set_level(BUZZER_PIN, 1);
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    gpio_set_level(BUZZER_PIN, 0);
                    ESP_LOGI(TAG, "[BUTTON] Confirmation beep done");
                }
            }
            else
            {
                if (level == 1)
                {
                    ESP_LOGW(TAG, "[BUTTON] Ignored - noise detected");
                }
                else
                {
                    ESP_LOGW(TAG, "[BUTTON] Ignored - debounce");
                }
            }
        }
    }
}

static void http_server_task(void *pvParameters)
{
    static char rx_buffer[512];
    static char html_buffer[4096];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = 0;

    while (1)
    {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;

        int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (listen_sock < 0)
        {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        ESP_LOGI(TAG, "Socket created");

        int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0)
        {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(listen_sock);
            break;
        }
        ESP_LOGI(TAG, "Socket bound, port %d", PORT);

        err = listen(listen_sock, 1);
        if (err != 0)
        {
            ESP_LOGE(TAG, "Error during listen: errno %d", errno);
            close(listen_sock);
            break;
        }

        ESP_LOGI(TAG, "Socket listening");
        ESP_LOGI(TAG, "========================");
        ESP_LOGI(TAG, "🎯 Waiting for requests from ESP32-CAM...");
        ESP_LOGI(TAG, "");

        while (1)
        {
            struct sockaddr_storage source_addr;
            socklen_t addr_len = sizeof(source_addr);
            int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
            if (sock < 0)
            {
                ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
                break;
            }

            struct sockaddr_in *source_addr_ip4 = (struct sockaddr_in *)&source_addr;
            inet_ntoa_r(source_addr_ip4->sin_addr, addr_str, sizeof(addr_str) - 1);

            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0)
            {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                close(sock);
                continue;
            }
            else
            {
                rx_buffer[len] = 0;
                total_requests++;

                // ===== LOGGING CHI TIẾT =====
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "╔══════════════════════════════════════════════╗");
                // FIX: Dùng PRIu32 cho uint32_t
                ESP_LOGI(TAG, "║          📥 REQUEST #%" PRIu32 " RECEIVED            ║", total_requests);
                ESP_LOGI(TAG, "╚══════════════════════════════════════════════╝");
                ESP_LOGI(TAG, "From IP: %s", addr_str);
                ESP_LOGI(TAG, "Length: %d bytes", len);
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "───────── REQUEST CONTENT START ─────────");
                ESP_LOGI(TAG, "%s", rx_buffer);
                ESP_LOGI(TAG, "───────── REQUEST CONTENT END ───────────");
                ESP_LOGI(TAG, "");

                // ===== KIỂM TRA TẤT CẢ CÁC ENDPOINT =====
                bool is_buzzer_request = false;
                const char *matched_endpoint = "NONE";

                if (strstr(rx_buffer, "GET /buzzer") != NULL)
                {
                    is_buzzer_request = true;
                    matched_endpoint = "GET /buzzer";
                }
                else if (strstr(rx_buffer, "POST /buzzer") != NULL)
                {
                    is_buzzer_request = true;
                    matched_endpoint = "POST /buzzer";
                }
                else if (strstr(rx_buffer, "POST /notify") != NULL)
                {
                    is_buzzer_request = true;
                    matched_endpoint = "POST /notify";
                }
                else if (strstr(rx_buffer, "GET /trigger") != NULL)
                {
                    is_buzzer_request = true;
                    matched_endpoint = "GET /trigger";
                }
                else if (strstr(rx_buffer, "POST /trigger") != NULL)
                {
                    is_buzzer_request = true;
                    matched_endpoint = "POST /trigger";
                }
                else if (strstr(rx_buffer, "GET / ") != NULL)
                {
                    matched_endpoint = "GET / (homepage)";
                }
                else
                {
                    // Tìm method và path
                    char method[16] = {0};
                    char path[128] = {0};
                    if (sscanf(rx_buffer, "%15s %127s", method, path) == 2)
                    {
                        snprintf((char *)&matched_endpoint, 256, "%s %s (UNKNOWN)", method, path);
                    }
                }

                ESP_LOGI(TAG, "📍 Matched Endpoint: %s", matched_endpoint);

                if (is_buzzer_request)
                {
                    ESP_LOGI(TAG, "✅ BUZZER REQUEST DETECTED!");
                    ESP_LOGI(TAG, "🔔 Activating buzzer...");

                    const char *response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nOK - Buzzer Activated";
                    send(sock, response, strlen(response), 0);
                    close(sock);

                    activate_buzzer_auto();
                }
                else
                {
                    ESP_LOGI(TAG, "📄 Serving homepage");

                    esp_netif_ip_info_t ip_info;
                    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                    esp_netif_get_ip_info(netif, &ip_info);
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

                    const char *buzzer_status = buzzer_on ? "ON 🔊" : "OFF 🔇";
                    snprintf(html_buffer, sizeof(html_buffer), html_response,
                             ip_str, total_requests, buzzer_triggers, buzzer_status, ip_str, ip_str);
                    send(sock, html_buffer, strlen(html_buffer), 0);
                    close(sock);
                }

                ESP_LOGI(TAG, "");
            }
        }

        if (listen_sock != -1)
        {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(listen_sock, 0);
            close(listen_sock);
        }
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ESP32-C3 Buzzer Server (DEBUG VERSION)    ║");
    ESP_LOGI(TAG, "╚═════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Configure buzzer
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BUZZER_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(BUZZER_PIN, 0);
    ESP_LOGI(TAG, "✅ [GPIO] Buzzer ready on GPIO%d", BUZZER_PIN);

    // Create button event queue
    button_event_queue = xQueueCreate(10, sizeof(uint32_t));

    // Install ISR service
    gpio_install_isr_service(0);
    ESP_LOGI(TAG, "✅ [ISR] Service installed");

    // Configure button
    gpio_config_t button_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&button_conf);
    ESP_LOGI(TAG, "✅ [GPIO] Button configured on GPIO%d", BUTTON_PIN);

    // Add ISR handler
    gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, NULL);
    ESP_LOGI(TAG, "✅ [ISR] Button handler added");

    // Create button task
    xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);

    // Create auto buzzer task
    xTaskCreate(auto_buzzer_task, "auto_buzzer", 4096, NULL, 5, NULL);

    // Initialize WiFi
    wifi_init_sta();

    // Get and display IP
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(netif, &ip_info);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║          🌐 NETWORK INFORMATION             ║");
    ESP_LOGI(TAG, "╚═════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "📡 WiFi SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "🌐 IP Address: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "🔌 Port: %d", PORT);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "🎯 To trigger buzzer from ESP32-CAM, use:");
    ESP_LOGI(TAG, "   http://" IPSTR ":8080/buzzer", IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "   http://" IPSTR ":8080/notify", IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "   http://" IPSTR ":8080/trigger", IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "");

    // Start HTTP server
    xTaskCreate(http_server_task, "http_server", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "✅ System Ready - Listening for requests...");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "");

    // Status monitoring loop với thông tin chi tiết hơn
    uint32_t loop_count = 0;
    while (1)
    {
        loop_count++;
        const char *mode_str = auto_buzzer_mode ? "AUTO " : "MANUAL ";
        const char *buzzer_str = buzzer_on ? "ON " : "OFF ";
    
        ESP_LOGI(TAG, "[STATUS #%" PRIu32 "] Buzzer=%s | Mode=%s | Requests=%" PRIu32 " | Triggers=%" PRIu32,
                 loop_count, buzzer_str, mode_str, total_requests, buzzer_triggers);

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}