#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <nvs_flash.h>
#include <esp_camera.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"

#define APP_CPU        1
#define PRO_CPU        0

#define FPS            14
#define STREAM_PERIOD  (1000 / FPS)

#define MJPEG_BOUNDARY "--123456789000000000000987654321\r\n"
#define MJPEG_CONTENT_TYPE "Content-Type: image/jpeg\r\nContent-Length: "

static const char *TAG = "cam_streamer";

static const char *MJPEG_HEADER =
    "HTTP/1.1 200 OK\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=123456789000000000000987654321\r\n\r\n";

static volatile uint8_t *camBuf = NULL;
static volatile size_t camSize = 0;
static size_t camBufCap = 0;

static SemaphoreHandle_t frame_ready_sem = NULL;
static SemaphoreHandle_t frame_lock_sem = NULL;

static httpd_handle_t g_server = NULL;

// ---------------------------
// Camera board config
// ---------------------------

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define CAMERA_MODEL_AI_THINKER

static void init_camera(void)
{
    esp_err_t err;

    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA, // use FRAMESIZE_QVGA/SVGA if needed
        .jpeg_quality = 12,
        .fb_count = 2
    };

    err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        esp_restart();
    }
}

// ---------------------------
// WiFi setup
// ---------------------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}



// ---------------------------
// Buffer management
// ---------------------------

static void ensure_frame_buffer(size_t required_size)
{
    if (required_size <= camBufCap) {
        return;
    }

    size_t new_cap = required_size + (required_size / 4);
    void *new_buf = NULL;

    // Prefer PSRAM if available
    new_buf = heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_buf) {
        new_buf = heap_caps_malloc(new_cap, MALLOC_CAP_DEFAULT);
    }
    if (!new_buf) {
        ESP_LOGE(TAG, "Out of memory!");
        esp_restart();
    }

    if (camBuf) {
        heap_caps_free(camBuf);
    }

    camBuf = (uint8_t *)new_buf;
    camBufCap = new_cap;
}

static void capture_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(STREAM_PERIOD);

    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t s = fb->len;
        ensure_frame_buffer(s);

        // Copy frame into RAM
        memcpy(camBuf, fb->buf, s);
        camSize = s;

        esp_camera_fb_return(fb);

        xSemaphoreGive(frame_ready_sem);

        vTaskDelayUntil(&last_wake, period);
    }
}

// ---------------------------
// HTTP handlers
// ---------------------------

static esp_err_t jpg_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Failed to capture JPG");
        return httpd_resp_send_500(req);
    }

    esp_err_t err = httpd_resp_set_type(req, "image/jpeg");
    if (err != ESP_OK) {
        esp_camera_fb_return(fb);
        return err;
    }

    err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return err;
}

static esp_err_t mjpeg_handler(httpd_req_t *req)
{
    // Send initial headers
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=123456789000000000000987654321");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    for (;;) {
        // Wait for next frame
        xSemaphoreTake(frame_ready_sem, portMAX_DELAY);

        // Abort if client disconnects (best-effort)
        if (req == NULL) {
            break;
        }

        char header[128];
        int hdr_len = snprintf(header, sizeof(header),
                               "Content-Type: image/jpeg\r\n"
                               "Content-Length: %zu\r\n\r\n",
                               camSize);

        // Send frame header and payload
        httpd_resp_send_chunk(req, header, hdr_len);
        httpd_resp_send_chunk(req, (const char *)camBuf, camSize);
        httpd_resp_send_chunk(req, "\r\n--123456789000000000000987654321\r\n", strlen("\r\n--123456789000000000000987654321\r\n"));

        // Let the client/connection breathe
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_OK;
}

// ---------------------------
// HTTP server
// ---------------------------

static httpd_uri_t uri_mjpeg = {
    .uri      = "/mjpeg/1",
    .method   = HTTP_GET,
    .handler  = mjpeg_handler,
    .user_ctx = NULL
};

static httpd_uri_t uri_jpg = {
    .uri      = "/jpg",
    .method   = HTTP_GET,
    .handler  = jpg_handler,
    .user_ctx = NULL
};

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 4447;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    if (httpd_start(&g_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    httpd_register_uri_handler(g_server, &uri_mjpeg);
    httpd_register_uri_handler(g_server, &uri_jpg);

    return ESP_OK;
}

// ---------------------------
// Periodic test task
// ---------------------------

static void periodic_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        ESP_LOGI(TAG, "Periodic task tick...");
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10000));
    }
}

// ---------------------------
// app_main
// ---------------------------

void app_main(void)
{


    init_camera();
    init_wifi();

    frame_ready_sem = xSemaphoreCreateBinary();
    frame_lock_sem = xSemaphoreCreateMutex();

    // Start capture task
    xTaskCreatePinnedToCore(capture_task,
                            "cam_task",
                            8192,
                            NULL,
                            2,
                            NULL,
                            APP_CPU);

    // Start periodic DNS/update-like task (placeholder)
    xTaskCreatePinnedToCore(periodic_task,
                            "periodic_task",
                            4096,
                            NULL,
                            2,
                            NULL,
                            APP_CPU);

    // Start web server
    ESP_ERROR_CHECK(start_http_server());

    ESP_LOGI(TAG, "HTTP streaming server ready");
    ESP_LOGI(TAG, "Stream URL: http://<IP>:4447/mjpeg/1");
    ESP_LOGI(TAG, "Single JPEG URL: http://<IP>:4447/jpg");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
