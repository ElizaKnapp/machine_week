# Tic-Tac-Toe Robot

Two ESP32 boards that play tic-tac-toe against a human. The camera board handles vision and game logic; the motor board draws on paper with a pen plotter.

---

## Hardware

| Board | Role |
|---|---|
| AI Thinker ESP32-CAM | Captures images, runs game logic, sends move commands |
| Seeed XIAO ESP32-C3 | Drives X/Y steppers and pen servo to draw on paper |

**Wiring between boards:**

```
Camera GPIO 14 (TX) ──► Motor D10 / GPIO10 (RX)
Camera GPIO 13 (RX) ◄── Motor D6  / GPIO21 (TX)
GND ◄──────────────────► GND
```

---

## Software

| File | Runs on |
|---|---|
| `machine_week.ino` | Camera board (ESP32-CAM) |
| `week10_machine/week10_machine.ino` | Motor board (XIAO ESP32-C3) |
| `detect.py` | Laptop (Python Flask server) |

---

## Setup

**1. Laptop**

```bash
pip install flask opencv-python
python detect.py
```

The server listens on port 6000. Update `serverUrl` in `machine_week.ino` if your laptop's IP differs from `192.168.0.160`.

**2. Motor board**

Flash `week10_machine.ino`. Open the serial monitor and jog the plotter to your desired home position using the jog commands below, then type `H` to set that as the origin.

**3. Camera board**

Flash `machine_week.ino`. Connect to the `MAKERSPACE` WiFi network (or update the credentials in the sketch).

---

## Gameplay

1. Open the camera board's serial monitor.
2. Type `start` — the motor homes and draws the tic-tac-toe grid on the paper.
3. Draw an **X** in whichever cell you want (you are always X).
4. Type `scan` — the camera photographs the board, the Python server detects your move, and the robot draws its **O** response using optimal strategy.
5. Repeat steps 3–4 until someone wins or the game draws.

---

## Camera Board Serial Commands

| Command | Effect |
|---|---|
| `start` | Home motors, draw grid, begin game |
| `scan` | Detect board, validate human move, play robot response |
| `go` | Detect board only — prints state without making a move (debug) |
| `test` | Print WiFi and server connection status |

---

## Motor Board Serial Commands

These can be typed directly into the motor board's USB serial monitor for bench testing without the camera board connected.

| Command | Effect |
|---|---|
| `H` | Set current position as home (no movement) |
| `HOME` | Move to home position (0, 0) |
| `GRID` | Draw tic-tac-toe grid |
| `X1`–`X9` | Draw X in cell 1–9 |
| `O1`–`O9` | Draw O in cell 1–9 |
| `U` / `D` | Pen up / pen down |
| `XR` / `XL` | Jog right / left |
| `YU` / `YD` | Jog up / down |

Cell numbering:

```
1 | 2 | 3
4 | 5 | 6
7 | 8 | 9
```

---

## Architecture

```
Camera board                    Laptop
─────────────────               ──────────────
capture JPEG         HTTP POST  receive JPEG
send to Python   ──────────────►  detect grid
receive board    ◄──────────────  return JSON
run minimax
choose move
     │
     │ UART
     ▼
Motor board
─────────────────
receive command
draw X or O
reply DONE
```
