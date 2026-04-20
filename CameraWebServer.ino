#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

#include "board_config.h"

// Already implemented in app_httpd.cpp
void startCameraServer();
void setupLedFlash();

// -------------------- WiFi --------------------
const char *ssid = "MAKERSPACE";
const char *password = "12345678";

// -------------------- Python server --------------------
// Replace this with your computer's local IP address
// Example: http://192.168.1.123:5000/detect
const char *serverUrl = "http://192.168.0.163:6000/detect";
// -------------------- Serial command handling --------------------
String readCommand() {
  if (!Serial.available()) return "";

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();
  return cmd;
}

// -------------------- Send image to Python server --------------------
void sendImageToServer() {
  Serial.println("Capturing image...");

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  Serial.print("Captured ");
  Serial.print(fb->len);
  Serial.println(" bytes");

  HTTPClient http;
  http.begin(serverUrl);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "image/jpeg");

  Serial.println("Sending image to Python server...");
  int httpCode = http.POST(fb->buf, fb->len);

  if (httpCode > 0) {
    String response = http.getString();

    Serial.print("HTTP status: ");
    Serial.println(httpCode);

    Serial.println("Server response:");
    Serial.println(response);
  } else {
    Serial.print("POST failed, error: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  esp_camera_fb_return(fb);
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  // Use JPEG because we are sending the image to Python
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;   // 320x240
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  Serial.print("Hello");
  Serial.print(psramFound());

  if (psramFound()) {
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.frame_size = FRAMESIZE_QVGA;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    while (true) {
      delay(1000);
    }
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 1);
    s->set_saturation(s, -1);

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
    s->set_vflip(s, 1);
#endif

    // Smaller preview size for faster web streaming startup if JPEG
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("ESP32 IP: http://");
  Serial.println(WiFi.localIP());

  startCameraServer();

  Serial.println();
  Serial.println("Website is running.");
  Serial.println("Open the ESP32 IP above in your browser to view the camera.");
  Serial.println("Type 'go' in Serial Monitor to capture and send one image to Python.");
  Serial.println("Type 'test' to check if WiFi is still connected.");
}

// -------------------- Loop --------------------
void loop() {
  String cmd = readCommand();
  if (cmd.length() == 0) {
    delay(20);
    return;
  }

  if (cmd == "go") {
    sendImageToServer();
  } else if (cmd == "test") {
    Serial.print("WiFi status: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "connected" : "not connected");
    Serial.print("Server URL: ");
    Serial.println(serverUrl);
  } else {
    Serial.print("Unknown command: ");
    Serial.println(cmd);
    Serial.println("Available commands:");
    Serial.println("  go   - capture image and send to Python server");
    Serial.println("  test - print connection info");
  }

  delay(20);
}