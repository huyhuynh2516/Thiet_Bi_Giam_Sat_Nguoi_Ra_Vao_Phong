/**
 * @file http_notifier.h
 * @brief HTTP notification client for ESP32-CAM to ESP32-C3 communication
 *
 * Thay thế ESP-NOW bằng HTTP POST request
 */

#ifndef HTTP_NOTIFIER_H
#define HTTP_NOTIFIER_H

#include "esp_err.h"

/**
 * @brief Gửi HTTP notification đến ESP32-C3
 *
 * @param esp32c3_ip Địa chỉ IP của ESP32-C3 (VD: "192.168.1.50")
 * @param photo_count Số lượng ảnh đã gửi
 * @param message Thông điệp (tùy chọn, có thể NULL)
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t send_http_notification(const char *esp32c3_ip, uint32_t photo_count, const char *message);

#endif // HTTP_NOTIFIER_H
