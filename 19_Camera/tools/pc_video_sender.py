import time
import urllib.request
import cv2

ESP32_URL = "http://192.168.4.1/pcvideo"
CAMERA_INDEX = 0
WIDTH = 320
HEIGHT = 240
JPEG_QUALITY = 55
TARGET_FPS = 8


def main():
    cap = cv2.VideoCapture(CAMERA_INDEX, cv2.CAP_DSHOW)
    if not cap.isOpened():
        raise RuntimeError(
            f"Cannot open PC camera index {CAMERA_INDEX}. "
            "Try CAMERA_INDEX = 1 if your computer has multiple cameras."
        )

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, WIDTH)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, HEIGHT)

    interval = 1.0 / TARGET_FPS
    print(f"Sending PC camera to ESP32: {ESP32_URL}")
    print("Press Q or ESC to stop.")

    last_send = 0.0
    frame_count = 0
    last_report = time.time()

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                print("PC camera frame capture failed.")
                break

            frame = cv2.resize(frame, (WIDTH, HEIGHT), interpolation=cv2.INTER_AREA)

            now = time.time()
            if now - last_send >= interval:
                ok_jpg, encoded = cv2.imencode(
                    ".jpg",
                    frame,
                    [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY],
                )

                if ok_jpg:
                    request = urllib.request.Request(
                        ESP32_URL,
                        data=encoded.tobytes(),
                        headers={
                            "Content-Type": "image/jpeg",
                            "Connection": "keep-alive",
                        },
                        method="POST",
                    )

                    try:
                        with urllib.request.urlopen(request, timeout=1.5) as response:
                            response.read()
                        frame_count += 1
                    except Exception as exc:
                        print(f"ESP32 send error: {exc}")

                last_send = now

            display = frame.copy()
            cv2.putText(
                display,
                "PC Camera -> ESP32 LCD",
                (8, 22),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.55,
                (0, 255, 0),
                1,
                cv2.LINE_AA,
            )
            cv2.imshow("PC Camera Sender", display)

            if now - last_report >= 5.0:
                elapsed = now - last_report
                print(f"Send rate: {frame_count / elapsed:.1f} FPS")
                frame_count = 0
                last_report = now

            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):
                break

    finally:
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
