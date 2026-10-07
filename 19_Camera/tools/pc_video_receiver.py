"""
ESP32-S3 local MJPEG video receiver.

1. Connect the PC to Wi-Fi: ESP32-CAM-Video
2. Password: 12345678
3. Run:
       python pc_video_receiver.py
4. Press Q or ESC to exit.

Requires:
    pip install opencv-python numpy
"""

import sys
import time
import urllib.request

import cv2
import numpy as np

STREAM_URL = "http://192.168.4.1/video"


def iter_jpegs(url: str):
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "ESP32-PC-Video-Receiver/1.0"},
    )

    with urllib.request.urlopen(request, timeout=10) as response:
        buffer = bytearray()

        while True:
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


def main():
    print("ESP32-S3 PC video receiver")
    print(f"Stream: {STREAM_URL}")
    print("Press Q or ESC to exit.")

    while True:
        try:
            frame_count = 0
            start_time = time.perf_counter()

            for jpeg in iter_jpegs(STREAM_URL):
                image = cv2.imdecode(
                    np.frombuffer(jpeg, dtype=np.uint8),
                    cv2.IMREAD_COLOR,
                )

                if image is None:
                    continue

                frame_count += 1
                elapsed = time.perf_counter() - start_time

                if elapsed >= 1.0:
                    fps = frame_count / elapsed
                    cv2.putText(
                        image,
                        f"ESP32 stream  {fps:.1f} FPS",
                        (8, 20),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.5,
                        (0, 255, 0),
                        1,
                        cv2.LINE_AA,
                    )
                    frame_count = 0
                    start_time = time.perf_counter()

                cv2.imshow("ESP32-S3 Camera", image)

                key = cv2.waitKey(1) & 0xFF
                if key in (ord("q"), ord("Q"), 27):
                    cv2.destroyAllWindows()
                    return

        except Exception as exc:
            cv2.destroyAllWindows()
            print(f"Connection lost: {exc}")
            print("Retrying in 2 seconds...")
            time.sleep(2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        cv2.destroyAllWindows()
        sys.exit(0)
