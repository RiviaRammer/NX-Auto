#!/usr/bin/env python3
"""Detect and track people in Nintendo Switch UVC frames with Ultralytics YOLO."""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import cv2
from ultralytics import YOLO

from ns_capture import backend_for, fourcc_text, parse_device, save_frame

WINDOW_NAME = "NS person detection (S=snapshot, Q=quit)"


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="在 NS 采集画面中检测并跟踪 person（COCO 类别 0）"
    )
    parser.add_argument("--source", default="/dev/video2", help="摄像头索引或设备路径")
    parser.add_argument(
        "--backend",
        choices=("auto", "dshow", "msmf", "v4l2", "avfoundation"),
        default="v4l2",
    )
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--fourcc", default="MJPG")
    parser.add_argument(
        "--model",
        default="models/yolo11n.pt",
        help="Ultralytics 权重；可换成 yolo26n.pt 或自训练 best.pt",
    )
    parser.add_argument("--confidence", type=float, default=0.35)
    parser.add_argument("--imgsz", type=int, default=640, help="YOLO 推理输入尺寸")
    parser.add_argument(
        "--classes",
        default="0",
        help="保留的类别 ID，逗号分隔；官方 COCO 的 person=0，all 表示全部",
    )
    parser.add_argument(
        "--compute-device",
        default=None,
        help="推理设备，例如 cpu、0；留空由 Ultralytics 自动选择",
    )
    parser.add_argument(
        "--tracker",
        default="bytetrack.yaml",
        help="跟踪配置，例如 bytetrack.yaml 或 botsort.yaml",
    )
    parser.add_argument("--no-track", action="store_true", help="只检测，不维护人物 ID")
    parser.add_argument("--output", type=Path, default=Path("captures"))
    parser.add_argument("--record", type=Path, help="把带框画面录制为 MP4")
    return parser


def parse_classes(value: str) -> list[int] | None:
    if value.lower() == "all":
        return None
    try:
        class_ids = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as exc:
        raise ValueError("--classes 应为逗号分隔的非负整数，或 all") from exc
    if not class_ids or any(class_id < 0 for class_id in class_ids):
        raise ValueError("--classes 应为逗号分隔的非负整数，或 all")
    return class_ids


def open_source(args: argparse.Namespace) -> cv2.VideoCapture:
    if len(args.fourcc) != 4:
        raise ValueError("--fourcc 必须恰好为 4 个字符")
    capture = cv2.VideoCapture(parse_device(args.source), backend_for(args.backend))
    if not capture.isOpened():
        raise RuntimeError(f"无法打开视频设备 {args.source!r}")
    capture.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*args.fourcc))
    capture.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    capture.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    capture.set(cv2.CAP_PROP_FPS, args.fps)
    capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    return capture


def make_writer(path: Path, width: int, height: int, fps: float) -> cv2.VideoWriter:
    path.parent.mkdir(parents=True, exist_ok=True)
    writer = cv2.VideoWriter(
        os.fspath(path), cv2.VideoWriter_fourcc(*"mp4v"), fps, (width, height)
    )
    if not writer.isOpened():
        raise RuntimeError(f"无法创建录像文件 {path}")
    return writer


def window_is_open() -> bool:
    try:
        return cv2.getWindowProperty(WINDOW_NAME, cv2.WND_PROP_VISIBLE) >= 1
    except cv2.error:
        return False


def main() -> int:
    args = make_parser().parse_args()
    if args.width <= 0 or args.height <= 0 or args.fps <= 0 or args.imgsz <= 0:
        raise ValueError("分辨率、帧率和 --imgsz 必须大于 0")
    if not 0.0 <= args.confidence <= 1.0:
        raise ValueError("--confidence 必须在 0 到 1 之间")
    class_ids = parse_classes(args.classes)

    capture = open_source(args)
    actual_width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    actual_fps = capture.get(cv2.CAP_PROP_FPS) or args.fps
    actual_fourcc = fourcc_text(capture.get(cv2.CAP_PROP_FOURCC))
    print(
        f"视频: {actual_width}x{actual_height}@{actual_fps:.2f}, "
        f"FourCC={actual_fourcc!r}"
    )
    print(f"加载模型: {args.model}")
    model = YOLO(args.model)
    writer = (
        make_writer(args.record, actual_width, actual_height, actual_fps)
        if args.record
        else None
    )

    smoothed_fps = 0.0
    previous = time.perf_counter()
    failures = 0
    cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
    try:
        while True:
            if not window_is_open():
                break
            ok, frame = capture.read()
            if not ok or frame is None:
                failures += 1
                if failures >= 30:
                    raise RuntimeError("连续 30 次取帧失败，请重新插拔采集卡")
                continue
            failures = 0

            predict_args = {
                "conf": args.confidence,
                "imgsz": args.imgsz,
                "verbose": False,
            }
            if class_ids is not None:
                predict_args["classes"] = class_ids
            if args.compute_device:
                predict_args["device"] = args.compute_device

            if args.no_track:
                result = model.predict(frame, **predict_args)[0]
            else:
                result = model.track(
                    frame,
                    persist=True,
                    tracker=args.tracker,
                    **predict_args,
                )[0]

            annotated = result.plot()
            people = len(result.boxes) if result.boxes is not None else 0
            count_label = "people" if class_ids == [0] else "detections"
            now = time.perf_counter()
            instant_fps = 1.0 / max(now - previous, 1e-6)
            previous = now
            smoothed_fps = (
                instant_fps if smoothed_fps == 0.0 else smoothed_fps * 0.9 + instant_fps * 0.1
            )
            cv2.putText(
                annotated,
                f"{count_label}: {people}  inference loop: {smoothed_fps:.1f} fps",
                (20, 40),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.9,
                (0, 255, 255),
                2,
                cv2.LINE_AA,
            )

            if writer is not None:
                writer.write(annotated)
            cv2.imshow(WINDOW_NAME, annotated)
            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):
                break
            if not window_is_open():
                break
            if key == ord("s"):
                print(f"已保存 {save_frame(annotated, args.output)}")
    finally:
        capture.release()
        if writer is not None:
            writer.release()
        cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        raise SystemExit(2)
