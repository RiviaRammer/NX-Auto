#!/usr/bin/env python3
"""Preview and capture Nintendo Switch frames from a UVC capture card."""

from __future__ import annotations

import argparse
import os
import sys
import time
from datetime import datetime
from pathlib import Path

import cv2


def parse_device(value: str) -> int | str:
    return int(value) if value.isdecimal() else value


def backend_for(name: str) -> int:
    choices = {
        "auto": cv2.CAP_ANY,
        "dshow": getattr(cv2, "CAP_DSHOW", cv2.CAP_ANY),
        "msmf": getattr(cv2, "CAP_MSMF", cv2.CAP_ANY),
        "v4l2": getattr(cv2, "CAP_V4L2", cv2.CAP_ANY),
        "avfoundation": getattr(cv2, "CAP_AVFOUNDATION", cv2.CAP_ANY),
    }
    return choices[name]


def fourcc_text(value: float) -> str:
    number = int(value)
    return "".join(chr((number >> (8 * i)) & 0xFF) for i in range(4))


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Preview/snapshot a Nintendo Switch connected through a UVC card."
    )
    parser.add_argument("--device", default="0", help="Camera index or /dev/videoX")
    parser.add_argument(
        "--backend",
        choices=("auto", "dshow", "msmf", "v4l2", "avfoundation"),
        default="auto",
        help="OpenCV capture backend",
    )
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--fourcc", default="MJPG", help="FourCC requested from card")
    parser.add_argument("--output", type=Path, default=Path("captures"))
    parser.add_argument("--headless", action="store_true", help="Do not create a window")
    parser.add_argument(
        "--save-every",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="Periodically save frames (implies headless-compatible operation)",
    )
    parser.add_argument(
        "--max-frames", type=int, default=0, help="Exit after N frames; 0 runs forever"
    )
    return parser


def open_capture(args: argparse.Namespace) -> cv2.VideoCapture:
    device = parse_device(args.device)
    capture = cv2.VideoCapture(device, backend_for(args.backend))
    if not capture.isOpened():
        raise RuntimeError(
            f"无法打开采集设备 {args.device!r}；请确认它未被 OBS/相机应用占用，并尝试指定 --backend"
        )

    if len(args.fourcc) != 4:
        raise ValueError("--fourcc 必须恰好是 4 个字符，例如 MJPG 或 YUY2")

    # Compressed MJPEG is the practical default for USB 2.0 at 720p.
    capture.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*args.fourcc))
    capture.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    capture.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    capture.set(cv2.CAP_PROP_FPS, args.fps)
    capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    return capture


def save_frame(frame, output: Path) -> Path:
    output.mkdir(parents=True, exist_ok=True)
    name = datetime.now().strftime("ns_%Y%m%d_%H%M%S_%f")[:-3] + ".jpg"
    target = output / name
    if not cv2.imwrite(os.fspath(target), frame, [cv2.IMWRITE_JPEG_QUALITY, 95]):
        raise RuntimeError(f"写入截图失败: {target}")
    return target


def main() -> int:
    args = make_parser().parse_args()
    if args.width <= 0 or args.height <= 0 or args.fps <= 0:
        raise ValueError("分辨率和帧率必须大于 0")
    if args.save_every < 0 or args.max_frames < 0:
        raise ValueError("--save-every 和 --max-frames 不能为负数")

    capture = open_capture(args)
    actual = (
        int(capture.get(cv2.CAP_PROP_FRAME_WIDTH)),
        int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT)),
        capture.get(cv2.CAP_PROP_FPS),
        fourcc_text(capture.get(cv2.CAP_PROP_FOURCC)),
    )
    print(
        f"已打开 {args.device}: {actual[0]}x{actual[1]} @ {actual[2]:.2f} fps, "
        f"FourCC={actual[3]!r}"
    )
    if (actual[0], actual[1]) != (args.width, args.height):
        print("警告：采集卡未接受请求的分辨率，以上述实际参数运行。", file=sys.stderr)
    if not args.headless:
        print("按 S 保存原始帧，按 Q 或 Esc 退出。")

    frames = 0
    failures = 0
    last_save = time.monotonic()
    try:
        while args.max_frames == 0 or frames < args.max_frames:
            ok, frame = capture.read()
            if not ok or frame is None:
                failures += 1
                if failures >= 30:
                    raise RuntimeError("连续 30 次读取失败；请重新插拔采集卡或降低分辨率/帧率")
                time.sleep(0.01)
                continue

            failures = 0
            frames += 1
            now = time.monotonic()
            if args.save_every > 0 and now - last_save >= args.save_every:
                print(f"已保存 {save_frame(frame, args.output)}")
                last_save = now

            if not args.headless:
                cv2.imshow("Nintendo Switch UVC (S=snapshot, Q=quit)", frame)
                key = cv2.waitKey(1) & 0xFF
                if key in (ord("q"), 27):
                    break
                if key == ord("s"):
                    print(f"已保存 {save_frame(frame, args.output)}")
    finally:
        capture.release()
        if not args.headless:
            cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        raise SystemExit(2)
