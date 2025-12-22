// ===== ESP32-CAM Person Detection with Auto Email =====
// Pure ESP-IDF Implementation - Fixed for espressif/esp32-camera

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "sensor.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mbedtls/base64.h"
#include "driver/gpio.h"
#include "http_notifier.h"
#include "cJSON.h"

// ===== CONFIG =====
#define WIFI_SSID "Noobs"
#define WIFI_PASS "flashno11"
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "huynhconghuy172004@gmail.com"
#define AUTHOR_PASSWORD "glmhjoukegbclwwa"
#define RECIPIENT_EMAIL "bach9747@gmail.com"

// ===== HTTP NOTIFICATION CONFIG =====
#define ESP32C3_IP "10.160.206.5"

static const char *TAG = "ESP32-CAM";

// ===== CAMERA PINS (AI-THINKER ESP32-CAM) =====
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22
#define FLASH_GPIO_NUM 4

// ===== GLOBALS =====
static httpd_handle_t server = NULL;
static camera_fb_t *photo_fb = NULL;
static volatile bool email_sending = false;
static int photo_counter = 0;

// ===== WIFI EVENT HANDLER =====
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// ===== CAMERA INIT =====
static esp_err_t init_camera(void)
{
    camera_config_t config;
    memset(&config, 0, sizeof(camera_config_t));

    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_pclk = CAM_PIN_PCLK;
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return err;
    }

    ESP_LOGI(TAG, "Camera initialized");
    return ESP_OK;
}

// ===== CAPTURE PHOTO =====
static esp_err_t capture_photo(void)
{
    ESP_LOGI(TAG, "Flushing camera buffers...");
    for (int i = 0; i < 8; i++)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb)
        {
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    gpio_set_level(FLASH_GPIO_NUM, 1);
    ESP_LOGI(TAG, "Flash ON");
    vTaskDelay(pdMS_TO_TICKS(200));

    photo_fb = esp_camera_fb_get();
    if (!photo_fb)
    {
        ESP_LOGE(TAG, "Camera capture failed");
        gpio_set_level(FLASH_GPIO_NUM, 0);
        return ESP_FAIL;
    }

    gpio_set_level(FLASH_GPIO_NUM, 0);
    ESP_LOGI(TAG, "Flash OFF - Captured %d bytes", photo_fb->len);

    return ESP_OK;
}

// ===== SMTP EMAIL SENDER (SSL) =====
static esp_err_t send_email_smtp(void)
{
    if (email_sending || !photo_fb)
    {
        return ESP_FAIL;
    }
    email_sending = true;

    ESP_LOGI(TAG, "Connecting to SMTP server...");

    // TLS config
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_tls_t *tls = esp_tls_init();
    if (!tls)
    {
        ESP_LOGE(TAG, "Failed to allocate esp_tls");
        email_sending = false;
        return ESP_FAIL;
    }

    if (esp_tls_conn_new_sync(SMTP_HOST, strlen(SMTP_HOST), SMTP_PORT, &cfg, tls) != 1)
    {
        ESP_LOGE(TAG, "TLS connection failed");
        esp_tls_conn_destroy(tls);
        email_sending = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TLS connected to SMTP");

    // SMTP commands
    char rx_buffer[512];
    char tx_buffer[512];

    // Read greeting
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);
    ESP_LOGI(TAG, "SMTP: %s", rx_buffer);

    // EHLO
    snprintf(tx_buffer, sizeof(tx_buffer), "EHLO esp32cam\r\n");
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);

    // AUTH LOGIN
    snprintf(tx_buffer, sizeof(tx_buffer), "AUTH LOGIN\r\n");
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);

    // Base64 encode email
    size_t olen;
    unsigned char email_b64[128];
    mbedtls_base64_encode(email_b64, sizeof(email_b64), &olen,
                          (unsigned char *)AUTHOR_EMAIL, strlen(AUTHOR_EMAIL));
    snprintf(tx_buffer, sizeof(tx_buffer), "%s\r\n", email_b64);
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);

    // Base64 encode password
    unsigned char pass_b64[128];
    mbedtls_base64_encode(pass_b64, sizeof(pass_b64), &olen,
                          (unsigned char *)AUTHOR_PASSWORD, strlen(AUTHOR_PASSWORD));
    snprintf(tx_buffer, sizeof(tx_buffer), "%s\r\n", pass_b64);
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);

    // MAIL FROM
    snprintf(tx_buffer, sizeof(tx_buffer), "MAIL FROM:<%s>\r\n", AUTHOR_EMAIL);
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);

    // RCPT TO
    snprintf(tx_buffer, sizeof(tx_buffer), "RCPT TO:<%s>\r\n", RECIPIENT_EMAIL);
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);

    // DATA
    snprintf(tx_buffer, sizeof(tx_buffer), "DATA\r\n");
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);

    // Email headers and body with attachment
    snprintf(tx_buffer, sizeof(tx_buffer),
             "From: ESP32CAM <%s>\r\n"
             "To: <%s>\r\n"
             "Subject: Person Detected!\r\n"
             "MIME-Version: 1.0\r\n"
             "Content-Type: multipart/mixed; boundary=\"BOUNDARY\"\r\n\r\n"
             "--BOUNDARY\r\n"
             "Content-Type: text/plain\r\n\r\n"
             "Person detected! Photo attached.\r\n\r\n"
             "--BOUNDARY\r\n"
             "Content-Type: image/jpeg; name=\"person.jpg\"\r\n"
             "Content-Disposition: attachment; filename=\"person.jpg\"\r\n"
             "Content-Transfer-Encoding: base64\r\n\r\n",
             AUTHOR_EMAIL, RECIPIENT_EMAIL);
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));

    // Send photo in base64 chunks
    size_t image_b64_len = (photo_fb->len * 4 / 3) + 4;
    unsigned char *image_b64 = malloc(image_b64_len);
    if (image_b64)
    {
        mbedtls_base64_encode(image_b64, image_b64_len, &olen, photo_fb->buf, photo_fb->len);

        // Send in chunks with line breaks
        for (size_t i = 0; i < olen; i += 76)
        {
            size_t chunk = (olen - i > 76) ? 76 : (olen - i);
            esp_tls_conn_write(tls, image_b64 + i, chunk);
            esp_tls_conn_write(tls, "\r\n", 2);
        }
        free(image_b64);
    }

    // End email
    snprintf(tx_buffer, sizeof(tx_buffer), "\r\n--BOUNDARY--\r\n.\r\n");
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));
    esp_tls_conn_read(tls, rx_buffer, sizeof(rx_buffer) - 1);

    // QUIT
    snprintf(tx_buffer, sizeof(tx_buffer), "QUIT\r\n");
    esp_tls_conn_write(tls, tx_buffer, strlen(tx_buffer));

    esp_tls_conn_destroy(tls);

    // Free photo framebuffer
    if (photo_fb)
    {
        esp_camera_fb_return(photo_fb);
        photo_fb = NULL;
    }

    photo_counter++;
    ESP_LOGI(TAG, "Email sent successfully! Total: %d", photo_counter);

    // ===== GỬI HTTP NOTIFICATION ĐẾN ESP32-C3 =====
    ESP_LOGI(TAG, "Sending HTTP notification to ESP32-C3...");

    // Tạo message với timestamp
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char notify_msg[128];
    snprintf(notify_msg, sizeof(notify_msg),
             "Email #%d sent! Person detected at %02d:%02d:%02d",
             photo_counter, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    // Gửi HTTP POST đến ESP32-C3
    esp_err_t result = send_http_notification(ESP32C3_IP, photo_counter, notify_msg);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "✓ HTTP notification sent to ESP32-C3 at %s", ESP32C3_IP);
    }
    else
    {
        ESP_LOGW(TAG, "✗ Failed to send HTTP notification to %s", ESP32C3_IP);
        ESP_LOGW(TAG, "Check: 1) ESP32-C3 is powered on, 2) IP address is correct, 3) WiFi connected");
    }

    email_sending = false;
    return ESP_OK;
}

// ===== HTTP HANDLERS =====
static const char *HTML_PAGE_PART1 =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-CAM AI Auto Detection</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:Arial;background:#1a1a2e;color:#fff;padding:20px}"
    ".container{max-width:800px;margin:0 auto;background:#16213e;border-radius:15px;overflow:hidden}"
    ".header{background:#0f3460;padding:20px;text-align:center}"
    ".stream-box{position:relative;background:#000}"
    ".stream-box img{width:100%;display:block}"
    ".stream-box canvas{position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none}"
    ".status{position:absolute;top:10px;left:10px;background:#4caf50;color:#fff;padding:10px 15px;border-radius:20px;font-weight:bold;z-index:10}"
    ".status.detecting{background:#ff9800;animation:pulse 1s infinite}"
    "@keyframes pulse{50%{transform:scale(1.05)}}"
    ".fps{position:absolute;top:10px;right:10px;background:#2196f3;color:#fff;padding:10px 15px;border-radius:20px;font-weight:bold;z-index:10}"
    ".controls{padding:20px;display:flex;gap:10px;flex-wrap:wrap}"
    ".controls button{flex:1;min-width:150px;padding:15px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;background:#2196f3;color:#fff}"
    ".controls button:disabled{background:#555;cursor:not-allowed;opacity:0.6}"
    ".controls button.active{background:#4caf50}"
    ".controls button.off{background:#666}"
    ".message{margin:0 20px 20px;padding:15px;border-radius:8px;text-align:center;font-weight:bold;display:none}"
    ".message.show{display:block}"
    ".message.success{background:#4caf50}"
    ".message.error{background:#f44336}"
    ".message.loading{background:#ff9800}"
    ".stats{display:flex;gap:10px;padding:0 20px 20px}"
    ".stat{flex:1;background:#0f3460;padding:15px;border-radius:8px;text-align:center}"
    ".stat-label{font-size:12px;opacity:0.8}"
    ".stat-value{font-size:24px;font-weight:bold;margin-top:5px}"
    ".info{padding:20px;text-align:center;font-size:14px;opacity:0.8}"
    "</style>"
    "</head><body>"
    "<div class='container'>"
    "<div class='header'><h1>ESP32-CAM AI Auto Detection</h1><p>ESP-IDF with TensorFlow.js</p></div>"
    "<div class='stream-box'>"
    "<div class='status' id='st'>Loading...</div>"
    "<div class='fps' id='fps'>-- FPS</div>"
    "<img id='stream' src='/stream'>"
    "<canvas id='canvas'></canvas>"
    "</div>"
    "<div class='controls'>"
    "<button id='btn1' onclick='toggleDetect()' class='active'>[DETECT] ON</button>"
    "<button id='btn2' onclick='toggleAuto()' class='active'>[AUTO] ON</button>"
    "<button id='btn3' onclick='manualCapture()'>[MANUAL] Capture</button>"
    "</div>"
    "<div id='msg' class='message'></div>"
    "<div class='stats'>"
    "<div class='stat'><div class='stat-label'>Detected</div><div class='stat-value' id='cnt'>0</div></div>"
    "<div class='stat'><div class='stat-label'>Photos Sent</div><div class='stat-value' id='photos'>0</div></div>"
    "<div class='stat'><div class='stat-label'>Confidence</div><div class='stat-value' id='conf'>--</div></div>"
    "<div class='stat'><div class='stat-label'>People</div><div class='stat-value' id='ppl'>0</div></div>"
    "</div>"
    "<div class='info'>"
    "<p>[AUTO] Tự động: Phát hiện người trong 10 giây → tự động chụp và gửi email</p>"
    "<p>[COOLDOWN] 20 giây giữa các lần gửi email tự động</p>"
    "<p>[EMAIL] " RECIPIENT_EMAIL "</p>"
    "</div>"
    "</div>"
    "<script src='https://cdn.jsdelivr.net/npm/@tensorflow/tfjs@3.11.0'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/@tensorflow-models/coco-ssd@2.2.2'></script>"
    "<script>";

static const char *HTML_PAGE_PART2 =
    "let model,stream,canvas,ctx;"
    "let detectionOn=true,autoOn=true,capturing=false;"
    "let frameCount=0,lastTime=Date.now(),totalDetections=0,photosSent=0;"
    "let personTime=null,cooldown=false;"
    "const DETECT_TIME=10000,COOLDOWN_TIME=20000;"
    "function msg(txt,type){"
    "const m=document.getElementById('msg');"
    "m.textContent=txt;m.className='message show '+type;"
    "setTimeout(()=>m.className='message',5000)"
    "}"
    "async function doCapture(isAuto){"
    "if(capturing)return false;"
    "capturing=true;"
    "const b1=document.getElementById('btn1');"
    "const b2=document.getElementById('btn2');"
    "const b3=document.getElementById('btn3');"
    "b1.disabled=true;b2.disabled=true;b3.disabled=true;"
    "b3.textContent='[WAIT] Sending...';"
    "stream.src='';"
    "const prefix=isAuto?'[AUTO]':'[MANUAL]';"
    "msg(prefix+' Đang chụp và gửi email...','loading');"
    "await new Promise(r=>setTimeout(r,2000));"
    "try{"
    "const res=await fetch('/capture',{signal:AbortSignal.timeout(60000)});"
    "const txt=await res.text();"
    "if(txt.includes('OK')){"
    "photosSent++;"
    "document.getElementById('photos').textContent=photosSent;"
    "msg(prefix+' Email đã gửi thành công!','success');"
    "return true"
    "}else{"
    "msg(prefix+' Gửi thất bại!','error');"
    "return false"
    "}"
    "}catch(e){"
    "msg(prefix+' Lỗi: '+e.message,'error');"
    "return false"
    "}finally{"
    "capturing=false;"
    "b1.disabled=false;b2.disabled=false;b3.disabled=false;"
    "b3.textContent='[MANUAL] Capture';"
    "setTimeout(()=>{stream.src='/stream?t='+Date.now();msg('[OK] Đã khởi động lại stream','success')},2000)"
    "}"
    "}"
    "async function manualCapture(){await doCapture(false)}"
    "async function autoCapture(){"
    "const ok=await doCapture(true);"
    "if(ok){"
    "cooldown=true;"
    "setTimeout(()=>cooldown=false,COOLDOWN_TIME)"
    "}"
    "}"
    "async function init(){"
    "stream=document.getElementById('stream');"
    "canvas=document.getElementById('canvas');"
    "ctx=canvas.getContext('2d');"
    "const st=document.getElementById('st');"
    "st.textContent='Loading AI...';"
    "try{"
    "model=await cocoSsd.load();"
    "st.textContent='[OK] Ready';"
    "st.className='status';"
    "stream.onload=()=>{"
    "canvas.width=stream.naturalWidth;"
    "canvas.height=stream.naturalHeight;"
    "detect()"
    "};"
    "if(stream.complete&&stream.naturalWidth>0){"
    "canvas.width=stream.naturalWidth;"
    "canvas.height=stream.naturalHeight;"
    "detect()"
    "}"
    "}catch(e){st.textContent='[X] AI Failed'}"
    "}"
    "async function detect(){"
    "if(!model||!stream.complete){setTimeout(detect,100);return}"
    "frameCount++;"
    "const now=Date.now();"
    "if(now-lastTime>=1000){"
    "document.getElementById('fps').textContent=Math.round(frameCount*1000/(now-lastTime))+' FPS';"
    "frameCount=0;lastTime=now"
    "}"
    "ctx.clearRect(0,0,canvas.width,canvas.height);"
    "if(detectionOn&&!capturing){"
    "try{"
    "const preds=await model.detect(stream);"
    "const people=preds.filter(p=>p.class==='person'&&p.score>0.3);"
    "people.forEach(p=>{"
    "ctx.strokeStyle='#0f0';ctx.lineWidth=3;"
    "ctx.strokeRect(p.bbox[0],p.bbox[1],p.bbox[2],p.bbox[3]);"
    "ctx.fillStyle='#0f0';ctx.font='16px Arial';"
    "ctx.fillText(p.class+' '+Math.round(p.score*100)+'%',p.bbox[0],p.bbox[1]>10?p.bbox[1]-5:10)"
    "});"
    "const st=document.getElementById('st');"
    "if(people.length>0){"
    "st.className='status detecting';"
    "totalDetections+=people.length;"
    "document.getElementById('cnt').textContent=totalDetections;"
    "document.getElementById('ppl').textContent=people.length;"
    "document.getElementById('conf').textContent=Math.round(Math.max(...people.map(p=>p.score))*100)+'%';"
    "if(autoOn&&!cooldown&&!capturing){"
    "if(personTime===null)personTime=Date.now();"
    "const elapsed=Date.now()-personTime;"
    "const remain=Math.ceil((DETECT_TIME-elapsed)/1000);"
    "if(remain>0){"
    "st.textContent='[!] '+people.length+' Người - Tự động sau '+remain+'s'"
    "}else{"
    "personTime=null;"
    "autoCapture()"
    "}"
    "}else{"
    "st.textContent='[!] '+people.length+' Người'+(cooldown?' (Cooldown)':'')"
    "}"
    "}else{"
    "st.className='status';st.textContent='[SCAN] Đang quét...';"
    "document.getElementById('ppl').textContent='0';"
    "document.getElementById('conf').textContent='--';"
    "personTime=null"
    "}"
    "}catch(e){}"
    "}"
    "requestAnimationFrame(detect)"
    "}"
    "function toggleDetect(){"
    "detectionOn=!detectionOn;"
    "const btn=document.getElementById('btn1');"
    "const st=document.getElementById('st');"
    "if(detectionOn){"
    "btn.textContent='[DETECT] ON';btn.className='active';"
    "st.textContent='[SCAN] Đang quét...'"
    "}else{"
    "btn.textContent='[OFF] Detection';btn.className='off';"
    "st.textContent='[OFF] Tạm dừng';"
    "ctx.clearRect(0,0,canvas.width,canvas.height);personTime=null"
    "}"
    "}"
    "function toggleAuto(){"
    "autoOn=!autoOn;"
    "const btn=document.getElementById('btn2');"
    "if(autoOn){"
    "btn.textContent='[AUTO] ON';btn.className='active';"
    "msg('[OK] Đã bật chế độ tự động','success')"
    "}else{"
    "btn.textContent='[OFF] Auto';btn.className='off';"
    "msg('[OFF] Đã tắt chế độ tự động','loading');personTime=null"
    "}"
    "}"
    "window.onload=init"
    "</script>"
    "</body></html>";

// HTTP handler: Home page
static esp_err_t home_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, HTML_PAGE_PART1, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, HTML_PAGE_PART2, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// HTTP handler: Stream
static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");

    while (true)
    {
        fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        size_t hlen = snprintf(part_buf, sizeof(part_buf),
                               "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               fb->len);

        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK)
        {
            esp_camera_fb_return(fb);
            break;
        }

        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        if (res != ESP_OK)
        {
            esp_camera_fb_return(fb);
            break;
        }

        res = httpd_resp_send_chunk(req, "\r\n", 2);

        esp_camera_fb_return(fb);

        if (res != ESP_OK)
        {
            break;
        }
    }

    return res;
}

// HTTP handler: Capture and send email
static esp_err_t capture_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== CAPTURE REQUEST ===");

    httpd_resp_set_type(req, "text/plain");

    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_err_t res = capture_photo();
    if (res == ESP_OK)
    {
        res = send_email_smtp();
        if (res == ESP_OK)
        {
            httpd_resp_sendstr(req, "OK - Email sent!");
            ESP_LOGI(TAG, "=== SUCCESS ===");
        }
        else
        {
            httpd_resp_sendstr(req, "FAIL - Email error");
            ESP_LOGE(TAG, "=== EMAIL FAILED ===");
        }
    }
    else
    {
        httpd_resp_sendstr(req, "FAIL - Camera error");
        ESP_LOGE(TAG, "=== CAPTURE FAILED ===");
    }

    return ESP_OK;
}

// HTTP server URIs
static const httpd_uri_t uri_home = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = home_handler,
};

static const httpd_uri_t uri_stream = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
};

static const httpd_uri_t uri_capture = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
};

// Start HTTP server
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 8;
    config.max_resp_headers = 8;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_home);
        httpd_register_uri_handler(server, &uri_stream);
        httpd_register_uri_handler(server, &uri_capture);
        ESP_LOGI(TAG, "HTTP server started");
        return server;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
}

// ===== WIFI INIT =====
static void wifi_init_sta(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi started, connecting to %s...", WIFI_SSID);
}

// ===== HTTP NOTIFICATION =====
// HTTP notification được xử lý bởi http_notifier.c
// Không cần khởi tạo gì thêm, chỉ cần gọi send_http_notification()

// ===== MAIN APP =====
void app_main(void)
{
    ESP_LOGI(TAG, "\n=== ESP32-CAM Pure ESP-IDF Starting ===");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize networking
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize camera
    if (init_camera() != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera initialization failed");
        return;
    }

    // Initialize flash LED
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FLASH_GPIO_NUM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(FLASH_GPIO_NUM, 0);
    ESP_LOGI(TAG, "Flash LED initialized");

    // Initialize WiFi with hardcoded credentials
    wifi_init_sta();

    // Wait for WiFi connection
    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_LOGI(TAG, "WiFi connected! Ready to send HTTP notifications to ESP32-C3");
    ESP_LOGI(TAG, "ESP32-C3 should be at: %s:8080", ESP32C3_IP);

    // Initialize SNTP for email timestamp
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized");

    // Start web server
    start_webserver();

    ESP_LOGI(TAG, "=== System Ready ===");
    ESP_LOGI(TAG, "Open browser and go to ESP32-CAM IP address");
    ESP_LOGI(TAG, "========================");

    // Main loop - keep alive
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}