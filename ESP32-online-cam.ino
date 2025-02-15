/*
    ==================================================================
    Программа выводит видеопоток камеры с помощью http сервера
    Так же есть возможно начать/остановить запись на SD карту
    Сохраняет кадры в папку с названием содержащим время начала записи
    Например: /2025-02-15_12-34-56/00001.jpg

    В веб странице есть кнопка которая на 3 секунды подает сигнал на реле
    ESP32 подключена к wifi, данные для подключения находятся ниже
    Прошивка делалась для ESP32-CAM с 4 МБ PSRAM и камерой OV2640
    Возможны ошибки, работа прошивки не проверялась!
    ==================================================================
*/

#include "Arduino.h"
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "soc/soc.h"          // для отключения brownout
#include "soc/rtc_cntl_reg.h" // для отключения brownout
#include "esp_http_server.h"
#include "SD_MMC.h" // для работы со встроенной SD-картой
#include "time.h"

// ===== Параметры WiFi =====
const char *ssid = "$SSID";
const char *password = "$PASSWORD";

// ===== Определения пинов камеры =====
#define PART_BOUNDARY "123456789000000000000987654321"

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// ===== Параметры стрима =====
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ===== Параметры реле =====
const int relayPin = 33;
const unsigned long relayDuration = 3000; // 3 секунды
bool relayActive = false;
unsigned long relayStartTime = 0;

// ===== Параметры записи =====
volatile bool recordingActive = false; // флаг записи
volatile uint32_t frameIndex = 0;      // счетчик кадров (для формирования имен файлов)
char folderName[64];

// ===== Дескриптор HTTP сервера =====
httpd_handle_t server_httpd = NULL;

// ===== Параметры NTP =====
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0; // для GMT+0
const int daylightOffset_sec = 0;  // если нет перехода на летнее время

// =======================================================================
// Обработчик MJPEG-стрима (URI: /stream)
// Для каждого кадра делается capture, кадр отправляется клиенту, а если запись включена – сохраняется на SD
// =======================================================================
static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK)
        return res;

    while (true)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        size_t jpg_buf_len = 0;
        uint8_t *jpg_buf = NULL;
        if (fb->format != PIXFORMAT_JPEG)
        {
            // Преобразуем кадр в JPEG
            bool jpeg_converted = frame2jpg(fb, 80, &jpg_buf, &jpg_buf_len);
            esp_camera_fb_return(fb);
            if (!jpeg_converted)
            {
                Serial.println("JPEG compression failed");
                res = ESP_FAIL;
            }
        }
        else
        {
            jpg_buf_len = fb->len;
            jpg_buf = fb->buf;
        }

        if (res != ESP_OK)
            break;

        // Если запись включена, сохраняем кадр как отдельный файл на SD-карте
        if (recordingActive)
        {
            char filePath[100];
            sprintf(filePath, "%s/%05d.jpg", folderName, frameIndex++);
            File file = SD_MMC.open(filePath, FILE_WRITE);
            if (file)
            {
                file.write(jpg_buf, jpg_buf_len);
                file.close();
                Serial.printf("Saved frame to %s\n", filePath);
            }
            else
            {
                Serial.println("Failed to open file for writing");
            }
        }

        // Формируем и отправляем заголовок для этого кадра
        char part_buf[64];
        size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, jpg_buf_len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK)
            break;
        res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
        if (res != ESP_OK)
            break;
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res != ESP_OK)
            break;

        // Если кадр был сконвертирован – освобождаем память
        if (fb->format != PIXFORMAT_JPEG)
        {
            free(jpg_buf);
        }
    }
    return res;
}

// =======================================================================
// Обработчик главной страницы (URI: /)
// Выводит HTML-страницу со стримом и кнопками управления
// =======================================================================
static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html = "<!DOCTYPE html><html>"
                       "<head><meta charset='utf-8'><title>ESP32-CAM Control</title></head>"
                       "<body>"
                       "<h1>ESP32-CAM Stream</h1>"
                       "<img src='/stream' style='width:640px;height:480px;'/><br><br>"
                       "<button onclick=\"fetch('/startRecord')\">Start Recording</button>"
                       "<button onclick=\"fetch('/stopRecord')\">Stop Recording</button>"
                       "<button onclick=\"fetch('/relayOn')\">Relay On</button>"
                       "</body></html>";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// =======================================================================
// Обработчик запуска записи (URI: /startRecord)
// =======================================================================
static esp_err_t start_record_handler(httpd_req_t *req)
{
    if (!recordingActive)
    {
        frameIndex = 0; // сбрасываем счётчик кадров
        recordingActive = true;
    }

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        Serial.println("Не удалось получить время");
    }
    // Формируем имя папки на основе текущего времени
    // Формат: /YYYY-MM-DD_HH-MM-SS

    strftime(folderName, sizeof(folderName), "/%Y-%m-%d_%H-%M-%S", &timeinfo);
    Serial.print("Создаётся папка: ");
    Serial.println(folderName);

    // Создаём папку на SD-карте
    if (SD_MMC.mkdir(folderName))
    {
        Serial.println("Папка успешно создана");
    }
    else
    {
        Serial.println("Не удалось создать папку");
    }

    httpd_resp_send(req, "Recording started", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// =======================================================================
// Обработчик остановки записи (URI: /stopRecord)
// =======================================================================
static esp_err_t stop_record_handler(httpd_req_t *req)
{
    recordingActive = false;
    httpd_resp_send(req, "Recording stopped", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// =======================================================================
// Обработчик включения реле (URI: /relayOn)
// =======================================================================
static esp_err_t relay_on_handler(httpd_req_t *req)
{
    digitalWrite(relayPin, HIGH);
    relayActive = true;
    relayStartTime = millis();
    httpd_resp_send(req, "Relay activated for 3 seconds", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// =======================================================================
// Функция регистрации всех URI и запуск HTTP-сервера
// =======================================================================
void startServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    // Запускаем сервер
    if (httpd_start(&server_httpd, &config) == ESP_OK)
    {
        // Регистрируем обработчик главной страницы
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server_httpd, &index_uri);

        // Регистрируем обработчик стрима
        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server_httpd, &stream_uri);

        // Регистрируем обработчики записи и реле
        httpd_uri_t start_record_uri = {
            .uri = "/startRecord",
            .method = HTTP_GET,
            .handler = start_record_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server_httpd, &start_record_uri);

        httpd_uri_t stop_record_uri = {
            .uri = "/stopRecord",
            .method = HTTP_GET,
            .handler = stop_record_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server_httpd, &stop_record_uri);

        httpd_uri_t relay_uri = {
            .uri = "/relayOn",
            .method = HTTP_GET,
            .handler = relay_on_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server_httpd, &relay_uri);
    }
}

// =======================================================================
// setup()
// =======================================================================
void setup()
{
    // Отключаем brownout (если требуется)
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);

    // Инициализируем пин реле
    pinMode(relayPin, OUTPUT);
    digitalWrite(relayPin, LOW);

    // ===== Инициализация камеры =====
    camera_config_t cam_config;
    cam_config.ledc_channel = LEDC_CHANNEL_0;
    cam_config.ledc_timer = LEDC_TIMER_0;
    cam_config.pin_d0 = Y2_GPIO_NUM;
    cam_config.pin_d1 = Y3_GPIO_NUM;
    cam_config.pin_d2 = Y4_GPIO_NUM;
    cam_config.pin_d3 = Y5_GPIO_NUM;
    cam_config.pin_d4 = Y6_GPIO_NUM;
    cam_config.pin_d5 = Y7_GPIO_NUM;
    cam_config.pin_d6 = Y8_GPIO_NUM;
    cam_config.pin_d7 = Y9_GPIO_NUM;
    cam_config.pin_xclk = XCLK_GPIO_NUM;
    cam_config.pin_pclk = PCLK_GPIO_NUM;
    cam_config.pin_vsync = VSYNC_GPIO_NUM;
    cam_config.pin_href = HREF_GPIO_NUM;
    cam_config.pin_sscb_sda = SIOD_GPIO_NUM;
    cam_config.pin_sscb_scl = SIOC_GPIO_NUM;
    cam_config.pin_pwdn = PWDN_GPIO_NUM;
    cam_config.pin_reset = RESET_GPIO_NUM;
    cam_config.xclk_freq_hz = 20000000;
    cam_config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound())
    {
        Serial.print("PSRAM найдено и используется\n");
        cam_config.frame_size = FRAMESIZE_UXGA;
        cam_config.jpeg_quality = 10;
        cam_config.fb_count = 2;
    }
    else
    {
        Serial.print("PSRAM не найдено!\n");
        cam_config.frame_size = FRAMESIZE_SVGA;
        cam_config.jpeg_quality = 12;
        cam_config.fb_count = 1;
    }

    esp_err_t err = esp_camera_init(&cam_config);
    if (err != ESP_OK)
    {
        Serial.printf("Camera init failed with error 0x%x", err);
        return;
    }

    // ===== Подключение к WiFi =====
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    Serial.print("Access the stream at: http://");
    Serial.println(WiFi.localIP());

    // ===== Инициализация SD-карты =====
    if (!SD_MMC.begin())
    {
        Serial.println("SD Card Mount Failed");
    }
    else
    {
        Serial.println("SD Card mounted");
    }

    // Настраиваем получение времени по NTP
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        Serial.println("Не удалось получить время");
        return;
    }

    // ===== Запуск HTTP-сервера =====
    startServer();
}

// =======================================================================
// loop()
// =======================================================================
void loop()
{
    // Неблокирующая проверка работы реле: если прошло 3 сек – отключаем реле
    if (relayActive && (millis() - relayStartTime >= relayDuration))
    {
        digitalWrite(relayPin, LOW);
        relayActive = false;
        Serial.println("Relay deactivated");
    }
    delay(1);
}