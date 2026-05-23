#!/usr/bin/env python3
"""实时绘制 ESP32-C5 串口输出的 CSI_FEATURE 数据。"""

from __future__ import annotations

import argparse
import csv
import queue
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path

from matplotlib.patches import FancyBboxPatch


ESP_SERIAL_KEYWORDS = (
    "usb serial",
    "usb-serial",
    "cp210",
    "ch340",
    "jtag",
    "uart",
    "esp",
)


@dataclass
class CsiFeature:
    """单行 CSI_FEATURE 的解析结果。

    调用方法：parse_csi_feature() 从串口文本行生成本结构，
    run_plot() 再把字段追加到曲线和 CSV。
    参数含义：
        frame: 帧号。
        rssi: 接收信号强度。
        channel: WiFi 信道。
        csi_len: 原始 CSI 字节长度。
        valid_points: 参与算法的有效点数。
        mean_amp/var_amp: 幅值均值和方差。
        delta_norm: 相邻帧归一化形状差异。
        baseline_delta: 当前帧与 baseline 的差异。
        smooth_mean_amp: 平滑幅值均值。
        motion_score/smooth_motion_score/window_score: 运动评分相关值。
        freeze_count: 数据冻结计数。
        motion_state: ESP 状态机输出。
    """

    frame: int
    rssi: int
    channel: int
    csi_len: int
    valid_points: int
    mean_amp: float
    var_amp: float
    delta_norm: float
    baseline_delta: float
    smooth_mean_amp: float
    motion_score: float
    smooth_motion_score: float
    window_score: float
    freeze_count: int
    motion_state: str
    agc_gain: int = 0
    fft_gain: int = 0
    compensate_gain: float = 1.0
    gain_compensated: bool = False


def parse_csi_feature(line: str) -> CsiFeature | None:
    """解析一行 CSI_FEATURE 串口文本。

    参数：
        line: 串口读取到的一整行文本。
    调用方法：serial_reader() 每收到一行文本后调用。
    返回值：解析成功返回 CsiFeature；不是 CSI_FEATURE 或格式错误时返回 None。
    """
    if line.startswith("CSI_FEATURE_RAW,"):
        pass
    elif line.startswith("CSI_FEATURE,"):
        pass
    else:
        return None

    parts = line.split(",")
    if len(parts) not in (16, 20):
        return None

    try:
        if len(parts) == 20:
            agc_gain = int(parts[15])
            fft_gain = int(parts[16])
            compensate_gain = float(parts[17])
            gain_compensated = parts[18].strip() == "1"
            motion_state = parts[19].strip()
        else:
            agc_gain = 0
            fft_gain = 0
            compensate_gain = 1.0
            gain_compensated = False
            motion_state = parts[15].strip()

        return CsiFeature(
            frame=int(parts[1]),
            rssi=int(parts[2]),
            channel=int(parts[3]),
            csi_len=int(parts[4]),
            valid_points=int(parts[5]),
            mean_amp=float(parts[6]),
            var_amp=float(parts[7]),
            delta_norm=float(parts[8]),
            baseline_delta=float(parts[9]),
            smooth_mean_amp=float(parts[10]),
            motion_score=float(parts[11]),
            smooth_motion_score=float(parts[12]),
            window_score=float(parts[13]),
            freeze_count=int(parts[14]),
            motion_state=motion_state,
            agc_gain=agc_gain,
            fft_gain=fft_gain,
            compensate_gain=compensate_gain,
            gain_compensated=gain_compensated,
        )
    except ValueError:
        return None


def serial_reader(port: str, baud: int, out_queue: queue.Queue[CsiFeature], stop_event: threading.Event) -> None:
    """串口读取线程函数。

    参数：
        port: 串口号，例如 COM5。
        baud: 串口波特率。
        out_queue: 输出队列，解析成功的 CsiFeature 会写入这里。
        stop_event: 停止事件，置位后退出循环。
    调用方法：run_plot() 创建后台线程时指定本函数为 target。
    """
    import serial

    with serial.Serial(port, baud, timeout=0.2) as ser:
        while not stop_event.is_set():
            raw_line = ser.readline()
            if not raw_line:
                continue

            line = raw_line.decode("utf-8", errors="ignore").strip()
            feature = parse_csi_feature(line)
            if feature is not None:
                out_queue.put(feature)


def find_serial_port() -> str:
    """自动查找 ESP32-C5 串口。

    参数：无。
    调用方法：run_plot() 在用户未指定 --port 时调用。
    返回值：找到的串口名；找不到或无法唯一确定时抛出 RuntimeError。
    """
    import serial.tools.list_ports

    ports = list(serial.tools.list_ports.comports())
    if not ports:
        raise RuntimeError("No serial ports found. Connect ESP32-C5 first.")

    for port in ports:
        text = f"{port.device} {port.description} {port.manufacturer or ''}".lower()
        if any(keyword in text for keyword in ESP_SERIAL_KEYWORDS):
            return port.device

    if len(ports) == 1:
        return ports[0].device

    available = ", ".join(f"{port.device}({port.description})" for port in ports)
    raise RuntimeError(f"Multiple serial ports found, please specify --port. Available: {available}")


def append_csv(csv_writer: csv.writer, feature: CsiFeature) -> None:
    """把一帧 CSI_FEATURE 追加写入 CSV。

    参数：
        csv_writer: 已打开 CSV 文件对应的 writer。
        feature: 要写入的一帧解析结果。
    调用方法：run_plot() 收到新帧且用户传入 --csv 时调用。
    """
    csv_writer.writerow([
        time.time(),
        feature.frame,
        feature.rssi,
        feature.channel,
        feature.csi_len,
        feature.valid_points,
        feature.mean_amp,
        feature.var_amp,
        feature.delta_norm,
        feature.baseline_delta,
        feature.smooth_mean_amp,
        feature.motion_score,
        feature.smooth_motion_score,
        feature.window_score,
        feature.freeze_count,
        feature.agc_gain,
        feature.fft_gain,
        feature.compensate_gain,
        int(feature.gain_compensated),
        feature.motion_state,
    ])


def draw_metric_card(ax, x: float, y: float, w: float, h: float, label: str, value: str, accent: str) -> None:
    """Draw a rounded metric card on an annotation axis."""
    shadow = FancyBboxPatch(
        (x + 0.008, y - 0.01),
        w,
        h,
        boxstyle="round,pad=0.012,rounding_size=0.035",
        linewidth=0,
        facecolor=(0.02, 0.04, 0.10, 0.18),
        transform=ax.transAxes,
        zorder=1,
    )
    card = FancyBboxPatch(
        (x, y),
        w,
        h,
        boxstyle="round,pad=0.012,rounding_size=0.035",
        linewidth=1.0,
        edgecolor=(1, 1, 1, 0.18),
        facecolor=(1, 1, 1, 0.10),
        transform=ax.transAxes,
        zorder=2,
    )
    glow = FancyBboxPatch(
        (x + 0.004, y + h - 0.018),
        w - 0.008,
        0.012,
        boxstyle="round,pad=0.004,rounding_size=0.02",
        linewidth=0,
        facecolor=accent,
        alpha=0.75,
        transform=ax.transAxes,
        zorder=3,
    )
    ax.add_patch(shadow)
    ax.add_patch(card)
    ax.add_patch(glow)
    ax.text(
        x + 0.03,
        y + h - 0.12,
        label,
        transform=ax.transAxes,
        color="#b7c3d7",
        fontsize=10,
        weight="medium",
        va="top",
        zorder=4,
    )
    ax.text(
        x + 0.03,
        y + 0.12,
        value,
        transform=ax.transAxes,
        color="white",
        fontsize=16,
        weight="bold",
        va="bottom",
        zorder=4,
    )


def style_plot_axis(ax, title: str) -> None:
    """Apply a soft glass-like visual style to the plot axes."""
    ax.set_facecolor((1, 1, 1, 0.08))
    for spine in ax.spines.values():
        spine.set_color((1, 1, 1, 0.16))
        spine.set_linewidth(1.0)
    ax.grid(True, color=(1, 1, 1, 0.10), linewidth=0.8)
    ax.tick_params(colors="#d6deeb", labelsize=9)
    ax.set_title(title, loc="left", color="white", fontsize=12, fontweight="bold", pad=12)


def run_plot(args: argparse.Namespace) -> None:
    """启动 matplotlib 实时曲线。

    参数：
        args: 命令行参数，包含 port、baud、window、interval、csv。
    调用方法：main() 解析命令行参数后调用。
    """
    import matplotlib.animation as animation
    import matplotlib.patheffects as pe
    import matplotlib.pyplot as plt

    port = args.port or find_serial_port()
    print(f"Using serial port: {port}, baud: {args.baud}")

    features: queue.Queue[CsiFeature] = queue.Queue()
    stop_event = threading.Event()
    reader = threading.Thread(
        target=serial_reader,
        args=(port, args.baud, features, stop_event),
        daemon=True,
    )
    reader.start()

    frames = deque(maxlen=args.window)
    delta_norm = deque(maxlen=args.window)
    baseline_delta = deque(maxlen=args.window)
    motion_score = deque(maxlen=args.window)
    smooth_motion_score = deque(maxlen=args.window)
    window_score = deque(maxlen=args.window)
    rssi = deque(maxlen=args.window)
    states = deque(maxlen=args.window)

    csv_file = None
    csv_writer = None
    if args.csv:
        csv_path = Path(args.csv)
        csv_path.parent.mkdir(parents=True, exist_ok=True)
        csv_file = csv_path.open("a", newline="", encoding="utf-8")
        csv_writer = csv.writer(csv_file)
        if csv_path.stat().st_size == 0:
            csv_writer.writerow([
                "time",
                "frame",
                "rssi",
                "channel",
                "csi_len",
                "valid_points",
                "mean_amp",
                "var_amp",
                "delta_norm",
                "baseline_delta",
                "smooth_mean_amp",
                "motion_score",
                "smooth_motion_score",
                "window_score",
                "freeze_count",
                "agc_gain",
                "fft_gain",
                "compensate_gain",
                "gain_compensated",
                "motion_state",
            ])

    plt.rcParams.update({
        "font.family": "DejaVu Sans",
        "axes.edgecolor": "#ffffff",
        "text.color": "white",
        "axes.labelcolor": "#d6deeb",
        "xtick.color": "#d6deeb",
        "ytick.color": "#d6deeb",
    })

    fig = plt.figure(figsize=(13, 8), facecolor="#09111f")
    gs = fig.add_gridspec(3, 1, height_ratios=[1.15, 3.2, 2.2], hspace=0.24)
    ax_header = fig.add_subplot(gs[0])
    ax_motion = fig.add_subplot(gs[1])
    ax_rssi = fig.add_subplot(gs[2], sharex=ax_motion)
    fig.canvas.manager.set_window_title("ESP32-C5 CSI realtime plot")
    fig.subplots_adjust(left=0.06, right=0.985, top=0.95, bottom=0.08)

    ax_header.set_axis_off()
    ax_header.set_facecolor("#09111f")

    header_bg = FancyBboxPatch(
        (0.0, 0.02),
        1.0,
        0.96,
        boxstyle="round,pad=0.02,rounding_size=0.05",
        linewidth=1.0,
        edgecolor=(1, 1, 1, 0.12),
        facecolor=(1, 1, 1, 0.06),
        transform=ax_header.transAxes,
        zorder=0,
    )
    ax_header.add_patch(header_bg)
    ax_header.text(0.03, 0.78, "ESP32-C5 CSI Monitor", fontsize=20, weight="bold", color="white", transform=ax_header.transAxes)
    ax_header.text(
        0.03,
        0.60,
        "Liquid Glass style realtime signal dashboard",
        fontsize=10,
        color="#9fb2cb",
        transform=ax_header.transAxes,
    )
    header_dynamic_artists = []
    last_feature: CsiFeature | None = None

    style_plot_axis(ax_motion, "Motion Scores")
    style_plot_axis(ax_rssi, "Signal Strength")

    colors = {
        "delta": "#7dd3fc",
        "baseline": "#fda4af",
        "score": "#86efac",
        "smooth": "#c4b5fd",
        "window": "#f9a8d4",
        "rssi": "#fca5a5",
    }

    (line_delta,) = ax_motion.plot([], [], label="delta_norm", linewidth=1.5, color=colors["delta"])
    (line_base,) = ax_motion.plot([], [], label="baseline_delta", linewidth=1.5, color=colors["baseline"])
    (line_score,) = ax_motion.plot([], [], label="motion_score", linewidth=1.8, color=colors["score"])
    (line_smooth,) = ax_motion.plot([], [], label="smooth_motion_score", linewidth=2.2, color=colors["smooth"])
    (line_window,) = ax_motion.plot([], [], label="window_score", linewidth=1.7, color=colors["window"])
    (line_rssi,) = ax_rssi.plot([], [], label="rssi", linewidth=2.0, color=colors["rssi"])

    glow = [pe.Stroke(linewidth=5.5, foreground=(1, 1, 1, 0.05)), pe.Normal()]
    for line in (line_delta, line_base, line_score, line_smooth, line_window, line_rssi):
        line.set_path_effects(glow)

    ax_motion.set_ylabel("CSI score")
    ax_motion.legend(
        loc="upper left",
        ncol=3,
        frameon=True,
        facecolor=(1, 1, 1, 0.08),
        edgecolor=(1, 1, 1, 0.10),
        labelcolor="#dfe7f5",
        fontsize=9,
    )
    ax_rssi.set_ylabel("RSSI dBm")
    ax_rssi.set_xlabel("frame")
    ax_rssi.legend(
        loc="upper left",
        frameon=True,
        facecolor=(1, 1, 1, 0.08),
        edgecolor=(1, 1, 1, 0.10),
        labelcolor="#dfe7f5",
        fontsize=9,
    )

    status_box = FancyBboxPatch(
        (0.72, 0.58),
        0.24,
        0.24,
        boxstyle="round,pad=0.015,rounding_size=0.04",
        linewidth=1.0,
        edgecolor=(1, 1, 1, 0.15),
        facecolor=(1, 1, 1, 0.08),
        transform=ax_motion.transAxes,
        zorder=1,
    )
    ax_motion.add_patch(status_box)
    status_text = ax_motion.text(
        0.74,
        0.72,
        "waiting",
        transform=ax_motion.transAxes,
        ha="left",
        va="center",
        fontsize=10,
        color="#f8fafc",
        zorder=2,
    )

    def update(_: int):
        """刷新 matplotlib 曲线。

        参数：
            _: matplotlib 动画帧序号，本函数不使用。
        调用方法：FuncAnimation 按 args.interval 周期自动调用。
        """
        nonlocal last_feature
        latest = None
        while True:
            try:
                latest = features.get_nowait()
            except queue.Empty:
                break

            frames.append(latest.frame)
            delta_norm.append(latest.delta_norm)
            baseline_delta.append(latest.baseline_delta)
            motion_score.append(latest.motion_score)
            smooth_motion_score.append(latest.smooth_motion_score)
            window_score.append(latest.window_score)
            rssi.append(latest.rssi)
            states.append(latest.motion_state)
            last_feature = latest

            if csv_writer is not None:
                append_csv(csv_writer, latest)

        if csv_file is not None:
            csv_file.flush()

        while header_dynamic_artists:
            artist = header_dynamic_artists.pop()
            artist.remove()

        if not frames:
            count_before = len(ax_header.patches), len(ax_header.texts)
            draw_metric_card(ax_header, 0.33, 0.18, 0.13, 0.56, "Frame", "--", "#7dd3fc")
            draw_metric_card(ax_header, 0.48, 0.18, 0.13, 0.56, "State", "waiting", "#fda4af")
            draw_metric_card(ax_header, 0.63, 0.18, 0.13, 0.56, "RSSI", "--", "#86efac")
            draw_metric_card(ax_header, 0.78, 0.18, 0.17, 0.56, "Gain", "x1.000", "#c4b5fd")
            header_dynamic_artists.extend(ax_header.patches[count_before[0]:])
            header_dynamic_artists.extend(ax_header.texts[count_before[1]:])
            return line_delta, line_base, line_score, line_smooth, line_window, line_rssi, status_text

        x = list(frames)
        line_delta.set_data(x, list(delta_norm))
        line_base.set_data(x, list(baseline_delta))
        line_score.set_data(x, list(motion_score))
        line_smooth.set_data(x, list(smooth_motion_score))
        line_window.set_data(x, list(window_score))
        line_rssi.set_data(x, list(rssi))

        ax_motion.relim()
        ax_motion.autoscale_view()
        ax_rssi.relim()
        ax_rssi.autoscale_view()

        latest_state = states[-1]
        state_color = "#fda4af" if latest_state == "motion" else "#86efac"
        status_text.set_text(
            f"live  frame {frames[-1]}\nstate {latest_state}\nrssi {rssi[-1]} dBm"
        )
        status_text.set_color(state_color)

        count_before = len(ax_header.patches), len(ax_header.texts)
        draw_metric_card(ax_header, 0.33, 0.18, 0.13, 0.56, "Frame", str(frames[-1]), "#7dd3fc")
        draw_metric_card(ax_header, 0.48, 0.18, 0.13, 0.56, "State", latest_state, state_color)
        draw_metric_card(ax_header, 0.63, 0.18, 0.13, 0.56, "RSSI", f"{rssi[-1]} dBm", "#86efac")
        gain_value = f"x{last_feature.compensate_gain:.3f}" if last_feature is not None else "x1.000"
        draw_metric_card(ax_header, 0.78, 0.18, 0.17, 0.56, "Gain", gain_value, "#c4b5fd")
        header_dynamic_artists.extend(ax_header.patches[count_before[0]:])
        header_dynamic_artists.extend(ax_header.texts[count_before[1]:])

        return line_delta, line_base, line_score, line_smooth, line_window, line_rssi, status_text

    ani = animation.FuncAnimation(fig, update, interval=args.interval, blit=False, cache_frame_data=False)

    try:
        plt.show()
    finally:
        stop_event.set()
        reader.join(timeout=1.0)
        if csv_file is not None:
            csv_file.close()
        _ = ani


def main() -> None:
    """matplotlib 版 CSI 实时曲线工具入口。

    参数：无，函数内部读取命令行参数。
    调用方法：直接运行 `python tools/plot_csi_serial.py`。
    """
    parser = argparse.ArgumentParser(description="实时绘制 ESP32-C5 串口输出的 CSI_FEATURE 数据。")
    parser.add_argument("--port", help="串口号，例如 COM5；不填写时自动检测。")
    parser.add_argument("--baud", type=int, default=115200, help="串口波特率。")
    parser.add_argument("--window", type=int, default=300, help="屏幕上保留的最近 CSI 帧数量。")
    parser.add_argument("--interval", type=int, default=100, help="曲线刷新间隔，单位 ms。")
    parser.add_argument("--csv", help="可选：保存解析后 CSI_FEATURE 行的 CSV 路径。")
    args = parser.parse_args()

    run_plot(args)


if __name__ == "__main__":
    main()
