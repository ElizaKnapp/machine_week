#include <AccelStepper.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>

// -------------------- WiFi / endpoints --------------------
const char *ssid        = "MAKERSPACE";
const char *password    = "12345678";
const char *getMoveUrl  = "http://192.168.0.163:6000/get-move";
const char *readyUrl    = "http://192.168.0.163:6000/ready";

// -------------------- Stepper / servo pins --------------------
const int X_STEP_PIN = D1;
const int X_DIR_PIN  = D5;
const int Y_STEP_PIN = D3;
const int Y_DIR_PIN  = D2;
const int SERVO_PIN  = D4;

AccelStepper stepperX(AccelStepper::DRIVER, X_STEP_PIN, X_DIR_PIN);
AccelStepper stepperY(AccelStepper::DRIVER, Y_STEP_PIN, Y_DIR_PIN);
Servo penServo;

// -------------------- Motion constants --------------------
const float maxSpeed    = 600.0;
const float accel       = 300.0;
const int   penUpPos    = 20;
const int   penDownPos  = 90;

// Coordinate system: both stepper axes are physically inverted relative to
// the command sign, so command (+x, +y) moves the pen into physical
// quadrant 3 (down-left). To draw in physical quadrant 1 (up-right from
// home), all commanded coordinates are negative.
// yDownSign = +1 so row offsets ADD to boardOriginY, making higher-numbered
// rows less negative (physically lower, closer to home).
const int   yDownSign   = 1;

// Grid layout. Commanded coordinates are negative so the physical drawing
// lands in the first quadrant (up and right of home).
// Left edge  = boardOriginX                            (most negative X)
// Right edge = boardOriginX + 3*cellSize               (closest to 0)
// Top edge   = boardOriginY                            (most negative Y)
// Bottom edge = boardOriginY + 3*cellSize              (closest to 0)
const long boardOriginX = -5000;
const long boardOriginY = -5000;
const long cellSize     =  1500;
const long inset        =   220;
const long jogStep      =   500;

// Park position — large positive command pushes the arm well below-left of
// home and completely out of the camera's field of view.
const long parkX = 500;
const long parkY = 500;

// -------------------- Game state --------------------
int lastMove = 0;   // last move drawn (0 = none / game over)

// -------------------- Poll interval --------------------
static const unsigned long POLL_INTERVAL_MS = 2000;

// -------------------- Motion helpers --------------------
void penUp()   { penServo.write(penUpPos);  delay(200); }
void penDown() { penServo.write(penDownPos); delay(200); }

void moveTo(long x, long y) {
    stepperX.moveTo(x);
    stepperY.moveTo(y);
    while (stepperX.distanceToGo() != 0 || stepperY.distanceToGo() != 0) {
        stepperX.run();
        stepperY.run();
    }
}

void lineTo(long x1, long y1, long x2, long y2) {
    penUp();
    moveTo(x1, y1);
    penDown();
    moveTo(x2, y2);
    penUp();
}

// Move the arm to the park position so it is fully clear of the camera frame.
void parkPen() {
    penUp();
    moveTo(parkX, parkY);
}

long boardY(long offset) {
    return boardOriginY + yDownSign * offset;
}

void cellBounds(int pos, long &left, long &top, long &right, long &bottom) {
    int row = (pos - 1) / 3;
    int col = (pos - 1) % 3;

    left  = boardOriginX + col * cellSize;
    right = left + cellSize;

    long yTop    = boardY(row * cellSize);
    long yBottom = boardY((row + 1) * cellSize);

    if (yTop < yBottom) { top = yTop; bottom = yBottom; }
    else                 { top = yBottom; bottom = yTop; }
}

// -------------------- Drawing --------------------
void drawGrid() {
    long left   = boardOriginX;
    long right  = boardOriginX + 3 * cellSize;
    long y1     = boardY(cellSize);
    long y2     = boardY(2 * cellSize);
    long top    = boardY(0);
    long bottom = boardY(3 * cellSize);

    lineTo(left, y1, right, y1);
    lineTo(left, y2, right, y2);
    lineTo(boardOriginX + cellSize,     top, boardOriginX + cellSize,     bottom);
    lineTo(boardOriginX + 2 * cellSize, top, boardOriginX + 2 * cellSize, bottom);
    parkPen();
}

void drawX(int pos) {
    long left, top, right, bottom;
    cellBounds(pos, left, top, right, bottom);
    lineTo(left  + inset, top    + inset, right - inset, bottom - inset);
    lineTo(right - inset, top    + inset, left  + inset, bottom - inset);
    parkPen();
}

void drawO(int pos) {
    long left, top, right, bottom;
    cellBounds(pos, left, top, right, bottom);

    long cx = (left + right) / 2;
    long cy = (top  + bottom) / 2;
    long r  = cellSize / 2 - inset;
    long d  = r * 7 / 10;

    penUp();
    moveTo(cx,     cy - r);
    penDown();
    moveTo(cx + d, cy - d);
    moveTo(cx + r, cy);
    moveTo(cx + d, cy + d);
    moveTo(cx,     cy + r);
    moveTo(cx - d, cy + d);
    moveTo(cx - r, cy);
    moveTo(cx - d, cy - d);
    moveTo(cx,     cy - r);
    parkPen();
}

// -------------------- USB serial bench-test commands --------------------
void handleSerialCommand(const String& cmd) {
    if (cmd.equalsIgnoreCase("HOME")) {
        penUp();
        moveTo(0, 0);
        Serial.println("DONE");
    } else if (cmd.equalsIgnoreCase("H")) {
        stepperX.setCurrentPosition(0);
        stepperY.setCurrentPosition(0);
        Serial.println("Home set");
    } else if (cmd.equalsIgnoreCase("GRID")) {
        drawGrid();
        Serial.println("DONE");
    } else if (cmd.equalsIgnoreCase("U")) {
        penUp();
    } else if (cmd.equalsIgnoreCase("D")) {
        penDown();
    } else if (cmd.equalsIgnoreCase("XR")) {
        moveTo(stepperX.currentPosition() + jogStep, stepperY.currentPosition());
    } else if (cmd.equalsIgnoreCase("XL")) {
        moveTo(stepperX.currentPosition() - jogStep, stepperY.currentPosition());
    } else if (cmd.equalsIgnoreCase("YU")) {
        moveTo(stepperX.currentPosition(), stepperY.currentPosition() - jogStep);
    } else if (cmd.equalsIgnoreCase("YD")) {
        moveTo(stepperX.currentPosition(), stepperY.currentPosition() + jogStep);
    } else if (cmd.length() == 2 && (cmd[0]=='X'||cmd[0]=='x') && cmd[1]>='1' && cmd[1]<='9') {
        drawX(cmd[1] - '0');
        Serial.println("DONE");
    } else if (cmd.length() == 2 && (cmd[0]=='O'||cmd[0]=='o') && cmd[1]>='1' && cmd[1]<='9') {
        drawO(cmd[1] - '0');
        Serial.println("DONE");
    } else {
        Serial.println("ERR — unknown command: " + cmd);
    }
}

// -------------------- HTTP helpers --------------------
void postReady() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.begin(readyUrl);
    http.addHeader("Content-Type", "application/json");
    http.POST("{}");
    http.end();
    Serial.println("[http] Posted /ready");
}

// Returns the move number from {"move": N}.
// Returns -1 when the server sends the reset signal.
// Returns -2 on HTTP/WiFi error (caller should skip the cycle).
int fetchMove() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] not connected");
        return -2;
    }

    HTTPClient http;
    http.begin(getMoveUrl);
    http.setTimeout(5000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[http] GET /get-move failed: %d\n", code);
        http.end();
        return -2;
    }

    String body = http.getString();
    http.end();

    // Parse {"move": N}
    int idx = body.indexOf("\"move\"");
    if (idx < 0) return -1;
    idx = body.indexOf(':', idx);
    if (idx < 0) return -1;
    return body.substring(idx + 1).toInt();
}

// -------------------- Setup --------------------
void setup() {
    Serial.begin(115200);
    delay(300);

    stepperX.setMaxSpeed(maxSpeed);
    stepperX.setAcceleration(accel);
    stepperX.setMinPulseWidth(20);

    stepperY.setMaxSpeed(maxSpeed);
    stepperY.setAcceleration(accel);
    stepperY.setMinPulseWidth(20);

    penServo.setPeriodHertz(50);
    penServo.attach(SERVO_PIN, 500, 2400);
    penUp();

    stepperX.setCurrentPosition(0);
    stepperY.setCurrentPosition(0);

    WiFi.begin(ssid, password);
    Serial.print("WiFi connecting");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\nConnected — IP: %s\n", WiFi.localIP().toString().c_str());

    Serial.println("Ready. Polling /get-move every 2 seconds. Type 'start' on the camera board to begin.");
    Serial.println("USB commands: GRID, X1-X9, O1-O9, HOME, H, U, D, XR, XL, YU, YD");
}

// -------------------- Loop --------------------
void loop() {
    // Handle USB serial bench-test commands
    static String input = "";
    while (Serial.available() > 0) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (input.length() > 0) { input.trim(); handleSerialCommand(input); input = ""; }
        } else {
            input += c;
        }
        Serial.println(input);
    }

    // Poll /get-move on interval
    static unsigned long lastPoll = 0;
    unsigned long now = millis();
    if (now - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = now;

        int move = fetchMove();
        if (move == -2) {
            // HTTP/WiFi error — skip this cycle
        } else if (move == -1) {
            // Reset signal — re-home and redraw grid for a new game
            Serial.println("Reset signal received — re-homing and redrawing grid.");
            penUp();
            moveTo(0, 0);
            drawGrid();
            lastMove = 0;
            postReady();    // tell the camera board it can start scanning
        } else if (move == 0) {
            // No pending move (game not started or game over) — idle
            if (lastMove != 0) {
                Serial.println("Game over — state reset.");
                lastMove = 0;
            }
        } else if (move != lastMove) {
            // New move — draw X in the indicated cell
            Serial.printf("Drawing X in cell %d\n", move);
            drawX(move);
            lastMove = move;
        }
    }
}
