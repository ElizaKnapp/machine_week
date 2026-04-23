from flask import Flask, request, jsonify
import numpy as np
import cv2
import os
#communicates with arduino
import serial
import serial.tools.list_ports
import time

app = Flask(__name__)


def _cluster_positions(vals, gap=20):
    if not vals:
        return []
    vals = sorted(vals)
    groups = [[vals[0]]]
    for v in vals[1:]:
        if abs(v - groups[-1][-1]) <= gap:
            groups[-1].append(v)
        else:
            groups.append([v])
    return [int(sum(g) / len(g)) for g in groups]


def _extract_grid_lines(binary_inv):
    h, w = binary_inv.shape

    horiz_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (max(15, w // 8), 1))
    vert_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (1, max(15, h // 8)))

    horiz = cv2.morphologyEx(binary_inv, cv2.MORPH_OPEN, horiz_kernel)
    vert = cv2.morphologyEx(binary_inv, cv2.MORPH_OPEN, vert_kernel)

    horiz_sum = np.sum(horiz > 0, axis=1)
    vert_sum = np.sum(vert > 0, axis=0)

    y_candidates = np.where(horiz_sum > 0.4 * horiz_sum.max())[0].tolist() if horiz_sum.max() > 0 else []
    x_candidates = np.where(vert_sum > 0.4 * vert_sum.max())[0].tolist() if vert_sum.max() > 0 else []

    ys = _cluster_positions(y_candidates, gap=max(8, h // 40))
    xs = _cluster_positions(x_candidates, gap=max(8, w // 40))

    return xs, ys, horiz, vert


def _pick_two_inner_lines(lines, limit):
    if len(lines) < 2:
        return None

    if len(lines) == 2:
        return sorted(lines)

    best = None
    best_score = float("inf")

    for i in range(len(lines) - 1):
        for j in range(i + 1, len(lines)):
            a, b = sorted((lines[i], lines[j]))
            dist = b - a
            if dist <= 0:
                continue

            mid = (a + b) / 2
            center_score = abs(mid - limit / 2)

            # reject pairs that are implausibly close
            spacing_score = 0 if dist > limit * 0.12 else 1e6

            score = center_score + spacing_score
            if score < best_score:
                best_score = score
                best = [a, b]

    return best


def _infer_boundaries_from_inner_lines(inner_lines, limit):
    if inner_lines is None or len(inner_lines) != 2:
        return None

    a, b = sorted(inner_lines)
    cell = b - a

    left = int(round(a - cell))
    right = int(round(b + cell))

    left = max(0, left)
    right = min(limit - 1, right)

    return [left, a, b, right]


def _classify_cell(cell_bgr):
    gray = cv2.cvtColor(cell_bgr, cv2.COLOR_BGR2GRAY)
    blur = cv2.GaussianBlur(gray, (5, 5), 0)

    _, th = cv2.threshold(
        blur, 0, 255, cv2.THRESH_BINARY_INV + cv2.THRESH_OTSU
    )

    h, w = th.shape

    mx = max(2, w // 8)
    my = max(2, h // 8)
    roi = th[my:h-my, mx:w-mx]

    if roi.size == 0:
        return "."

    ink_ratio = np.count_nonzero(roi) / roi.size
    if ink_ratio < 0.06:
        return "."

    contours, hierarchy = cv2.findContours(
        roi, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE
    )

    if not contours:
        return "."

    roi_h, roi_w = roi.shape
    roi_area = roi_h * roi_w

    good_contours = []
    for i, cnt in enumerate(contours):
        area = cv2.contourArea(cnt)
        if area >= 0.08 * roi_area:
            good_contours.append((i, cnt, area))

    if not good_contours:
        return "."

    # ---------- O score ----------
    best_o_score = -1.0

    for i, cnt, area in good_contours:
        peri = cv2.arcLength(cnt, True)
        if peri <= 0:
            continue

        circularity = 4 * np.pi * area / (peri * peri)
        x, y, ww, hh = cv2.boundingRect(cnt)
        aspect = ww / max(hh, 1)

        has_hole = hierarchy is not None and hierarchy[0][i][2] != -1

        cx = roi_w // 2
        cy = roi_h // 2
        yy, xx = np.ogrid[:roi_h, :roi_w]
        dist2 = (xx - cx) ** 2 + (yy - cy) ** 2

        outer_r = min(roi_h, roi_w) * 0.32
        inner_r = outer_r * 0.55

        ring_mask = (dist2 <= outer_r ** 2) & (dist2 >= inner_r ** 2)
        center_mask = dist2 <= (inner_r * 0.65) ** 2

        ring_ratio = np.count_nonzero(roi[ring_mask]) / max(np.count_nonzero(ring_mask), 1)
        center_ratio = np.count_nonzero(roi[center_mask]) / max(np.count_nonzero(center_mask), 1)

        ring_score = ring_ratio - center_ratio

        o_score = 0.0
        if 0.65 <= circularity <= 1.15:
            o_score += 1.2
        if 0.8 <= aspect <= 1.2:
            o_score += 1.0
        if has_hole:
            o_score += 1.6
        if ring_score > 0.16:
            o_score += 1.4
        if ring_score > 0.24:
            o_score += 0.8

        best_o_score = max(best_o_score, o_score)

    # ---------- X score ----------
    diag1_vals = []
    diag2_vals = []

    n = min(roi_h, roi_w)
    for i in range(n):
        y = int(i * roi_h / n)
        x1 = int(i * roi_w / n)
        x2 = int((n - 1 - i) * roi_w / n)

        y = min(max(y, 0), roi_h - 1)
        x1 = min(max(x1, 0), roi_w - 1)
        x2 = min(max(x2, 0), roi_w - 1)

        diag1_vals.append(roi[y, x1] > 0)
        diag2_vals.append(roi[y, x2] > 0)

    diag1_ratio = np.mean(diag1_vals)
    diag2_ratio = np.mean(diag2_vals)

    center_box = roi[roi_h // 3: 2 * roi_h // 3, roi_w // 3: 2 * roi_w // 3]
    center_ratio = np.count_nonzero(center_box) / max(center_box.size, 1)

    x_score = 0.0
    if diag1_ratio > 0.42:
        x_score += 1.2
    if diag2_ratio > 0.42:
        x_score += 1.2
    if diag1_ratio > 0.52 and diag2_ratio > 0.52:
        x_score += 1.2
    if center_ratio > 0.22:
        x_score += 0.6

    print(
        f"    ink={ink_ratio:.3f}, O_score={best_o_score:.2f}, "
        f"X_score={x_score:.2f}, diag1={diag1_ratio:.2f}, "
        f"diag2={diag2_ratio:.2f}, center={center_ratio:.2f}"
    )

    # ---------- stricter decision ----------
    # only classify if clearly confident; otherwise default to empty
    if best_o_score >= 3.8 and best_o_score >= x_score + 0.9:
        return "O"

    if x_score >= 3.2 and x_score >= best_o_score + 0.7:
        return "X"

    return "."


def detect_board(img):
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    blur = cv2.GaussianBlur(gray, (5, 5), 0)

    _, binary_inv = cv2.threshold(
        blur, 0, 255, cv2.THRESH_BINARY_INV + cv2.THRESH_OTSU
    )

    xs, ys, horiz_img, vert_img = _extract_grid_lines(binary_inv)

    x_inner = _pick_two_inner_lines(xs, img.shape[1])
    y_inner = _pick_two_inner_lines(ys, img.shape[0])

    xs = _infer_boundaries_from_inner_lines(x_inner, img.shape[1])
    ys = _infer_boundaries_from_inner_lines(y_inner, img.shape[0])

    if xs is None or ys is None:
        raise ValueError("Could not detect 2 vertical and 2 horizontal inner grid lines")

    # save debug grid overlay
    debug = img.copy()
    for x in xs:
        cv2.line(debug, (x, 0), (x, debug.shape[0] - 1), (0, 255, 0), 2)
    for y in ys:
        cv2.line(debug, (0, y), (debug.shape[1] - 1, y), (0, 255, 0), 2)
    cv2.imwrite("debug_grid.jpg", debug)

    os.makedirs("images", exist_ok=True)

    board = []

    for r in range(3):
        row = []
        for c in range(3):
            x0, x1 = xs[c], xs[c + 1]
            y0, y1 = ys[r], ys[r + 1]

            pad_x = max(2, (x1 - x0) // 10)
            pad_y = max(2, (y1 - y0) // 10)

            cx0 = min(max(x0 + pad_x, 0), img.shape[1] - 1)
            cx1 = min(max(x1 - pad_x, 1), img.shape[1])
            cy0 = min(max(y0 + pad_y, 0), img.shape[0] - 1)
            cy1 = min(max(y1 - pad_y, 1), img.shape[0])

            if cx1 <= cx0 or cy1 <= cy0:
                row.append("?")
                continue

            cell = img[cy0:cy1, cx0:cx1]

            cell_name = f"cell_r{r}_c{c}.jpg"
            cell_path = os.path.join("images", cell_name)
            cv2.imwrite(cell_path, cell)

            label = _classify_cell(cell)
            print(f"Cell ({r},{c}) saved to {cell_path}: classified as: {label}")

            row.append(label)

        board.append(row)

    return board

#TURN CONTROL
def count_moves(board):
    flat = sum(board, [])
    return flat.count("X"), flat.count("O")

#code for game logic (choosing positon- currently takes first empty spot)
def choose_move(board):
    positions = board_to_positions(board)

    # take first empty spot
    for i in range(1, 10):
        if positions[i] == ".":
            return i

    return None

def board_to_positions(board):
    positions = {}
    idx = 1
    for r in range(3):
        for c in range(3):
            positions[idx] = board[r][c]
            idx += 1
    return positions

def send_move_to_arduino(move, player="X"):
    cmd = f"{player}{move}\n"
    print("Sending:", cmd)
    arduino.write(cmd.encode())

@app.route("/detect", methods=["POST"])
def detect():
    print("Were are detecting")
    data = request.data
    if not data:
        return jsonify({"error": "no image received"}), 400

    arr = np.frombuffer(data, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)

    if img is None:
        return jsonify({"error": "could not decode image"}), 400

    try:
        board = detect_board(img)
    except Exception as e:
        return jsonify({"error": str(e)}), 400

    # --- turn logic ---
    x_count, o_count = count_moves(board)

    move = None

    # bot is X, human is O
    if x_count <= o_count:
        move = choose_move(board)
        if move:
            send_move_to_arduino(move, player="X")
#---end of turn logic----
    return jsonify({
        "board": board,
        "move": move
    })


if __name__ == "__main__":
    # TODO: update port based on individual configurations
    arduino = serial.Serial('/dev/cu.usbmodem101', 115200)
    time.sleep(2)
    app.run(host="0.0.0.0", port=6000, debug=True)