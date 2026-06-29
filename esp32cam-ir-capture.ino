#include <Arduino.h>
#include <HTTPClient.h>
#include <IRremote.hpp>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_camera.h>
#include <esp_arduino_version.h>

const char *WIFI_SSID = "Garsa_Phone";
const char *WIFI_PASSWORD = "testedaesp";

const char *API_CAPTURE_URL = "https://image-processing-server-eight.vercel.app/captures";
const char *DEVICE_ID = "esp32cam-01";

constexpr uint16_t IR_RECEIVE_PIN = 13;

// 1=0x45 2=0x46 3=0x47 4=0x44 5=0x40 6=0x43 7=0x07 8=0x15 9=0x09
// 0=0x19 *=0x16 #=0x0D Up=0x18 Down=0x52 Right=0x5A Left=0x08

constexpr uint8_t OK_IR_COMMAND = 0x1C;

// false: OK starts captures once. true: OK toggles start/stop.
constexpr bool TOGGLE_CAPTURE_ON_OK = true;
constexpr uint32_t CAPTURE_INTERVAL_MS = 2000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
// =======================

// AI Thinker ESP32-CAM pin map.
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

hw_timer_t *captureTimer = nullptr;
portMUX_TYPE captureTimerMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool captureDue = false;
bool captureEnabled = false;
String lastTriggerCommand = "ir-ok";


void IRAM_ATTR onCaptureTimer() {
  portENTER_CRITICAL_ISR(&captureTimerMux);
  captureDue = true;
  portEXIT_CRITICAL_ISR(&captureTimerMux);
}

bool popCaptureDue() {
  portENTER_CRITICAL(&captureTimerMux);
  const bool due = captureDue;
  captureDue = false;
  portEXIT_CRITICAL(&captureTimerMux);
  return due;
}

void configureCaptureTimer() {
  captureTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(captureTimer, &onCaptureTimer, true);
  timerAlarmWrite(captureTimer, static_cast<uint64_t>(CAPTURE_INTERVAL_MS) * 1000ULL, true);
}

void startCaptureTimer() {
  if (!captureTimer) {
    return;
  }

  portENTER_CRITICAL(&captureTimerMux);
  captureDue = true;
  portEXIT_CRITICAL(&captureTimerMux);

  timerWrite(captureTimer, 0);
  timerAlarmEnable(captureTimer);
}

void stopCaptureTimer() {
  if (!captureTimer) {
    return;
  }

  timerAlarmDisable(captureTimer);

  portENTER_CRITICAL(&captureTimerMux);
  captureDue = false;
  portEXIT_CRITICAL(&captureTimerMux);
}

bool configureCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_CIF;
    config.jpeg_quality = 14;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  const esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", error);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_framesize(sensor, psramFound() ? FRAMESIZE_SVGA : FRAMESIZE_CIF);
  }

  return true;
}

bool ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection failed");
    return false;
  }

  Serial.print("Wi-Fi connected. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool beginHttp(HTTPClient &http, WiFiClient &plainClient, WiFiClientSecure &secureClient) {
  const String url(API_CAPTURE_URL);

  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    return http.begin(secureClient, url);
  }

  return http.begin(plainClient, url);
}

bool uploadFrame(camera_fb_t *frame) {
  if (!ensureWifiConnected()) {
    return false;
  }

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;

  if (!beginHttp(http, plainClient, secureClient)) {
    Serial.println("HTTP begin failed");
    return false;
  }

  http.setTimeout(20000);
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-capture-source", "esp32-cam-ir");
  http.addHeader("x-trigger-command", lastTriggerCommand);

  Serial.printf("Uploading JPEG: %u bytes\n", frame->len);
  const int statusCode = http.POST(frame->buf, frame->len);
  const String responseBody = http.getString();
  http.end();

  if (statusCode < 200 || statusCode >= 300) {
    Serial.printf("Upload failed. HTTP %d\n", statusCode);
    if (responseBody.length() > 0) {
      Serial.println(responseBody);
    }
    return false;
  }

  Serial.printf("Upload OK. HTTP %d\n", statusCode);
  if (responseBody.length() > 0) {
    Serial.println(responseBody);
  }
  return true;
}

void captureAndUpload() {
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    Serial.println("Camera capture failed");
    return;
  }

  if (frame->format != PIXFORMAT_JPEG) {
    Serial.println("Captured frame is not JPEG");
    esp_camera_fb_return(frame);
    return;
  }

  uploadFrame(frame);
  esp_camera_fb_return(frame);
}

String formatIrCommand(uint32_t value) {
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "0x%X", static_cast<unsigned int>(value));
  return String(buffer);
}

bool isOkIrCommand(uint8_t command) {
  if (OK_IR_COMMAND == 0x0) {
    return true;
  }

  return command == OK_IR_COMMAND;
}

void handleIrReceiver() {
  if (!IrReceiver.decode()) {
    return;
  }

  if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
    IrReceiver.resume();
    return;
  }

  const uint8_t command = IrReceiver.decodedIRData.command;
  const String commandText = formatIrCommand(command);

  Serial.print("IR received: ");
  Serial.print(commandText);
  Serial.print(" raw=");
  Serial.print(formatIrCommand(IrReceiver.decodedIRData.decodedRawData));
  Serial.print(" protocol=");
  Serial.println(getProtocolString(IrReceiver.decodedIRData.protocol));

  if (isOkIrCommand(command)) {
    lastTriggerCommand = commandText;

    if (TOGGLE_CAPTURE_ON_OK && captureEnabled) {
      captureEnabled = false;
      stopCaptureTimer();
      Serial.println("Capture timer stopped by OK button");
    } else if (!captureEnabled) {
      captureEnabled = true;
      startCaptureTimer();
      Serial.println("Capture timer started by OK button");
    }
  }

  IrReceiver.resume();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-CAM IR capture client starting");

  if (!configureCamera()) {
    Serial.println("Stopping because camera configuration failed");
    return;
  }

  ensureWifiConnected();
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  configureCaptureTimer();

  Serial.printf("IR receiver ready on GPIO %u\n", IR_RECEIVE_PIN);
  Serial.println("Press OK on the IR remote to start captures");
}

void loop() {
  handleIrReceiver();

  if (captureEnabled && popCaptureDue()) {
    captureAndUpload();
  }

  delay(5);
}
