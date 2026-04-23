from flask import Flask, request, jsonify
import numpy as np
import cv2
import os

app = Flask(__name__)

# -------------------- App state --------------------
current_move   = None   # int 1-9, or None when no move is pending
previous_board = None   # last accepted [[row],[row],[row]] for move validation
reset_pending  = False  # True after /reset until motor polls /get-move
motor_ready    = False  # True after motor posts /ready


# -------------------- Vision helpers --------------------

def _cluster_positions(vals, gap=20):
    """Cluster raw pixel positions and return (centroid, vote_count) pairs
    sorted by vote count descending so callers can take the top-N."""
    if not vals:
        return []
    vals = sorted(vals)
    groups = [[vals[0]]]
    for v in vals[1:]:
        if abs(v - groups[-1][-1]) <= gap:
            groups[-1].append(v)
        else:
            groups.append([v])
    # Return (centroid, votes) sorted by votes desc so top-2 = strongest lines
    scored = sorted(
        [(int(sum(g) / len(g)), len(g)) for g in groups],
        key=lambda x: -x[1]
    )
    return scored


def _top2(clusters):
    """Return the 2 highest-vote cluster centroids, sorted by position."""
    top = [pos for pos, _ in clusters[:2]]
    return sorted(top)


def _hough_detect_lines(gray):
    """Detect near-horizontal and near-vertical lines via Canny + HoughLinesP.
    More robust than morphological projection for blurry or slightly tilted grids.
    Returns at most 2 lines per axis (the 2 most-voted clusters)."""
    h, w = gray.shape
    blurred = cv2.GaussianBlur(gray, (3, 3), 0)
    edges   = cv2.Canny(blurred, 20, 80)

    min_len   = max(20, min(w, h) // 6)
    threshold = max(20, min_len // 2)
    lines = cv2.HoughLinesP(edges, 1, np.pi / 180,
                             threshold=threshold,
                             minLineLength=min_len,
                             maxLineGap=15)
    if lines is None:
        return [], []

    x_pos, y_pos = [], []
    for line in lines:
        x1, y1, x2, y2 = line[0]
        angle = abs(np.degrees(np.arctan2(y2 - y1, x2 - x1)))
        if angle < 25 or angle > 155:
            y_pos.append((y1 + y2) // 2)
        elif 65 < angle < 115:
            x_pos.append((x1 + x2) // 2)

    xs = _top2(_cluster_positions(x_pos, gap=max(8, w // 40)))
    ys = _top2(_cluster_positions(y_pos, gap=max(8, h // 40)))
    return xs, ys


def _extract_grid_lines(gray):
    """Detect the 2 vertical and 2 horizontal grid lines.
    Uses adaptive thresholding + morphological projection as the primary method,
    with Hough lines as a fallback for blurry or slightly tilted images.
    Always picks the 2 strongest (highest-vote) lines per axis."""
    h, w = gray.shape

    # Adaptive thresholding is more robust than global Otsu when a dark object
    # (e.g. the machine arm) skews the intensity histogram.
    binary_inv = cv2.adaptiveThreshold(
        gray, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY_INV, 21, 8
    )

    horiz_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (max(15, w // 8), 1))
    vert_kernel  = cv2.getStructuringElement(cv2.MORPH_RECT, (1, max(15, h // 8)))

    horiz = cv2.morphologyEx(binary_inv, cv2.MORPH_OPEN, horiz_kernel)
    vert  = cv2.morphologyEx(binary_inv, cv2.MORPH_OPEN, vert_kernel)

    horiz_sum = np.sum(horiz > 0, axis=1)
    vert_sum  = np.sum(vert  > 0, axis=0)

    thresh       = 0.25
    y_candidates = np.where(horiz_sum > thresh * horiz_sum.max())[0].tolist() if horiz_sum.max() > 0 else []
    x_candidates = np.where(vert_sum  > thresh * vert_sum.max())[0].tolist()  if vert_sum.max()  > 0 else []

    # Take the 2 strongest clusters per axis
    xs = _top2(_cluster_positions(x_candidates, gap=max(8, w // 40)))
    ys = _top2(_cluster_positions(y_candidates, gap=max(8, h // 40)))

    # Hough fallback: kick in for whichever axis came up short
    if len(xs) < 2 or len(ys) < 2:
        hx, hy = _hough_detect_lines(gray)
        if len(xs) < 2 and len(hx) >= 2:
            print("  [lines] using Hough for X axis")
            xs = hx
        if len(ys) < 2 and len(hy) >= 2:
            print("  [lines] using Hough for Y axis")
            ys = hy

    return xs, ys, horiz, vert


def _pick_two_inner_lines(lines, limit):
    if len(lines) < 2:
        return None
    if len(lines) == 2:
        return sorted(lines)

    best       = None
    best_score = float("inf")

    for i in range(len(lines) - 1):
        for j in range(i + 1, len(lines)):
            a, b = sorted((lines[i], lines[j]))
            dist = b - a
            if dist <= 0:
                continue
            mid           = (a + b) / 2
            center_score  = abs(mid - limit / 2)
            spacing_score = 0 if dist > limit * 0.12 else 1e6
            score         = center_score + spacing_score
            if score < best_score:
                best_score = score
                best       = [a, b]

    return best


def _infer_boundaries_from_inner_lines(inner_lines, limit):
    if inner_lines is None or len(inner_lines) != 2:
        return None
    a, b  = sorted(inner_lines)
    cell  = b - a
    left  = max(0,         int(round(a - cell)))
    right = min(limit - 1, int(round(b + cell)))
    return [left, a, b, right]


def _classify_cell(cell_bgr, gray_path=None):
    gray = cv2.cvtColor(cell_bgr, cv2.COLOR_BGR2GRAY)
    blur = gray


    if gray_path:
        cv2.imwrite(gray_path, blur)

    print(blur.min())
    print(blur.max())

    # Reject blank cells before Otsu — Otsu on a uniform image produces ~50% noise.
    # Count pixels meaningfully darker than the background (ink on paper).
    background = np.percentile(blur, 40)   # bright background level
    print(background)

    dark_ratio  = np.sum(blur < background - 18) / blur.size
    print(f"    std={blur.std():.1f}  dark_ratio={dark_ratio:.3f}")
    if dark_ratio < 0.04:   # less than 4% real dark pixels → blank
        return "."

    _, th = cv2.threshold(blur, 0, 255, cv2.THRESH_BINARY_INV + cv2.THRESH_OTSU)

    h, w  = th.shape
    mx    = max(2, w // 8)
    my    = max(2, h // 8)
    roi   = th[my:h - my, mx:w - mx]

    if roi.size == 0:
        return "."

    ink_ratio = np.count_nonzero(roi) / roi.size
    if ink_ratio < 0.10:
        return "."

    contours, hierarchy = cv2.findContours(roi, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return "."

    roi_h, roi_w = roi.shape
    roi_area     = roi_h * roi_w

    good_contours = [
        (i, cnt, cv2.contourArea(cnt))
        for i, cnt in enumerate(contours)
        if cv2.contourArea(cnt) >= 0.08 * roi_area
    ]
    if not good_contours:
        return "."

    # ---------- O score ----------
    best_o_score = -1.0

    for i, cnt, area in good_contours:
        peri = cv2.arcLength(cnt, True)
        if peri <= 0:
            continue

        circularity = 4 * np.pi * area / (peri * peri)
        _, _, ww, hh = cv2.boundingRect(cnt)
        aspect   = ww / max(hh, 1)
        has_hole = hierarchy is not None and hierarchy[0][i][2] != -1

        cx = roi_w // 2
        cy = roi_h // 2
        yy, xx = np.ogrid[:roi_h, :roi_w]
        dist2    = (xx - cx) ** 2 + (yy - cy) ** 2
        outer_r  = min(roi_h, roi_w) * 0.45   # wider ring catches larger O's
        inner_r  = outer_r * 0.50

        ring_mask   = (dist2 <= outer_r ** 2) & (dist2 >= inner_r ** 2)
        center_mask = dist2 <= (inner_r * 0.60) ** 2

        ring_ratio   = np.count_nonzero(roi[ring_mask])   / max(np.count_nonzero(ring_mask),   1)
        center_ratio = np.count_nonzero(roi[center_mask]) / max(np.count_nonzero(center_mask), 1)
        ring_score   = ring_ratio - center_ratio

        o_score = 0.0
        if 0.55 <= circularity <= 1.25: o_score += 1.2  # looser — hand-drawn circles
        if 0.5  <= aspect      <= 2.0:  o_score += 1.0  # allow ovals (taller or wider)
        if has_hole:                    o_score += 1.6   # bonus if hole is clear
        if ring_score > 0.16:           o_score += 1.4
        if ring_score > 0.24:           o_score += 0.8

        best_o_score = max(best_o_score, o_score)

    # ---------- X score ----------
    n           = min(roi_h, roi_w)
    diag1_vals  = []
    diag2_vals  = []

    for i in range(n):
        y  = min(max(int(i * roi_h / n), 0), roi_h - 1)
        x1 = min(max(int(i * roi_w / n), 0), roi_w - 1)
        x2 = min(max(int((n - 1 - i) * roi_w / n), 0), roi_w - 1)
        diag1_vals.append(roi[y, x1] > 0)
        diag2_vals.append(roi[y, x2] > 0)

    diag1_ratio  = np.mean(diag1_vals)
    diag2_ratio  = np.mean(diag2_vals)
    center_box   = roi[roi_h // 3: 2 * roi_h // 3, roi_w // 3: 2 * roi_w // 3]
    center_ratio = np.count_nonzero(center_box) / max(center_box.size, 1)

    x_score = 0.0
    if diag1_ratio > 0.36:                              x_score += 1.2
    if diag2_ratio > 0.36:                              x_score += 1.2
    if diag1_ratio > 0.46 and diag2_ratio > 0.46:      x_score += 1.2
    if center_ratio > 0.18:                             x_score += 0.6

    print(
        f"    ink={ink_ratio:.3f}, O_score={best_o_score:.2f}, "
        f"X_score={x_score:.2f}, diag1={diag1_ratio:.2f}, "
        f"diag2={diag2_ratio:.2f}, center={center_ratio:.2f}"
    )

    # Diagonal veto: if both diagonals carry significant ink it's an X, not an O.
    both_diags = diag1_ratio > 0.28 and diag2_ratio > 0.28
    if best_o_score >= 3.0 and best_o_score >= x_score + 0.7 and not both_diags:
        return "O"
    if x_score >= 2.8 and x_score >= best_o_score + 0.5:
        return "X"
    return "."


def _crop_to_board(img):
    """Crop to the centre third of the frame in both dimensions.
    The board is positioned in the centre of the camera view, and this
    eliminates the pen arm (near the home corner) and blank margins."""
    h, w = img.shape[:2]
    x0, x1 = w // 4, 3 * w // 4
    y0, y1 = h // 3, 5 * h // 6

    # Save annotated full frame so you can verify the crop region
    annotated = img.copy()
    cv2.rectangle(annotated, (x0, y0), (x1, y1), (0, 255, 0), 3)
    cv2.imwrite("debug_box.jpg", annotated)
    print(f"Centre crop: ({x0},{y0}) → ({x1},{y1})")

    return img[y0:y1, x0:x1]


def detect_board(img):
    img = _crop_to_board(img)   # zoom in on the board before detailed analysis

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    blur = cv2.GaussianBlur(gray, (5, 5), 0)

    xs, ys, _, _ = _extract_grid_lines(blur)

    x_inner = _pick_two_inner_lines(xs, img.shape[1])
    y_inner = _pick_two_inner_lines(ys, img.shape[0])

    xs = _infer_boundaries_from_inner_lines(x_inner, img.shape[1])
    ys = _infer_boundaries_from_inner_lines(y_inner, img.shape[0])

    if xs is None or ys is None:
        raise ValueError("Could not detect 2 vertical and 2 horizontal inner grid lines")

    # Draw only the 2 inner lines per axis (xs/ys[1] and xs/ys[2])
    debug = img.copy()
    for x in (xs[1], xs[2]):
        cv2.line(debug, (x, 0), (x, debug.shape[0] - 1), (0, 255, 0), 2)
    for y in (ys[1], ys[2]):
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

            cell      = img[cy0:cy1, cx0:cx1]
            cell_path = os.path.join("images", f"cell_r{r}_c{c}.jpg")
            gray_path = os.path.join("images", f"cell_r{r}_c{c}_gray.jpg")
            cv2.imwrite(cell_path, cell)

            label = _classify_cell(cell, gray_path=gray_path)
            print(f"Cell ({r},{c}): {label}")
            row.append(label)
        board.append(row)

    return board


# -------------------- Game logic --------------------

_WIN_LINES = [
    [0, 1, 2], [3, 4, 5], [6, 7, 8],
    [0, 3, 6], [1, 4, 7], [2, 5, 8],
    [0, 4, 8], [2, 4, 6],
]


def _check_winner(flat):
    for line in _WIN_LINES:
        if flat[line[0]] != "." and flat[line[0]] == flat[line[1]] == flat[line[2]]:
            return flat[line[0]]
    return None


def _minimax(flat, is_robot_turn, depth):
    winner = _check_winner(flat)
    if winner == "X":        return 10 - depth
    if winner == "O":        return depth - 10
    if "." not in flat:      return 0

    best = float("-inf") if is_robot_turn else float("inf")
    for i in range(9):
        if flat[i] == ".":
            flat[i] = "X" if is_robot_turn else "O"
            score    = _minimax(flat, not is_robot_turn, depth + 1)
            flat[i]  = "."
            best = max(best, score) if is_robot_turn else min(best, score)
    return best


def choose_move(board):
    flat       = [board[r][c] for r in range(3) for c in range(3)]
    best_score = float("-inf")
    best_move  = None
    for i in range(9):
        if flat[i] == ".":
            flat[i] = "X"
            score   = _minimax(flat, False, 0)
            flat[i] = "."
            if score > best_score:
                best_score = score
                best_move  = i + 1  # 1-indexed cell number
    return best_move


def _is_valid_human_move(new_board, prev_board):
    """Returns True iff exactly 1 new O was placed in a previously empty cell
    and no existing mark was changed or removed."""
    new_os = 0
    for r in range(3):
        for c in range(3):
            old, new = prev_board[r][c], new_board[r][c]
            if old != "." and new != old:
                return False        # existing mark changed or removed
            if old == "." and new == "O":
                new_os += 1
            elif old == "." and new == "X":
                return False        # unexpected robot mark appeared
    return new_os == 1


# -------------------- Routes --------------------

@app.route("/detect", methods=["POST"])
def detect():
    global current_move, previous_board

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
        return jsonify({"error": str(e)}), 404  # grid not detected in image

    # Validate the human's move
    if previous_board is None:
        flat    = [cell for row in board for cell in row]
        valid   = (flat.count("O") == 1 and flat.count("X") == 0)
    else:
        valid   = _is_valid_human_move(board, previous_board)

    if valid:
        previous_board = board
        current_move   = choose_move(board)
        print(f"Valid move accepted. Robot plays cell {current_move}.")
    else:
        print("Invalid or unchanged board — ignoring frame.")

    return jsonify({"board": board, "move": current_move})


@app.route("/get-move", methods=["GET"])
def get_move():
    global reset_pending
    if reset_pending:
        reset_pending = False   # serve the signal once then clear it
        return jsonify({"move": -1})
    return jsonify({"move": current_move if current_move is not None else 0})


@app.route("/reset", methods=["POST"])
def reset_game():
    global current_move, previous_board, reset_pending, motor_ready
    current_move   = None
    previous_board = None
    reset_pending  = True
    motor_ready    = False
    print("Game reset. Waiting for motor to redraw grid.")
    return jsonify({"status": "ok"})


@app.route("/ready", methods=["POST"])
def motor_ready_post():
    global motor_ready
    motor_ready = True
    print("Motor signalled ready — scanning can begin.")
    return jsonify({"status": "ok"})


@app.route("/ready", methods=["GET"])
def motor_ready_get():
    return jsonify({"ready": motor_ready})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=6000, debug=True)
