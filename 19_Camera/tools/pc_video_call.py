"""
ESP32-S3 bidirectional video test.

PC camera -> ESP32 LCD:
    POST http://192.168.4.1:81/pcvideo

ESP32 camera -> PC:
    GET  http://192.168.4.1/video

Connect the PC to Wi-Fi:
    ESP32-CAM-Video
    Password: 12345678

Run:
    python pc_video_call.py

Press Q or ESC to exit.
"""

import sys
import time
import threading
import urllib.request

import cv2
import numpy as np

ESP32_HOST = "192.168.4.1"
PC_TO_ESP32_URL = f"http://{ESP32_HOST}:81/pcvideo"
ESP32_TO_PC_URL = f"http://{ESP32_HOST}/video"

CAMERA_INDEX = 0
WIDTH = 320
HEIGHT = 240
JPEG_QUALITY = 55
TARGET_FPS = 8

state_lock = threading.Lock()
latest_pc_frame = None
latest_esp_frame = None
running = True
pc_send_fps = 0.0
esp_recv_fps = 0.0


def iter_jpegs(url):
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "ESP32-Bidirectional-Video/1.0"},
    )

    with urllib.request.urlopen(request, timeout=10) as response:
        buffer = bytearray()

        while running:
            chunk = response.read(4096)
            if not chunk:
                return

            buffer.extend(chunk)

            while True:
                start = buffer.find(b"\xff\xd8")
                if start < 0:
                    if len(buffer) > 2:
                        del buffer[:-2]
                    break

                end = buffer.find(b"\xff\xd9", start + 2)
                if end < 0:
                    if start > 0:
                        del buffer[:start]
                    break

                jpeg = bytes(buffer[start:end + 2])
                del buffer[:end + 2]
                yield jpeg


def receive_thread():
    global latest_esp_frame, esp_recv_fps

    while running:
        count = 0
        start_time = time.perf_counter()

        try:
            for jpeg in iter_jpegs(ESP32_TO_PC_URL):
                image = cv2.imdecode(
                    np.frombuffer(jpeg, dtype=np.uint8),
                    cv2.IMREAD_COLOR,
                )
                if image is None:
                    continue

                image = cv2.resize(image, (WIDTH, HEIGHT))
                with state_lock:
                    latest_esp_frame = image

                count += 1
                elapsed = time.perf_counter() - start_time
                if elapsed >= 1.0:
                    esp_recv_fps = count / elapsed
                    count = 0
                    start_time = time.perf_counter()

        except Exception as exc:
            if running:
                print(f"[ESP32 -> PC] connection lost: {exc}")
                time.sleep(1.0)


def send_thread():
    global latest_pc_frame, pc_send_fps

    cap = cv2.VideoCapture(CAMERA_INDEX, cv2.CAP_DSHOW)
    if not cap.isOpened():
        print(
            f"Cannot open PC camera index {CAMERA_INDEX}. "
            "Try CAMERA_INDEX = 1."
        )
        return

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, WIDTH)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, HEIGHT)

    interval = 1.0 / TARGET_FPS
    last_send = 0.0
    count = 0
    start_time = time.perf_counter()

    try:
        while running:
            ok, frame = cap.read()
            if not ok:
                print("[PC -> ESP32] camera capture failed.")
                break

            frame = cv2.resize(frame, (WIDTH, HEIGHT), interpolation=cv2.INTER_AREA)

            with state_lock:
                latest_pc_frame = frame.copy()

            now = time.perf_counter()
            if now - last_send < interval:
                continue

            ok_jpg, encoded = cv2.imencode(
                ".jpg",
                frame,
                [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY],
            )

            if ok_jpg:
                request = urllib.request.Request(
                    PC_TO_ESP32_URL,
                    data=encoded.tobytes(),
                    headers={
                        "Content-Type": "image/jpeg",
                        "Connection": "close",
                    },
                    method="POST",
                )

                try:
                    with urllib.request.urlopen(request, timeout=1.5) as response:
                        response.read()
                    count += 1
                except Exception as exc:
                    print(f"[PC -> ESP32] send error: {exc}")

            last_send = now

            elapsed = now - start_time
            if elapsed >= 1.0:
                pc_send_fps = count / elapsed
                count = 0
                start_time = now

    finally:
        cap.release()


def add_label(image, text):
    cv2.rectangle(image, (0, 0), (WIDTH, 30), (0, 0, 0), -1)
    cv2.putText(
        image,
        text,
        (8, 21),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.52,
        (0, 255, 0),
        1,
        cv2.LINE_AA,
    )
    return image


def main():
    global running

    print("==========================================")
    print(" ESP32-S3 Bidirectional Video Test")
    print(" PC camera  -> ESP32 LCD")
    print(" ESP32 camera -> PC window")
    print("==========================================")
    print(f"ESP32: {ESP32_HOST}")
    print("Press Q or ESC to exit.")

    rx = threading.Thread(target=receive_thread, daemon=True)
    tx = threading.Thread(target=send_thread, daemon=True)
    rx.start()
    tx.start()

    blank = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)

    try:
        while running:
            with state_lock:
                pc = None if latest_pc_frame is None else latest_pc_frame.copy()
                esp = None if latest_esp_frame is None else latest_esp_frame.copy()
                tx_fps = pc_send_fps
                rx_fps = esp_recv_fps

            if pc is None:
                pc = blank.copy()
            if esp is None:
                esp = blank.copy()

            left = add_label(pc, f"PC Camera -> ESP32 LCD  {tx_fps:.1f} FPS")
            right = add_label(esp, f"ESP32 Camera -> PC  {rx_fps:.1f} FPS")

            combined = np.hstack((left, right))
            cv2.imshow("ESP32 Bidirectional Video", combined)

            key = cv2.waitKey(20) & 0xFF
            if key in (ord("q"), ord("Q"), 27):
                break

    finally:
        running = False
        cv2.destroyAllWindows()

    print("Video call test stopped.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        running = False
        cv2.destroyAllWindows()
        sys.exit(0)
