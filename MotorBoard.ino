#include <AccelStepper.h>
#include <ESP32Servo.h>

const int X_STEP_PIN = D1;
const int X_DIR_PIN  = D5;
const int Y_STEP_PIN = D3;
const int Y_DIR_PIN  = D2;
const int SERVO_PIN  = D4;

AccelStepper stepperX(AccelStepper::DRIVER, X_STEP_PIN, X_DIR_PIN);
AccelStepper stepperY(AccelStepper::DRIVER, Y_STEP_PIN, Y_DIR_PIN);
Servo penServo;

const float maxSpeed = 600.0;
const float accel = 300.0;

const int penUpPos = 20;
const int penDownPos = 90;

const int yDownSign = -1;

const long boardOriginX = 500;
const long boardOriginY = 500;
const long cellSize = 1500;
const long inset = 220;
const long jogStep = 500;

// D7 (GPIO20) <- camera GPIO 14 TX
// D6 (GPIO21) -> camera GPIO 13 RX
#define CAM_RX_PIN D7
#define CAM_TX_PIN D6
#define CAM_BAUD   9600
HardwareSerial CamSerial(1);

String input = "";
String camInput = "";

void setup() {
  Serial.begin(115200);
  CamSerial.begin(CAM_BAUD, SERIAL_8N1, CAM_RX_PIN, CAM_TX_PIN);
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

  Serial.println("Ready");
  Serial.println("Commands: GRID, X1-X9, O1-O9, H, U, D, XR, XL, YU, YD");
}

void loop() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (input.length() > 0) { input.trim(); handleCommand(input, Serial); input = ""; }
    } else { input += c; }
  }

  while (CamSerial.available() > 0) {
    char c = CamSerial.read();
    if (c == '\n' || c == '\r') {
      if (camInput.length() > 0) { camInput.trim(); handleCommand(camInput, CamSerial); camInput = ""; }
    } else { camInput += c; }
  }
}

void handleCommand(String cmd, Stream& src) {
  if (cmd.equalsIgnoreCase("PING")) {
    src.println("OK");
    return;
  }

  if (cmd.equalsIgnoreCase("HOME")) {
    penUp();
    moveTo(0, 0);
    src.println("DONE");
    return;
  }

  if (cmd.equalsIgnoreCase("H")) {
    stepperX.setCurrentPosition(0);
    stepperY.setCurrentPosition(0);
    Serial.println("Home set");
    return;
  }

  if (cmd.equalsIgnoreCase("U")) {
    penUp();
    return;
  }

  if (cmd.equalsIgnoreCase("D")) {
    penDown();
    return;
  }

  if (cmd.equalsIgnoreCase("XR")) {
    moveTo(stepperX.currentPosition() + jogStep, stepperY.currentPosition());
    return;
  }

  if (cmd.equalsIgnoreCase("XL")) {
    moveTo(stepperX.currentPosition() - jogStep, stepperY.currentPosition());
    return;
  }

  if (cmd.equalsIgnoreCase("YU")) {
    moveTo(stepperX.currentPosition(), stepperY.currentPosition() - jogStep);
    return;
  }

  if (cmd.equalsIgnoreCase("YD")) {
    moveTo(stepperX.currentPosition(), stepperY.currentPosition() + jogStep);
    return;
  }

  if (cmd.equalsIgnoreCase("GRID")) {
    drawGrid();
    src.println("DONE");
    return;
  }

  if (cmd.length() == 2 && (cmd[0] == 'X' || cmd[0] == 'x') && cmd[1] >= '1' && cmd[1] <= '9') {
    drawX(cmd[1] - '0');
    src.println("DONE");
    return;
  }

  if (cmd.length() == 2 && (cmd[0] == 'O' || cmd[0] == 'o') && cmd[1] >= '1' && cmd[1] <= '9') {
    drawO(cmd[1] - '0');
    src.println("DONE");
    return;
  }

  src.println("ERR");
  Serial.println("Unknown command: " + cmd);
}

void penUp() {
  penServo.write(penUpPos);
  delay(200);
}

void penDown() {
  penServo.write(penDownPos);
  delay(200);
}

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

long boardY(long offset) {
  return boardOriginY + yDownSign * offset;
}

void cellBounds(int pos, long &left, long &top, long &right, long &bottom) {
  int row = (pos - 1) / 3;
  int col = (pos - 1) % 3;

  left = boardOriginX + col * cellSize;
  right = left + cellSize;

  long yTop = boardY(row * cellSize);
  long yBottom = boardY((row + 1) * cellSize);

  if (yTop < yBottom) {
    top = yTop;
    bottom = yBottom;
  } else {
    top = yBottom;
    bottom = yTop;
  }
}

void drawGrid() {
  long left  = boardOriginX;
  long right = boardOriginX + 3 * cellSize;

  long y1 = boardY(cellSize);
  long y2 = boardY(2 * cellSize);

  long top    = boardY(0);
  long bottom = boardY(3 * cellSize);

  lineTo(left, y1, right, y1);
  lineTo(left, y2, right, y2);

  lineTo(boardOriginX + cellSize, top, boardOriginX + cellSize, bottom);
  lineTo(boardOriginX + 2 * cellSize, top, boardOriginX + 2 * cellSize, bottom);
}

void drawX(int pos) {
  long left, top, right, bottom;
  cellBounds(pos, left, top, right, bottom);

  lineTo(left + inset, top + inset, right - inset, bottom - inset);
  lineTo(right - inset, top + inset, left + inset, bottom - inset);
}

void drawO(int pos) {
  long left, top, right, bottom;
  cellBounds(pos, left, top, right, bottom);

  long cx = (left + right) / 2;
  long cy = (top + bottom) / 2;
  long r = cellSize / 2 - inset;
  long d = r * 7 / 10;

  long x1 = cx,     y1 = cy - r;
  long x2 = cx + d, y2 = cy - d;
  long x3 = cx + r, y3 = cy;
  long x4 = cx + d, y4 = cy + d;
  long x5 = cx,     y5 = cy + r;
  long x6 = cx - d, y6 = cy + d;
  long x7 = cx - r, y7 = cy;
  long x8 = cx - d, y8 = cy - d;

  penUp();
  moveTo(x1, y1);
  penDown();

  moveTo(x2, y2);
  moveTo(x3, y3);
  moveTo(x4, y4);
  moveTo(x5, y5);
  moveTo(x6, y6);
  moveTo(x7, y7);
  moveTo(x8, y8);
  moveTo(x1, y1);

  penUp();
}