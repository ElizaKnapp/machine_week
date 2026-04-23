#include <Arduino.h>
#include <esp_camera.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "board_config.h"

void startCameraServer();
void setupLedFlash();

// -------------------- WiFi --------------------
const char *ssid = "MAKERSPACE";
const char *password = "12345678";

// Replace with your computer's local IP address
const char *serverUrl = "http://192.168.0.160:6000/detect";

// -------------------- Motor UART --------------------
// AI Thinker free pins (not used by OV2640 or PSRAM):
//   GPIO 14 → motor board RX
//   GPIO 13 ← motor board TX
// Adjust if your wiring differs.
#define MOTOR_TX_PIN 14
#define MOTOR_RX_PIN 13
#define MOTOR_BAUD   9600

HardwareSerial MotorSerial(2);  // UART2

// -------------------- Game state --------------------
char board[9];      // '.', 'X', 'O'; index 0–8 = positions 1–9 (row-major)
bool gameActive = false;
const char ROBOT_MARK = 'O';
const char HUMAN_MARK = 'X';

// -------------------- Board helpers --------------------
void clearBoard(char b[9])             { for (int i = 0; i < 9; i++) b[i] = '.'; }
void copyBoard(const char s[9], char d[9]) { for (int i = 0; i < 9; i++) d[i] = s[i]; }

void printBoard(const char b[9]) {
    for (int r = 0; r < 3; r++) {
        Serial.printf(" %c | %c | %c\n", b[r*3], b[r*3+1], b[r*3+2]);
        if (r < 2) Serial.println("---+---+---");
    }
}

// -------------------- Game logic --------------------
static const int WIN_LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8},   // rows
    {0,3,6},{1,4,7},{2,5,8},   // cols
    {0,4,8},{2,4,6}            // diagonals
};

char checkWinner(const char b[9]) {
    for (const auto& w : WIN_LINES) {
        if (b[w[0]] != '.' && b[w[0]] == b[w[1]] && b[w[1]] == b[w[2]])
            return b[w[0]];
    }
    return '.';
}

bool isFull(const char b[9]) {
    for (int i = 0; i < 9; i++) if (b[i] == '.') return false;
    return true;
}

int minimax(char b[9], bool robotTurn, int depth) {
    char w = checkWinner(b);
    if (w == ROBOT_MARK) return 10 - depth;
    if (w == HUMAN_MARK) return depth - 10;
    if (isFull(b))       return 0;

    int best = robotTurn ? -100 : 100;
    for (int i = 0; i < 9; i++) {
        if (b[i] != '.') continue;
        b[i] = robotTurn ? ROBOT_MARK : HUMAN_MARK;
        int score = minimax(b, !robotTurn, depth + 1);
        b[i] = '.';
        if (robotTurn) { if (score > best) best = score; }
        else           { if (score < best) best = score; }
    }
    return best;
}

// Returns 0-indexed cell index for robot's best move, -1 if board is full.
int chooseMove(char b[9]) {
    int best = -1, bestScore = -100;
    for (int i = 0; i < 9; i++) {
        if (b[i] != '.') continue;
        b[i] = ROBOT_MARK;
        int score = minimax(b, false, 0);
        b[i] = '.';
        if (score > bestScore) { bestScore = score; best = i; }
    }
    return best;
}

// -------------------- Motor communication --------------------
bool sendMotorCommand(const String& cmd) {
    Serial.printf("[uart] -> %s\n", cmd.c_str());
    MotorSerial.println(cmd);

    unsigned long start = millis();
    while (millis() - start < 15000) {
        if (MotorSerial.available()) {
            String reply = MotorSerial.readStringUntil('\n');
            reply.trim();
            Serial.printf("[uart] <- %s\n", reply.c_str());
            return reply == "OK" || reply == "DONE";
        }
        delay(20);
    }
    Serial.println("[uart] timeout — no reply from motor board");
    return false;
}

// -------------------- Image capture and board detection --------------------
// Extracts all single-char JSON string values that are '.', 'X', or 'O'.
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

bool captureAndDetect(char cells[9]) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        return false;
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
        return false;
    }

    String response = http.getString();
    http.end();
    Serial.println(response);

    if (!parseBoardFromJSON(response, cells)) {
        Serial.println("Could not parse 9 cells from response");
        return false;
    }
    return true;
}

// -------------------- Game flow --------------------
void startGame() {
    clearBoard(board);
    gameActive = true;

    Serial.println("Homing and drawing grid...");
    if (!sendMotorCommand("HOME")) { Serial.println("HOME failed"); return; }
    if (!sendMotorCommand("GRID")) { Serial.println("GRID failed"); return; }

    Serial.println("Game started. Human = X, Robot = O.");
    Serial.println("Make your move, then type 'scan'.");
}

void playOneTurn() {
    char current[9];
    if (!captureAndDetect(current)) {
        Serial.println("Detection failed — try 'scan' again.");
        return;
    }

    // Validate: exactly one new human mark; no robot marks removed.
    int newHuman = 0, lostRobot = 0;
    for (int i = 0; i < 9; i++) {
        if (current[i] == HUMAN_MARK && board[i] != HUMAN_MARK) newHuman++;
        if (board[i] == ROBOT_MARK && current[i] != ROBOT_MARK) lostRobot++;
    }
    if (lostRobot > 0) {
        Serial.println("Vision error: robot mark disappeared. Scan again.");
        return;
    }
    if (newHuman != 1) {
        Serial.printf("Expected 1 new human mark, got %d. Scan again.\n", newHuman);
        return;
    }

    copyBoard(current, board);
    Serial.println("After your move:");
    printBoard(board);

    if (checkWinner(board) == HUMAN_MARK) { Serial.println("Human wins!"); gameActive = false; return; }
    if (isFull(board))                    { Serial.println("Draw!");        gameActive = false; return; }

    int move = chooseMove(board);
    if (move < 0) { Serial.println("No moves left."); gameActive = false; return; }

    board[move] = ROBOT_MARK;
    String cmd = String(ROBOT_MARK) + String(move + 1);  // e.g. "O5"
    Serial.printf("Robot plays %s\n", cmd.c_str());
    sendMotorCommand(cmd);

    Serial.println("After robot move:");
    printBoard(board);

    if (checkWinner(board) == ROBOT_MARK) { Serial.println("Robot wins!"); gameActive = false; return; }
    if (isFull(board))                    { Serial.println("Draw!");       gameActive = false; return; }

    Serial.println("Your turn — make your move then type 'scan'.");
}

// -------------------- Serial command handling --------------------
String readCommand() {
    if (!Serial.available()) return "";
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    return cmd;
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

    MotorSerial.begin(MOTOR_BAUD, SERIAL_8N1, MOTOR_RX_PIN, MOTOR_TX_PIN);
    Serial.println("Motor UART initialised.");

    WiFi.begin(ssid, password);
    WiFi.setSleep(false);
    Serial.print("WiFi connecting");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\nConnected — IP: %s\n", WiFi.localIP().toString().c_str());

    startCameraServer();

    Serial.println("\nCommands:");
    Serial.println("  start  — home, draw grid, begin game");
    Serial.println("  scan   — detect board, robot moves");
    Serial.println("  go     — detect board only (debug)");
    Serial.println("  test   — print WiFi/server status");
}

// -------------------- Loop --------------------
void loop() {
    String cmd = readCommand();
    if (cmd.length() == 0) { delay(20); return; }

    if (cmd == "start") {
        startGame();
    } else if (cmd == "scan") {
        if (!gameActive) Serial.println("No active game. Type 'start' first.");
        else playOneTurn();
    } else if (cmd == "go") {
        char cells[9];
        if (captureAndDetect(cells)) { Serial.println("Detected:"); printBoard(cells); }
    } else if (cmd == "test") {
        Serial.printf("WiFi: %s\nServer: %s\n",
            WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
            serverUrl);
    } else {
        Serial.printf("Unknown command: %s\n", cmd.c_str());
        Serial.println("Commands: start, scan, go, test");
    }

    delay(20);
}
