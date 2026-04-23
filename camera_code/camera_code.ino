#include <Arduino.h>
#include <esp_camera.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "board_config.h"

void startCameraServer();
void setupLedFlash();

// -------------------- WiFi / endpoints --------------------
const char *ssid       = "MAKERSPACE";
const char *password   = "12345678";
const char *serverUrl  = "http://192.168.0.160:6000/detect";
const char *resetUrl   = "http://192.168.0.160:6000/reset";
const char *readyUrl   = "http://192.168.0.160:6000/ready";

// -------------------- Scan interval --------------------
static const unsigned long SCAN_INTERVAL_MS  = 10000;
static const unsigned long READY_POLL_MS     = 3000;
static const unsigned long READY_TIMEOUT_MS  = 120000;  // 2 min max for motor

// -------------------- Game state --------------------
bool gameRunning = false;   // only true after "start" is typed and motor is ready

// -------------------- JSON / board helpers --------------------
// Extracts the 9 board characters ('.', 'X', 'O') from the server response.
// Server returns: {"board":[[".",".","."],[".",".","."],[".",".","."]],...}
bool parseBoardFromJSON(const String& json, char cells[9]) {
    int idx = 0;
    for (int i = 0; i + 2 < (int)json.length() && idx < 9; i++) {
        if (json[i] == '"') {
            char c = json[i + 1];
            if ((c == '.' || c == 'X' || c == 'O') && json[i + 2] == '"') {
                cells[idx++] = c;
                i += 2;
            }
        }
    }
    return idx == 9;
}

void printBoard(const char b[9]) {
    for (int r = 0; r < 3; r++) {
        Serial.printf(" %c | %c | %c\n", b[r*3], b[r*3+1], b[r*3+2]);
        if (r < 2) Serial.println("---+---+---");
    }
}

// -------------------- HTTP helpers --------------------
bool postReset() {
    HTTPClient http;
    http.begin(resetUrl);
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST("{}");
    http.end();
    return code == 200;
}

// Polls /ready until the motor signals it has finished drawing the grid.
// Returns true when ready, false on timeout.
bool waitForMotorReady() {
    unsigned long start = millis();
    while (millis() - start < READY_TIMEOUT_MS) {
        delay(READY_POLL_MS);
        HTTPClient http;
        http.begin(readyUrl);
        http.setTimeout(5000);
        int code = http.GET();
        if (code == 200) {
            String body = http.getString();
            http.end();
            if (body.indexOf("true") >= 0) {
                return true;
            }
        } else {
            http.end();
        }
        Serial.println("Waiting for motor to finish drawing grid...");
    }
    Serial.println("Timed out waiting for motor ready signal.");
    return false;
}

// -------------------- Capture and send --------------------
void captureAndSend() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        return;
    }

    HTTPClient http;
    http.begin(serverUrl);
    http.setTimeout(10000);
    http.addHeader("Content-Type", "image/jpeg");

    int httpCode = http.POST(fb->buf, fb->len);
    esp_camera_fb_return(fb);

    if (httpCode != 200) {
        Serial.printf("HTTP error: %d — %s\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return;
    }

    String response = http.getString();
    http.end();

    char cells[9];
    if (parseBoardFromJSON(response, cells)) {
        Serial.println("Board:");
        printBoard(cells);
    } else {
        Serial.println("Could not parse board from response");
        Serial.println(response);
    }
}

// -------------------- Setup --------------------
void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println();

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_QVGA;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 12;
    config.fb_count     = 1;

    if (psramFound()) {
        config.fb_count    = 2;
        config.grab_mode   = CAMERA_GRAB_LATEST;
        config.fb_location = CAMERA_FB_IN_PSRAM;
    } else {
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.frame_size  = FRAMESIZE_QVGA;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        while (true) delay(1000);
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 1);
        s->set_saturation(s, -1);
        s->set_framesize(s, FRAMESIZE_QVGA);
    }

#if defined(LED_GPIO_NUM)
    setupLedFlash();
#endif

    WiFi.begin(ssid, password);
    WiFi.setSleep(false);
    Serial.print("WiFi connecting");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\nConnected — IP: %s\n", WiFi.localIP().toString().c_str());

    startCameraServer();
    Serial.println("Ready. Type 'start' to begin a new game.");
}

// -------------------- Loop --------------------
void loop() {
    // Read serial commands
    static String serialBuf = "";
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            serialBuf.trim();
            serialBuf.toLowerCase();
            if (serialBuf == "start") {
                gameRunning = false;
                Serial.println("Resetting game state...");
                if (postReset()) {
                    Serial.println("Waiting for motor to redraw the grid (replace paper now)...");
                    if (waitForMotorReady()) {
                        gameRunning = true;
                        Serial.println("Grid drawn. Game started — draw an O to take your turn.");
                    }
                } else {
                    Serial.println("Could not reach Python server — is detect.py running?");
                }
            }
            serialBuf = "";
        } else {
            serialBuf += c;
        }
    }

    // Scan on interval only while a game is active
    if (gameRunning) {
        static unsigned long lastScan = 0;
        unsigned long now = millis();
        if (now - lastScan >= SCAN_INTERVAL_MS) {
            lastScan = now;
            Serial.println("Scanning...");
            captureAndSend();
        }
    }

    delay(20);
}
