# Tic-Tac-Toe Robot

A two-board ESP32 system that plays tic-tac-toe against a human. The robot draws **X** with a pen plotter; the human draws **O** by hand. Both boards coordinate through a Python server over WiFi — there is no direct wiring between them.

For the full technical spec, see [`SPEC.md`](SPEC.md).

---

## Hardware

| Board | Role |
|---|---|
| AI Thinker ESP32-CAM | Captures images, detects the human's move via the Python server |
| Seeed XIAO ESP32-C3 | Drives X/Y steppers and the pen servo; polls the server for the next move |

**Power:** The motor board should be powered from a battery (or a beefy external supply), **not** from laptop USB. Drawing strokes pull enough current to brown out a laptop-powered board, which causes the pen to tap instead of drawing.

**No inter-board wiring.** Each board talks only to the Python server over WiFi.

---

## Software

| File | Runs on |
|---|---|
| `camera_code/camera_code.ino` | Camera board (AI Thinker ESP32-CAM) |
| `stepper_code/stepper_code.ino` | Motor board (XIAO ESP32-C3) |
| `detect.py` | Laptop (Python Flask server, port 6000) |

---

## Setup

### 1. Laptop

```bash
pip install -r requirements.txt
python detect.py
```

The server binds `0.0.0.0:6000`. Note the laptop's LAN IP — if it isn't `192.168.0.160`, update the three URL constants in both `camera_code/camera_code.ino` and `stepper_code/stepper_code.ino`.

### 2. Motor board (XIAO ESP32-C3)

- Install libraries: **AccelStepper** and **ESP32Servo**.
- Open `stepper_code/stepper_code.ino`.
- Tools → Board → ESP32 Arduino → **XIAO_ESP32C3**.
- Flash.
- Open the USB serial monitor. Jog the plotter with `XR`/`XL`/`YU`/`YD` to your desired home position, then type `H` to set origin.

### 3. Camera board (AI Thinker ESP32-CAM)

- Install the **esp32 by Espressif Systems** board package (provides `esp_camera.h`).
- Open `camera_code/camera_code.ino`.
- Tools → Board → ESP32 Arduino → **AI Thinker ESP32-CAM**.
- Tools → Partition Scheme → **Huge APP (3MB No OTA/1MB SPIFFS)**.
- Flash with **IO0 pulled to GND** during upload (release after).

### 4. WiFi

Both boards connect to `MAKERSPACE` (password `12345678`) on boot. Update the credentials in both sketches if your network differs.

---

## Gameplay

1. Make sure `detect.py` is running on the laptop.
2. Power on both boards. They connect to WiFi and idle — the motor polls `/get-move` every 2 seconds, the camera waits for `start`.
3. On the camera board's serial monitor, type **`start`**. This:
   - resets the Python server's game state,
   - signals the motor to re-home and redraw a fresh grid on paper,
   - waits for the motor to confirm ready, then
   - begins scanning the board every 10 seconds.
4. Draw an **O** by hand in any empty cell.
5. Within ~10 seconds the camera will scan, the server will validate your move, compute the robot's **X**, and the motor will draw it.
6. Repeat until someone wins or the board fills.

---

## Communication Architecture

No wires between the boards — everything goes through the laptop.

```
           ┌─────────────────────────────────────┐
           │  Python Server (detect.py)          │
           │  http://192.168.0.160:6000          │
           │                                     │
           │  /detect   POST JPEG → {board, move}│
           │  /get-move GET → {move}             │
           │  /reset    POST                     │
           │  /ready    POST / GET               │
           └──────▲──────────────────────▲───────┘
                  │ JPEG + move           │ polls every 2s
                  │ every 10s             │ draws when new X
                  │                       │
         ┌────────┴────────┐     ┌────────┴────────┐
         │  Camera Board   │     │  Motor Board    │
         │  ESP32-CAM      │     │  XIAO ESP32-C3  │
         │  camera_code    │     │  stepper_code   │
         └─────────────────┘     └────────┬────────┘
                                          │
                                          ▼
                                        Paper
```

---

## Serial Commands

### Camera board (USB serial, 115200 baud)

| Command | Effect |
|---|---|
| `start` | Reset game, trigger motor to redraw grid, begin scanning loop |

### Motor board (USB serial, 115200 baud — for bench testing)

| Command | Effect |
|---|---|
| `H` | Set current position as home (no movement) |
| `HOME` | Pen up, move to `(0, 0)` |
| `GRID` | Draw the tic-tac-toe grid |
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

## Troubleshooting

| Symptom | Fix |
|---|---|
| Pen taps up and down instead of drawing strokes | Power brownout. Run the motor board from a battery instead of laptop USB. |
| `esp_camera.h: No such file` on the camera board | Install the Espressif ESP32 board package in Arduino IDE. |
| `start` doesn't trigger grid redraw on the motor | Known bug — see SPEC.md §11. The `move == -1` reset branch in `stepper_code.ino` is currently unreachable. |
| Camera board won't connect to WiFi | Check SSID/password in `camera_code/camera_code.ino`. The camera prints WiFi status on its USB serial monitor at boot. |
| Detection returns garbage | Check lighting and the camera's view of the full grid. Inspect `latest.jpg`, `debug_grid.jpg`, and `images/cell_r*_c*.jpg` after a scan. |
