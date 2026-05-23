#!/usr/bin/env python3
"""在浏览器中实时绘制 ESP32-C5 串口输出的 CSI_FEATURE 数据。"""

from __future__ import annotations

import argparse
import json
import queue
import threading
import time
import webbrowser
from dataclasses import asdict, dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ESP_SERIAL_KEYWORDS = ("usb", "serial", "jtag", "uart", "cp210", "ch340", "esp")
SKIP_SERIAL_KEYWORDS = ("bluetooth", "蓝牙")

CSI_STATE_TEXT = {
    0: "unknown",
    1: "static",
    2: "active",
    3: "motion",
    4: "offset",
    5: "gain_freeze",
    6: "data_frozen",
    7: "recover",
}
CSI_STATE_ID_BY_TEXT = {value: key for key, value in CSI_STATE_TEXT.items()}


@dataclass
class CsiFeature:
    source: str
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
    decision_state_id: int
    decision_state_text: str
    agc_gain: int = 0
    fft_gain: int = 0
    compensate_gain: float = 1.0
    gain_compensated: bool = False
    decision_mode: int = 0
    global_norm_score: float = 0.0
    global_state_id: int = 0
    global_state_text: str = "unknown"
    subband_norm_score: float = 0.0
    subband_state_id: int = 0
    subband_state_text: str = "unknown"
    fusion_score: float = 0.0
    fusion_state_id: int = 0
    fusion_state_text: str = "unknown"
    decision_score: float = 0.0
    subband_count: int = 0
    best_band: int = -1
    bands: list[dict] | None = None


@dataclass
class CsiDebug:
    frame: int
    agc_gain: int
    fft_gain: int
    compensate_gain: float
    delta_norm: float
    motion_score: float
    baseline_delta: float
    amp_motion: float = 0.0


class PlotState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.clients: list[queue.Queue[dict]] = []
        self.status = "waiting for serial port"
        self.port = ""

    def publish(self, item: dict) -> None:
        with self.lock:
            for client in list(self.clients):
                try:
                    client.put_nowait(item)
                except queue.Full:
                    pass

    def add_client(self) -> queue.Queue[dict]:
        client: queue.Queue[dict] = queue.Queue(maxsize=200)
        with self.lock:
            self.clients.append(client)
            client.put_nowait({"type": "status", "status": self.status, "port": self.port})
        return client

    def remove_client(self, client: queue.Queue[dict]) -> None:
        with self.lock:
            if client in self.clients:
                self.clients.remove(client)

    def set_status(self, status: str, port: str = "") -> None:
        self.status = status
        self.port = port
        self.publish({"type": "status", "status": status, "port": port})


def parse_state_id(value: str) -> int:
    try:
        return int(value)
    except ValueError:
        return CSI_STATE_ID_BY_TEXT.get(value.strip(), 0)


def state_text_from_id(state_id: int) -> str:
    return CSI_STATE_TEXT.get(state_id, "unknown")


def parse_csi_feature(line: str) -> CsiFeature | None:
    if line.startswith("CSI_FEATURE_RAW,"):
        source = "raw"
    elif line.startswith("CSI_FEATURE,"):
        source = "raw"
    else:
        return None

    parts = line.split(",")
    if len(parts) < 16:
        return None

    try:
        if len(parts) >= 20:
            agc_gain = int(parts[15])
            fft_gain = int(parts[16])
            compensate_gain = float(parts[17])
            gain_compensated = parts[18].strip() == "1"
            decision_state_id = parse_state_id(parts[19].strip())
        else:
            agc_gain = 0
            fft_gain = 0
            compensate_gain = 1.0
            gain_compensated = False
            decision_state_id = parse_state_id(parts[15].strip())

        decision_mode = 0
        global_norm_score = 0.0
        global_state_id = 0
        subband_norm_score = 0.0
        subband_state_id = 0
        fusion_score = 0.0
        fusion_state_id = 0
        decision_score = 0.0
        subband_count = 0
        best_band = -1
        bands = [
            {"delta": 0.0, "base": 0.0, "score": 0.0, "state_id": 0, "state_text": "unknown"}
            for _ in range(4)
        ]

        if len(parts) >= 47:
            decision_mode = int(parts[20])
            global_norm_score = float(parts[21])
            global_state_id = parse_state_id(parts[22])
            subband_norm_score = float(parts[23])
            subband_state_id = parse_state_id(parts[24])
            fusion_score = float(parts[25])
            fusion_state_id = parse_state_id(parts[26])
            decision_score = float(parts[27])
            decision_state_id = parse_state_id(parts[28])
            subband_count = int(parts[29])
            best_band = int(parts[30])
            index = 31
            for band_index in range(4):
                state_id = parse_state_id(parts[index + 3])
                bands[band_index] = {
                    "delta": float(parts[index]),
                    "base": float(parts[index + 1]),
                    "score": float(parts[index + 2]),
                    "state_id": state_id,
                    "state_text": state_text_from_id(state_id),
                }
                index += 4

        decision_state_text = state_text_from_id(decision_state_id)
        return CsiFeature(
            source=source,
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
            motion_state=decision_state_text,
            decision_state_id=decision_state_id,
            decision_state_text=decision_state_text,
            agc_gain=agc_gain,
            fft_gain=fft_gain,
            compensate_gain=compensate_gain,
            gain_compensated=gain_compensated,
            decision_mode=decision_mode,
            global_norm_score=global_norm_score,
            global_state_id=global_state_id,
            global_state_text=state_text_from_id(global_state_id),
            subband_norm_score=subband_norm_score,
            subband_state_id=subband_state_id,
            subband_state_text=state_text_from_id(subband_state_id),
            fusion_score=fusion_score,
            fusion_state_id=fusion_state_id,
            fusion_state_text=state_text_from_id(fusion_state_id),
            decision_score=decision_score,
            subband_count=subband_count,
            best_band=best_band,
            bands=bands,
        )
    except (ValueError, IndexError):
        return None


def parse_csi_dbg(line: str) -> CsiDebug | None:
    if not line.startswith("CSI_DBG,"):
        return None

    parts = line.split(",")
    if len(parts) not in (8, 9):
        return None

    try:
        return CsiDebug(
            frame=int(parts[1]),
            agc_gain=int(parts[2]),
            fft_gain=int(parts[3]),
            compensate_gain=float(parts[4]),
            delta_norm=float(parts[5]),
            motion_score=float(parts[6]),
            baseline_delta=float(parts[7]),
            amp_motion=float(parts[8]) if len(parts) == 9 else 0.0,
        )
    except ValueError:
        return None


def parse_csi_link(line: str) -> dict | None:
    if not line.startswith("CSI_LINK,"):
        return None

    item: dict[str, str] = {"type": "link"}
    for part in line.split(",")[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        item[key.strip()] = value.strip()
    return item


def find_serial_port() -> str | None:
    import serial.tools.list_ports

    ports = list(serial.tools.list_ports.comports())
    candidates = []
    for port in ports:
        text = f"{port.device} {port.description} {port.manufacturer or ''}".lower()
        if any(skip in text for skip in SKIP_SERIAL_KEYWORDS):
            continue
        if any(keyword in text for keyword in ESP_SERIAL_KEYWORDS):
            candidates.append(port.device)

    if candidates:
        return candidates[0]

    non_bluetooth = []
    for port in ports:
        text = f"{port.device} {port.description}".lower()
        if not any(skip in text for skip in SKIP_SERIAL_KEYWORDS):
            non_bluetooth.append(port.device)
    return non_bluetooth[0] if len(non_bluetooth) == 1 else None


def serial_loop(args: argparse.Namespace, state: PlotState, stop_event: threading.Event) -> None:
    import serial

    while not stop_event.is_set():
        port = args.port or find_serial_port()
        if not port:
            state.set_status("waiting for ESP32-C5 serial port")
            time.sleep(1.0)
            continue

        try:
            state.set_status("opening serial", port)
            with serial.Serial(port, args.baud, timeout=0.2) as ser:
                state.set_status("receiving CSI_FEATURE", port)
                while not stop_event.is_set():
                    raw_line = ser.readline()
                    if not raw_line:
                        continue

                    line = raw_line.decode("utf-8", errors="ignore").strip()
                    link = parse_csi_link(line)
                    if link is not None:
                        state.publish(link)
                        continue

                    debug = parse_csi_dbg(line)
                    if debug is not None:
                        item = asdict(debug)
                        item["type"] = "debug"
                        state.publish(item)
                        continue

                    feature = parse_csi_feature(line)
                    if feature is None:
                        continue

                    item = asdict(feature)
                    item["type"] = "feature"
                    state.publish(item)
        except serial.SerialException as exc:
            state.set_status(f"serial error: {exc}", port)
            time.sleep(1.0)


HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP32-C5 CSI 实时曲线</title>
  <style>
    :root {
      --bg: #f4f6f8;
      --panel: #ffffff;
      --panel-2: #f7f9fb;
      --ink: #17202d;
      --muted: #607086;
      --line: #d8e0e8;
      --line-soft: #e9eef3;
      --header: #121822;
      --accent: #0f766e;
      --danger: #b91c1c;
      --ok: #166534;
      --warn: #a16207;
      --recover: #1d4ed8;
      --freeze: #6d28d9;
    }
    * { box-sizing: border-box; }
    html, body {
      width: 100%;
      max-width: 100%;
      margin: 0;
      min-height: 100%;
      font-family: "Segoe UI", Arial, sans-serif;
      background: var(--bg);
      color: var(--ink);
      overflow-x: hidden;
    }
    body { overflow-y: auto; }
    header {
      position: sticky;
      top: 0;
      z-index: 5;
      width: 100%;
      max-width: 100vw;
      min-height: 58px;
      display: grid;
      grid-template-columns: minmax(190px, 260px) minmax(0, 1fr);
      align-items: center;
      gap: 20px;
      padding: 10px 22px;
      background: var(--header);
      color: white;
      box-shadow: 0 2px 14px rgba(15, 23, 42, 0.18);
    }
    #appTitle { min-width: 0; font-size: 18px; font-weight: 750; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .header-right {
      min-width: 0;
      display: grid;
      grid-template-columns: minmax(150px, 0.75fr) minmax(200px, 1fr) minmax(220px, 1fr) 92px minmax(210px, 0.9fr);
      align-items: center;
      gap: 8px;
    }
    button {
      height: 34px;
      border: 1px solid rgba(255,255,255,0.18);
      border-radius: 6px;
      background: #ffffff;
      color: #17202d;
      cursor: pointer;
      font-size: 13px;
      font-weight: 650;
    }
    button:hover { background: #e9f4f2; border-color: #99d4ca; }
    button.paused { background: #fff7ed; border-color: #fed7aa; color: #9a3412; }
    .chip {
      min-width: 0;
      height: 34px;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 7px;
      padding: 0 16px;
      border: 1px solid rgba(255,255,255,0.10);
      border-radius: 6px;
      font-size: 13px;
      line-height: 1.1;
      text-align: center;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      font-variant-numeric: tabular-nums;
    }
    .chip::before {
      content: "";
      flex: 0 0 7px;
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: #94a3b8;
      box-shadow: 0 0 0 3px rgba(148, 163, 184, 0.18);
    }
    #status { justify-content: flex-start; background: transparent; color: #d8dee8; font-size: 12px; opacity: 0.98; border-color: transparent; }
    #status::before { background: #14b8a6; box-shadow: 0 0 0 3px rgba(20, 184, 166, 0.20); }
    #wifiName { background: #223044; color: white; }
    #wifiName::before { background: #38bdf8; box-shadow: 0 0 0 3px rgba(56, 189, 248, 0.16); }
    #humanState { background: #2f3d52; color: white; }
    #humanState.motion { background: var(--danger); }
    #humanState.static { background: var(--ok); }
    #humanState.active { background: #b45309; }
    #humanState.offset { background: #0f766e; }
    #humanState.recover { background: var(--recover); }
    #humanState.data_frozen { background: var(--freeze); }
    #humanState.gain_freeze { background: var(--warn); }
    #humanState.unknown { background: #475467; }
    #gainState { background: var(--warn); color: white; }
    #gainState.ready { background: var(--ok); }
    #gainState.collect { background: var(--warn); }
    main {
      min-height: calc(100vh - 58px);
      min-width: 0;
      display: grid;
      grid-template-rows: auto minmax(380px, 42vh) minmax(340px, 38vh) minmax(280px, 30vh) minmax(260px, 28vh) minmax(320px, 34vh);
      gap: 12px;
      padding: 12px;
      overflow: visible;
    }
    #metricValues {
      display: grid;
      grid-template-columns: repeat(7, minmax(0, 1fr));
      gap: 8px;
      min-height: 86px;
      padding: 0;
      font-size: 11px;
      overflow: visible;
    }
    .metric {
      position: relative;
      min-width: 0;
      display: grid;
      grid-template-rows: 18px 1fr;
      gap: 3px;
      padding: 8px 9px 7px;
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      box-shadow: 0 1px 3px rgba(15, 23, 42, 0.05);
      overflow: hidden;
    }
    .metric::before {
      content: "";
      position: absolute;
      inset: 0 auto 0 0;
      width: 3px;
      background: var(--accent);
      opacity: 0.82;
    }
    .metric span:first-child { color: var(--muted); overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
    .metric span:last-child {
      align-self: end;
      font-size: 16px;
      font-weight: 750;
      font-variant-numeric: tabular-nums;
      color: #111827;
      overflow: hidden;
      white-space: nowrap;
      text-overflow: ellipsis;
    }
    .plot-panel {
      min-height: 0;
      min-width: 0;
      display: grid;
      grid-template-rows: minmax(0, 1fr) 28px 34px;
      border: 1px solid var(--line);
      border-radius: 8px;
      background: var(--panel);
      box-shadow: 0 1px 3px rgba(15, 23, 42, 0.06);
      overflow: hidden;
    }
    .canvas-scroll {
      width: 100%;
      max-width: 100%;
      min-width: 0;
      min-height: 0;
      overflow-x: auto;
      overflow-y: hidden;
      overscroll-behavior-x: contain;
    }
    canvas {
      display: block;
      width: 100%;
      min-width: 1280px;
      height: 100%;
      background: var(--panel);
    }
    .plot-readout {
      min-width: 0;
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 0 12px;
      border-top: 1px solid var(--line-soft);
      background: #ffffff;
      color: #344054;
      font-size: 12px;
      font-variant-numeric: tabular-nums;
      white-space: nowrap;
      overflow-x: auto;
    }
    .readout-text { min-width: 0; flex: 1 1 auto; overflow: hidden; text-overflow: ellipsis; }
    .axis-control {
      flex: 0 0 auto;
      display: inline-flex;
      align-items: center;
      gap: 5px;
      color: var(--muted);
      font-size: 11px;
    }
    .axis-control input {
      width: 62px;
      height: 22px;
      padding: 0 5px;
      border: 1px solid var(--line);
      border-radius: 5px;
      background: #f8fafc;
      color: var(--ink);
      font-size: 11px;
      font-variant-numeric: tabular-nums;
    }
    .legend {
      min-width: 0;
      display: flex;
      gap: 12px;
      align-items: center;
      padding: 0 12px;
      border-top: 1px solid var(--line-soft);
      background: var(--panel-2);
      font-size: 12px;
      white-space: nowrap;
      overflow-x: auto;
    }
    .legend-title { flex: 0 0 100px; font-weight: 750; color: #111827; }
    .item { flex: 0 0 auto; color: #405066; }
    .item::before {
      content: "";
      display: inline-block;
      width: 18px;
      height: 3px;
      margin-right: 6px;
      border-radius: 999px;
      vertical-align: middle;
      background: var(--c);
    }
    .band-grid {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 10px;
      padding: 12px;
      align-content: start;
    }
    .band-card {
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 10px;
      background: var(--panel-2);
      min-height: 92px;
    }
    .band-title {
      display: flex;
      justify-content: space-between;
      gap: 8px;
      font-weight: 700;
      color: var(--ink);
      margin-bottom: 8px;
    }
    .band-detail {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 6px;
      color: var(--muted);
      font-size: 12px;
    }
    @media (max-width: 1200px) {
      header { grid-template-columns: 170px minmax(0, 1fr); padding-left: 16px; padding-right: 16px; }
      .header-right { grid-template-columns: minmax(120px, 0.8fr) minmax(130px, 1fr) minmax(130px, 1fr) 80px minmax(120px, 0.8fr); gap: 6px; }
      .chip { font-size: 12px; padding: 0 7px; }
      .legend { gap: 10px; font-size: 11px; }
      .legend-title { flex-basis: 86px; }
      #metricValues { grid-template-columns: repeat(5, minmax(0, 1fr)); }
    }
    @media (max-width: 760px) {
      header { position: static; grid-template-columns: 1fr; gap: 8px; }
      .header-right { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      #status { grid-column: 1 / -1; }
      main { grid-template-rows: auto; grid-auto-rows: 320px; padding: 8px; gap: 8px; }
      canvas { min-width: 960px; }
      #metricValues { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .band-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    }
  </style>
</head>
<body>
  <header>
    <div id="appTitle">ESP32-C5 CSI</div>
    <div class="header-right">
      <div id="wifiName" class="chip">WiFi 等待中</div>
      <div id="humanState" class="chip">状态 等待中</div>
      <div id="gainState" class="chip">增益 等待中</div>
      <button id="pauseBtn" type="button">暂停</button>
      <div id="status" class="chip">connecting...</div>
    </div>
  </header>
  <main>
    <div id="metricValues">
      <div class="metric"><span>frame 帧号</span><span id="vFrame">--</span></div>
      <div class="metric"><span>RSSI dBm</span><span id="vRssi">--</span></div>
      <div class="metric"><span>state 状态</span><span id="vState">--</span></div>
      <div class="metric"><span>delta</span><span id="vDeltaNorm">--</span></div>
      <div class="metric"><span>baseline</span><span id="vBaselineDelta">--</span></div>
      <div class="metric"><span>motion</span><span id="vMotionScore">--</span></div>
      <div class="metric"><span>smooth</span><span id="vSmoothMotionScore">--</span></div>
      <div class="metric"><span>AGC</span><span id="vAgcGain">--</span></div>
      <div class="metric"><span>FFT</span><span id="vFftGain">--</span></div>
      <div class="metric"><span>comp</span><span id="vCompensateGain">--</span></div>
      <div class="metric"><span>decision mode</span><span id="vDecisionMode">--</span></div>
      <div class="metric"><span>decision score</span><span id="vDecisionScore">--</span></div>
      <div class="metric"><span>global score</span><span id="vGlobalScore">--</span></div>
      <div class="metric"><span>global state</span><span id="vGlobalState">--</span></div>
      <div class="metric"><span>subband score</span><span id="vSubbandScore">--</span></div>
      <div class="metric"><span>subband state</span><span id="vSubbandState">--</span></div>
      <div class="metric"><span>fusion score</span><span id="vFusionScore">--</span></div>
      <div class="metric"><span>fusion state</span><span id="vFusionState">--</span></div>
      <div class="metric"><span>best band</span><span id="vBestBand">--</span></div>
    </div>
    <div class="plot-panel">
      <div class="canvas-scroll"><canvas id="rawScore"></canvas></div>
      <div class="plot-readout">
        <span id="rawReadout" class="readout-text">waiting for RAW...</span>
        <label class="axis-control">横轴缩放 <input id="rawXScale" type="number" min="0.5" max="40" step="0.5" value="4"></label>
      </div>
      <div class="legend">
        <span class="legend-title">RAW 特征</span>
        <span class="item" style="--c:#1f77b4">delta</span>
        <span class="item" style="--c:#ff7f0e">baseline</span>
        <span class="item" style="--c:#2ca02c">motion</span>
        <span class="item" style="--c:#9467bd">smooth</span>
      </div>
    </div>
    <div class="plot-panel">
      <div class="canvas-scroll"><canvas id="gainDebug"></canvas></div>
      <div class="plot-readout">
        <span id="debugReadout" class="readout-text">waiting for CSI_DBG...</span>
        <label class="axis-control">横轴缩放 <input id="debugXScale" type="number" min="0.5" max="40" step="0.5" value="4"></label>
      </div>
      <div class="legend">
        <span class="legend-title">DBG 诊断</span>
        <span class="item" style="--c:#2ca02c">motion</span>
        <span class="item" style="--c:#ff7f0e">baseline</span>
        <span class="item" style="--c:#17becf">comp</span>
        <span class="item" style="--c:#d62728">agc</span>
        <span class="item" style="--c:#9467bd">fft</span>
      </div>
    </div>
    <div class="plot-panel">
      <div class="canvas-scroll"><canvas id="rssiPlot"></canvas></div>
      <div class="plot-readout">
        <span id="rssiReadout" class="readout-text">waiting for RSSI...</span>
        <label class="axis-control">横轴缩放 <input id="rssiXScale" type="number" min="0.5" max="40" step="0.5" value="4"></label>
      </div>
      <div class="legend">
        <span class="legend-title">RSSI 信号</span>
        <span class="item" style="--c:#d62728">rssi dBm</span>
      </div>
    </div>
    <div class="plot-panel">
      <div class="canvas-scroll"><div id="bandCards" class="band-grid"></div></div>
      <div class="plot-readout"><span id="subbandStatus" class="readout-text">subband disabled</span></div>
      <div class="legend">
        <span class="legend-title">频段状态</span>
        <span class="item" style="--c:rgba(22, 101, 52, 0.18)">static</span>
        <span class="item" style="--c:rgba(245, 158, 11, 0.16)">active</span>
        <span class="item" style="--c:rgba(185, 28, 28, 0.18)">motion</span>
        <span class="item" style="--c:rgba(20, 184, 166, 0.16)">offset</span>
      </div>
    </div>
    <div class="plot-panel">
      <div class="canvas-scroll"><canvas id="bandScorePlot"></canvas></div>
      <div class="plot-readout">
        <span id="bandReadout" class="readout-text">subband disabled</span>
        <label class="axis-control">横轴缩放 <input id="bandXScale" type="number" min="0.5" max="40" step="0.5" value="4"></label>
      </div>
      <div class="legend">
        <span class="legend-title">频段得分</span>
        <span class="item" style="--c:#2563eb">band0</span>
        <span class="item" style="--c:#f97316">band1</span>
        <span class="item" style="--c:#16a34a">band2</span>
        <span class="item" style="--c:#9333ea">band3</span>
        <span class="item" style="--c:#dc2626">subband</span>
      </div>
    </div>
  </main>
  <script>
    const maxPoints = __MAX_POINTS__;
    const smoothSamples = __SMOOTH_SAMPLES__;
    const scoreMaxOverride = __SCORE_MAX__;
    const rawData = [];
    const debugData = [];
    let paused = false;

    const statusEl = document.getElementById("status");
    const wifiNameEl = document.getElementById("wifiName");
    const humanStateEl = document.getElementById("humanState");
    const gainStateEl = document.getElementById("gainState");
    const pauseBtn = document.getElementById("pauseBtn");
    const rawScoreCanvas = document.getElementById("rawScore");
    const gainDebugCanvas = document.getElementById("gainDebug");
    const rssiCanvas = document.getElementById("rssiPlot");
    const bandScoreCanvas = document.getElementById("bandScorePlot");
    const rawReadoutEl = document.getElementById("rawReadout");
    const debugReadoutEl = document.getElementById("debugReadout");
    const rssiReadoutEl = document.getElementById("rssiReadout");
    const bandReadoutEl = document.getElementById("bandReadout");
    const bandCardsEl = document.getElementById("bandCards");
    const subbandStatusEl = document.getElementById("subbandStatus");
    const rawXScaleEl = document.getElementById("rawXScale");
    const debugXScaleEl = document.getElementById("debugXScale");
    const rssiXScaleEl = document.getElementById("rssiXScale");
    const bandXScaleEl = document.getElementById("bandXScale");
    const metricEls = {
      frame: document.getElementById("vFrame"),
      rssi: document.getElementById("vRssi"),
      state: document.getElementById("vState"),
      deltaNorm: document.getElementById("vDeltaNorm"),
      baselineDelta: document.getElementById("vBaselineDelta"),
      motionScore: document.getElementById("vMotionScore"),
      smoothMotionScore: document.getElementById("vSmoothMotionScore"),
      agcGain: document.getElementById("vAgcGain"),
      fftGain: document.getElementById("vFftGain"),
      compensateGain: document.getElementById("vCompensateGain"),
      decisionMode: document.getElementById("vDecisionMode"),
      decisionScore: document.getElementById("vDecisionScore"),
      globalScore: document.getElementById("vGlobalScore"),
      globalState: document.getElementById("vGlobalState"),
      subbandScore: document.getElementById("vSubbandScore"),
      subbandState: document.getElementById("vSubbandState"),
      fusionScore: document.getElementById("vFusionScore"),
      fusionState: document.getElementById("vFusionState"),
      bestBand: document.getElementById("vBestBand"),
    };
    const STATE_STYLES = {
      motion: {shortLabel: "motion", chipClass: "motion", fillColor: "rgba(185, 28, 28, 0.18)", textColor: "#c1121f"},
      static: {shortLabel: "static", chipClass: "static", fillColor: "rgba(22, 101, 52, 0.18)", textColor: "#166534"},
      active: {shortLabel: "active", chipClass: "active", fillColor: "rgba(245, 158, 11, 0.16)", textColor: "#b45309"},
      offset: {shortLabel: "offset", chipClass: "offset", fillColor: "rgba(20, 184, 166, 0.16)", textColor: "#0f766e"},
      recover: {shortLabel: "recover", chipClass: "recover", fillColor: "rgba(29, 78, 216, 0.18)", textColor: "#1d4ed8"},
      gain_freeze: {shortLabel: "gain freeze", chipClass: "gain_freeze", fillColor: "rgba(133, 77, 14, 0.24)", textColor: "#92400e"},
      data_frozen: {shortLabel: "data frozen", chipClass: "data_frozen", fillColor: "rgba(109, 40, 217, 0.22)", textColor: "#6d28d9"},
      unknown: {shortLabel: "unknown", chipClass: "unknown", fillColor: "rgba(71, 84, 103, 0.10)", textColor: "#475467"},
    };
    const DECISION_MODES = ["global", "subband", "fusion"];

    function getStateStyle(state) {
      return STATE_STYLES[state] || STATE_STYLES.unknown;
    }
    function fmt(value, digits = 4) {
      const number = Number(value);
      return Number.isFinite(number) ? number.toFixed(digits) : "--";
    }
    function readXScale(input) {
      const value = Number.parseFloat(input.value);
      return Number.isFinite(value) && value > 0 ? value : 4;
    }
    function hasSubband(row) {
      return row && Number(row.subband_count || 0) > 0 && Array.isArray(row.bands);
    }
    function updateCanvasWidth(canvas, rows, xScale) {
      const scroller = canvas.parentElement;
      if (!scroller) return;
      const targetWidth = Math.max(scroller.clientWidth, rows.length * xScale + 72);
      const pinnedToRight = scroller.scrollLeft + scroller.clientWidth >= scroller.scrollWidth - 24;
      canvas.style.width = `${Math.ceil(targetWidth)}px`;
      if (pinnedToRight) scroller.scrollLeft = scroller.scrollWidth;
    }
    function resizeCanvas(canvas) {
      const rect = canvas.getBoundingClientRect();
      const ratio = window.devicePixelRatio || 1;
      canvas.width = Math.max(1, Math.floor(rect.width * ratio));
      canvas.height = Math.max(1, Math.floor(rect.height * ratio));
      const ctx = canvas.getContext("2d");
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      return {ctx, width: rect.width, height: rect.height};
    }
    function plotArea(w, h) {
      return {left: 48, right: w - 12, top: 14, bottom: h - 38};
    }
    function percentile(values, ratio) {
      const sorted = values.filter(Number.isFinite).sort((a, b) => a - b);
      if (!sorted.length) return 0;
      return sorted[Math.min(sorted.length - 1, Math.max(0, Math.floor((sorted.length - 1) * ratio)))];
    }
    function rangeFor(rows, key, fallbackMin, fallbackMax) {
      const values = rows.map(row => Number(row[key])).filter(Number.isFinite);
      if (!values.length) return [fallbackMin, fallbackMax];
      let minValue = Math.min(...values);
      let maxValue = Math.max(...values);
      if (Math.abs(maxValue - minValue) < 0.000001) {
        minValue -= 1;
        maxValue += 1;
      }
      const pad = (maxValue - minValue) * 0.12;
      return [minValue - pad, maxValue + pad];
    }
    function smoothedRows(rows, keys, samples) {
      if (samples <= 1 || rows.length < 3) return rows;
      const half = Math.floor(samples / 2);
      return rows.map((row, index) => {
        const next = {...row};
        const start = Math.max(0, index - half);
        const end = Math.min(rows.length - 1, index + half);
        const count = end - start + 1;
        for (const key of keys) {
          let sum = 0;
          for (let i = start; i <= end; i++) sum += Number(rows[i][key] || 0);
          next[key] = sum / count;
        }
        return next;
      });
    }
    function drawGrid(ctx, w, h, rows = []) {
      const {left, right, top, bottom} = plotArea(w, h);
      ctx.clearRect(0, 0, w, h);
      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, w, h);
      ctx.strokeStyle = "#edf1f5";
      ctx.lineWidth = 1;
      for (let i = 0; i <= 5; i++) {
        const y = top + (bottom - top) * i / 5;
        ctx.beginPath();
        ctx.moveTo(left, y);
        ctx.lineTo(right, y);
        ctx.stroke();
      }
      ctx.strokeStyle = "#cbd5e1";
      ctx.beginPath();
      ctx.moveTo(left, top);
      ctx.lineTo(left, bottom);
      ctx.lineTo(right, bottom);
      ctx.stroke();
      ctx.fillStyle = "#667085";
      ctx.font = "11px 'Segoe UI', Arial";
      ctx.textAlign = "center";
      if (rows.length >= 1) {
        ctx.fillText(String(rows[0].frame ?? ""), left, bottom + 16);
        ctx.fillText(String(rows[rows.length - 1].frame ?? ""), right - 12, bottom + 16);
      }
      ctx.textAlign = "start";
    }
    function drawSeries(ctx, rows, key, color, minY, maxY, w, h, lineWidth = 1.6) {
      if (rows.length < 2) return;
      const {left, right, top, bottom} = plotArea(w, h);
      const span = Math.max(0.000001, maxY - minY);
      ctx.strokeStyle = color;
      ctx.lineWidth = lineWidth;
      ctx.lineJoin = "round";
      ctx.lineCap = "round";
      ctx.beginPath();
      rows.forEach((row, index) => {
        const x = left + (right - left) * index / Math.max(1, rows.length - 1);
        const y = Math.max(top, Math.min(bottom, bottom - (bottom - top) * (Number(row[key] || 0) - minY) / span));
        if (index === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.stroke();
    }
    function drawStateRegions(ctx, rows, w, h) {
      if (rows.length < 2) return;
      const {left, right, top, bottom} = plotArea(w, h);
      let start = 0;
      for (let i = 1; i <= rows.length; i++) {
        const prevState = rows[start].motion_state || "unknown";
        const currentState = i < rows.length ? (rows[i].motion_state || "unknown") : null;
        if (i === rows.length || currentState !== prevState) {
          const x1 = left + (right - left) * start / Math.max(1, rows.length - 1);
          const x2 = i === rows.length ? right : left + (right - left) * i / Math.max(1, rows.length - 1);
          ctx.fillStyle = getStateStyle(prevState).fillColor;
          ctx.fillRect(x1, top, Math.max(2, x2 - x1), bottom - top);
          start = i;
        }
      }
    }
    function drawText(ctx, text, x, y, color = "#344054") {
      ctx.fillStyle = color;
      ctx.font = "12px 'Segoe UI', Arial";
      ctx.fillText(text, x, y);
    }
    function updateHumanState(state) {
      const style = getStateStyle(state);
      humanStateEl.textContent = `State ${style.shortLabel}`;
      humanStateEl.className = "chip";
      humanStateEl.classList.add(style.chipClass);
    }
    function updateGainState(row) {
      const ready = row.gain_compensated === true;
      gainStateEl.textContent = `Gain ${ready ? "ready" : "collecting"} x${fmt(row.compensate_gain, 3)}`;
      gainStateEl.className = ready ? "chip ready" : "chip collect";
    }
    function updateMetricValues(row) {
      metricEls.frame.textContent = row.frame;
      metricEls.rssi.textContent = row.rssi;
      metricEls.state.textContent = `${row.decision_state_id} ${row.decision_state_text}`;
      metricEls.state.style.color = getStateStyle(row.motion_state).textColor;
      metricEls.deltaNorm.textContent = fmt(row.delta_norm);
      metricEls.baselineDelta.textContent = fmt(row.baseline_delta);
      metricEls.motionScore.textContent = fmt(row.motion_score);
      metricEls.smoothMotionScore.textContent = fmt(row.smooth_motion_score);
      metricEls.agcGain.textContent = row.agc_gain ?? "--";
      metricEls.fftGain.textContent = row.fft_gain ?? "--";
      metricEls.compensateGain.textContent = fmt(row.compensate_gain, 4);
      metricEls.decisionMode.textContent = DECISION_MODES[row.decision_mode] || String(row.decision_mode ?? 0);
      metricEls.decisionScore.textContent = fmt(row.decision_score);
      metricEls.globalScore.textContent = fmt(row.global_norm_score);
      metricEls.globalState.textContent = `${row.global_state_id ?? 0} ${row.global_state_text || "unknown"}`;
      metricEls.subbandScore.textContent = fmt(row.subband_norm_score);
      metricEls.subbandState.textContent = `${row.subband_state_id ?? 0} ${row.subband_state_text || "unknown"}`;
      metricEls.fusionScore.textContent = fmt(row.fusion_score);
      metricEls.fusionState.textContent = `${row.fusion_state_id ?? 0} ${row.fusion_state_text || "unknown"}`;
      metricEls.bestBand.textContent = Number.isFinite(row.best_band) && row.best_band >= 0 ? `Band${row.best_band}` : "--";
    }
    function updateBandCards(row) {
      // 网页端只做 state_id 到文字的映射和展示，不根据 band delta/base/score 重新判断状态。
      if (!hasSubband(row)) {
        subbandStatusEl.textContent = "subband disabled";
        bandCardsEl.innerHTML = "";
        return;
      }
      subbandStatusEl.textContent = `subband=${row.subband_state_id} ${row.subband_state_text} best=${row.best_band}`;
      bandCardsEl.innerHTML = row.bands.map((band, index) => {
        const style = getStateStyle(band.state_text);
        return `<div class="band-card">
          <div class="band-title"><span>Band${index}</span><span style="color:${style.textColor}">${band.state_id} ${band.state_text}</span></div>
          <div class="band-detail"><span>score ${fmt(band.score)}</span><span>delta ${fmt(band.delta)}</span><span>base ${fmt(band.base)}</span></div>
        </div>`;
      }).join("");
    }
    function drawFeaturePanel() {
      updateCanvasWidth(rawScoreCanvas, rawData, readXScale(rawXScaleEl));
      const {ctx, width, height} = resizeCanvas(rawScoreCanvas);
      drawGrid(ctx, width, height, rawData);
      if (!rawData.length) {
        drawText(ctx, "waiting for RAW...", 58, 36);
        rawReadoutEl.textContent = "waiting for RAW...";
        return;
      }
      const keys = ["delta_norm", "baseline_delta", "motion_score", "smooth_motion_score"];
      const rows = smoothedRows(rawData, keys, smoothSamples);
      const values = [];
      rows.forEach(row => keys.forEach(key => values.push(Number(row[key] || 0))));
      const maxScore = scoreMaxOverride > 0 ? scoreMaxOverride : Math.max(0.02, percentile(values, 0.98) * 1.35);
      drawStateRegions(ctx, rawData, width, height);
      drawSeries(ctx, rows, "delta_norm", "#1f77b4", 0, maxScore, width, height, 1.2);
      drawSeries(ctx, rows, "baseline_delta", "#ff7f0e", 0, maxScore, width, height, 1.2);
      drawSeries(ctx, rows, "motion_score", "#2ca02c", 0, maxScore, width, height, 1.4);
      drawSeries(ctx, rows, "smooth_motion_score", "#9467bd", 0, maxScore, width, height, 2.0);
      const last = rawData[rawData.length - 1];
      rawReadoutEl.textContent = `RAW f=${last.frame} state=${last.decision_state_id} ${last.decision_state_text} rssi=${last.rssi}`;
      rawReadoutEl.style.color = getStateStyle(last.motion_state).textColor;
    }
    function drawDebugPanel() {
      updateCanvasWidth(gainDebugCanvas, debugData, readXScale(debugXScaleEl));
      const {ctx, width, height} = resizeCanvas(gainDebugCanvas);
      drawGrid(ctx, width, height, debugData);
      if (!debugData.length) {
        drawText(ctx, "waiting for CSI_DBG...", 58, 36);
        debugReadoutEl.textContent = "waiting for CSI_DBG...";
        return;
      }
      const keys = ["motion_score", "baseline_delta", "compensate_gain", "agc_gain", "fft_gain"];
      const rows = smoothedRows(debugData, keys, smoothSamples);
      const maxScore = scoreMaxOverride > 0 ? scoreMaxOverride : Math.max(0.02, percentile(rows.flatMap(row => [row.motion_score, row.baseline_delta]), 0.98) * 1.3);
      drawSeries(ctx, rows, "motion_score", "#2ca02c", 0, maxScore, width, height, 1.8);
      drawSeries(ctx, rows, "baseline_delta", "#ff7f0e", 0, maxScore, width, height, 1.5);
      drawSeries(ctx, rows, "compensate_gain", "#17becf", ...rangeFor(rows, "compensate_gain", 0.5, 1.5), width, height, 1.3);
      drawSeries(ctx, rows, "agc_gain", "#d62728", ...rangeFor(rows, "agc_gain", 0, 64), width, height, 1.1);
      drawSeries(ctx, rows, "fft_gain", "#9467bd", ...rangeFor(rows, "fft_gain", -32, 32), width, height, 1.1);
      const last = debugData[debugData.length - 1];
      debugReadoutEl.textContent = `frame=${last.frame} motion=${fmt(last.motion_score)} base=${fmt(last.baseline_delta)} comp=${fmt(last.compensate_gain, 3)} agc=${last.agc_gain} fft=${last.fft_gain}`;
    }
    function drawRssiPanel() {
      updateCanvasWidth(rssiCanvas, rawData, readXScale(rssiXScaleEl));
      const {ctx, width, height} = resizeCanvas(rssiCanvas);
      drawGrid(ctx, width, height, rawData);
      if (!rawData.length) {
        drawText(ctx, "waiting for RSSI...", 58, 36);
        rssiReadoutEl.textContent = "waiting for RSSI...";
        return;
      }
      const rows = smoothedRows(rawData, ["rssi"], Math.min(5, smoothSamples));
      const [minRssi, maxRssi] = rangeFor(rows, "rssi", -95, -25);
      drawStateRegions(ctx, rawData, width, height);
      drawSeries(ctx, rows, "rssi", "#d62728", minRssi, maxRssi, width, height, 2.0);
      const last = rawData[rawData.length - 1];
      rssiReadoutEl.textContent = `frame=${last.frame} rssi=${last.rssi} dBm state=${last.decision_state_id} ${last.decision_state_text}`;
      rssiReadoutEl.style.color = getStateStyle(last.motion_state).textColor;
    }
    function drawBandPanel() {
      updateCanvasWidth(bandScoreCanvas, rawData, readXScale(bandXScaleEl));
      const {ctx, width, height} = resizeCanvas(bandScoreCanvas);
      drawGrid(ctx, width, height, rawData);
      const bandRows = rawData.filter(hasSubband).map(row => ({
        frame: row.frame,
        motion_state: row.motion_state,
        band0_score: row.bands[0]?.score ?? 0,
        band1_score: row.bands[1]?.score ?? 0,
        band2_score: row.bands[2]?.score ?? 0,
        band3_score: row.bands[3]?.score ?? 0,
        subband_norm_score: row.subband_norm_score,
      }));
      if (!bandRows.length) {
        // 兼容旧固件或关闭 subband 的数据流，没有扩展字段时页面不报错。
        drawText(ctx, "subband disabled", 58, 36);
        bandReadoutEl.textContent = "subband disabled";
        return;
      }
      const keys = ["band0_score", "band1_score", "band2_score", "band3_score", "subband_norm_score"];
      const rows = smoothedRows(bandRows, keys, smoothSamples);
      const values = [];
      rows.forEach(row => keys.forEach(key => values.push(Number(row[key] || 0))));
      const maxScore = scoreMaxOverride > 0 ? scoreMaxOverride : Math.max(0.02, percentile(values, 0.98) * 1.35);
      drawStateRegions(ctx, bandRows, width, height);
      drawSeries(ctx, rows, "band0_score", "#2563eb", 0, maxScore, width, height, 1.4);
      drawSeries(ctx, rows, "band1_score", "#f97316", 0, maxScore, width, height, 1.4);
      drawSeries(ctx, rows, "band2_score", "#16a34a", 0, maxScore, width, height, 1.4);
      drawSeries(ctx, rows, "band3_score", "#9333ea", 0, maxScore, width, height, 1.4);
      drawSeries(ctx, rows, "subband_norm_score", "#dc2626", 0, maxScore, width, height, 2.0);
      const last = bandRows[bandRows.length - 1];
      bandReadoutEl.textContent = `frame=${last.frame} band0=${fmt(last.band0_score)} band1=${fmt(last.band1_score)} band2=${fmt(last.band2_score)} band3=${fmt(last.band3_score)} subband=${fmt(last.subband_norm_score)}`;
    }
    function redraw() {
      drawFeaturePanel();
      drawDebugPanel();
      drawRssiPanel();
      drawBandPanel();
    }
    window.addEventListener("resize", redraw);
    [rawXScaleEl, debugXScaleEl, rssiXScaleEl, bandXScaleEl].forEach(input => {
      input.addEventListener("input", redraw);
      input.addEventListener("change", redraw);
    });
    pauseBtn.addEventListener("click", () => {
      paused = !paused;
      pauseBtn.textContent = paused ? "继续" : "暂停";
      pauseBtn.classList.toggle("paused", paused);
      redraw();
    });
    setInterval(redraw, 120);

    const events = new EventSource("/events");
    events.onmessage = (event) => {
      const msg = JSON.parse(event.data);
      if (msg.type === "status") {
        statusEl.textContent = `${msg.status}${msg.port ? " - " + msg.port : ""}`;
      } else if (msg.type === "link") {
        wifiNameEl.textContent = `WiFi ${msg.wifi_ssid || "unknown"}`;
      } else if (msg.type === "feature") {
        if (paused) return;
        rawData.push(msg);
        while (rawData.length > maxPoints) rawData.shift();
        updateHumanState(msg.motion_state);
        updateGainState(msg);
        updateMetricValues(msg);
        updateBandCards(msg);
      } else if (msg.type === "debug") {
        if (paused) return;
        debugData.push(msg);
        while (debugData.length > maxPoints) debugData.shift();
      }
    };
  </script>
</body>
</html>
"""


def make_handler(state: PlotState, args: argparse.Namespace):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path == "/" or self.path.startswith("/index"):
                page = (
                    HTML
                    .replace("__MAX_POINTS__", str(args.max_points))
                    .replace("__SMOOTH_SAMPLES__", str(args.smooth_samples))
                    .replace("__SCORE_MAX__", str(args.score_max if args.score_max is not None else 0))
                )
                body = page.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            if self.path.startswith("/events"):
                client = state.add_client()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                try:
                    while True:
                        item = client.get()
                        payload = json.dumps(item, separators=(",", ":"))
                        self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    state.remove_client(client)
                return

            self.send_error(404)

        def log_message(self, format: str, *args) -> None:
            return

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description="打开浏览器实时绘制 ESP32-C5 CSI 串口数据。")
    parser.add_argument("--port", help="串口号，例如 COM7；不填时自动检测。")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--max-points", type=int, default=3600, help="浏览器中保留的最大 CSI 帧数。")
    parser.add_argument("--smooth-samples", type=int, default=7, help="仅用于显示的移动平均窗口。")
    parser.add_argument("--score-max", type=float, default=0.30, help="CSI 分数曲线固定 Y 轴上限。")
    parser.add_argument("--no-open", action="store_true", help="不自动打开浏览器。")
    args = parser.parse_args()

    args.max_points = max(60, args.max_points)
    args.smooth_samples = max(1, args.smooth_samples)
    args.score_max = max(0.001, args.score_max)

    state = PlotState()
    stop_event = threading.Event()
    reader = threading.Thread(target=serial_loop, args=(args, state, stop_event), daemon=True)
    reader.start()

    server = ThreadingHTTPServer((args.host, args.http_port), make_handler(state, args))
    url = f"http://{args.host}:{args.http_port}/"
    print(f"CSI plot server: {url}")
    print("连接或复位 ESP32-C5。关闭此窗口即可停止服务。")

    if not args.no_open:
        webbrowser.open(url)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        server.server_close()


if __name__ == "__main__":
    main()
