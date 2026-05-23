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
    agc_gain: int = 0
    fft_gain: int = 0
    compensate_gain: float = 1.0
    gain_compensated: bool = False


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


def parse_csi_feature(line: str) -> CsiFeature | None:
    if line.startswith("CSI_FEATURE_RAW,"):
        source = "raw"
    elif line.startswith("CSI_FEATURE,"):
        source = "raw"
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
            motion_state=motion_state,
            agc_gain=agc_gain,
            fft_gain=fft_gain,
            compensate_gain=compensate_gain,
            gain_compensated=gain_compensated,
        )
    except ValueError:
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
    #humanState.recover { background: var(--recover); }
    #humanState.data_frozen { background: var(--freeze); }
    #humanState.gain_freeze { background: var(--warn); }
    #humanState.unknown { background: #475467; }
    #humanState.motion::before { background: #fecaca; box-shadow: 0 0 0 3px rgba(254, 202, 202, 0.22); }
    #humanState.static::before { background: #bbf7d0; box-shadow: 0 0 0 3px rgba(187, 247, 208, 0.18); }
    #humanState.recover::before { background: #bfdbfe; box-shadow: 0 0 0 3px rgba(191, 219, 254, 0.20); }
    #humanState.gain_freeze::before { background: #fdba74; box-shadow: 0 0 0 3px rgba(253, 186, 116, 0.20); }
    #humanState.data_frozen::before { background: #d8b4fe; box-shadow: 0 0 0 3px rgba(216, 180, 254, 0.22); }
    #humanState.unknown::before { background: #cbd5e1; box-shadow: 0 0 0 3px rgba(203, 213, 225, 0.18); }
    #gainState { background: var(--warn); color: white; }
    #gainState.ready { background: var(--ok); }
    #gainState.collect { background: var(--warn); }
    main {
      min-height: calc(100vh - 58px);
      min-width: 0;
      display: grid;
      grid-template-rows: auto minmax(380px, 42vh) minmax(340px, 38vh) minmax(280px, 30vh);
      gap: 12px;
      padding: 12px;
      overflow: visible;
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
    .readout-text {
      min-width: 0;
      flex: 1 1 auto;
      overflow: hidden;
      text-overflow: ellipsis;
    }
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
      main { grid-template-rows: auto 360px 320px 260px; padding: 8px; gap: 8px; }
      canvas { min-width: 960px; }
      #metricValues { grid-template-columns: repeat(2, minmax(0, 1fr)); }
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
      <div class="metric"><span>motion max</span><span id="vMotionScoreMax">--</span></div>
      <div class="metric"><span>motion min</span><span id="vMotionScoreMin">--</span></div>
      <div class="metric"><span>smooth</span><span id="vSmoothMotionScore">--</span></div>
      <div class="metric"><span>AGC</span><span id="vAgcGain">--</span></div>
      <div class="metric"><span>FFT</span><span id="vFftGain">--</span></div>
      <div class="metric"><span>comp</span><span id="vCompensateGain">--</span></div>
      <div class="metric"><span>gain</span><span id="vGainCompensated">--</span></div>
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
        <span class="item" style="--c:rgba(22, 101, 52, 0.18)">static</span>
        <span class="item" style="--c:rgba(185, 28, 28, 0.18)">motion</span>
        <span class="item" style="--c:rgba(29, 78, 216, 0.18)">recover</span>
        <span class="item" style="--c:rgba(133, 77, 14, 0.24)">gain 冻结</span>
        <span class="item" style="--c:rgba(109, 40, 217, 0.22)">data 冻结</span>
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
        <span class="item" style="--c:rgba(22, 101, 52, 0.18)">static</span>
        <span class="item" style="--c:rgba(185, 28, 28, 0.18)">motion</span>
        <span class="item" style="--c:rgba(29, 78, 216, 0.18)">recover</span>
        <span class="item" style="--c:rgba(133, 77, 14, 0.24)">gain</span>
        <span class="item" style="--c:rgba(109, 40, 217, 0.22)">data</span>
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
    const rawReadoutEl = document.getElementById("rawReadout");
    const debugReadoutEl = document.getElementById("debugReadout");
    const rssiReadoutEl = document.getElementById("rssiReadout");
    const rawXScaleEl = document.getElementById("rawXScale");
    const debugXScaleEl = document.getElementById("debugXScale");
    const rssiXScaleEl = document.getElementById("rssiXScale");
    const metricEls = {
      frame: document.getElementById("vFrame"),
      rssi: document.getElementById("vRssi"),
      state: document.getElementById("vState"),
      deltaNorm: document.getElementById("vDeltaNorm"),
      baselineDelta: document.getElementById("vBaselineDelta"),
      motionScore: document.getElementById("vMotionScore"),
      motionScoreMax: document.getElementById("vMotionScoreMax"),
      motionScoreMin: document.getElementById("vMotionScoreMin"),
      smoothMotionScore: document.getElementById("vSmoothMotionScore"),
      agcGain: document.getElementById("vAgcGain"),
      fftGain: document.getElementById("vFftGain"),
      compensateGain: document.getElementById("vCompensateGain"),
      gainCompensated: document.getElementById("vGainCompensated"),
    };

    const STATE_STYLES = {
      motion: {
        label: "motion (有人动)",
        shortLabel: "motion 有人动",
        chipClass: "motion",
        fillColor: "rgba(185, 28, 28, 0.18)",
        textColor: "#c1121f",
      },
      static: {
        label: "static (无人动)",
        shortLabel: "static 无人动",
        chipClass: "static",
        fillColor: "rgba(22, 101, 52, 0.18)",
        textColor: "#166534",
      },
      recover: {
        label: "recover (基线恢复)",
        shortLabel: "recover 恢复",
        chipClass: "recover",
        fillColor: "rgba(29, 78, 216, 0.18)",
        textColor: "#1d4ed8",
      },
      gain_freeze: {
        label: "gain_freeze (增益冻结)",
        shortLabel: "gain 冻结",
        chipClass: "gain_freeze",
        fillColor: "rgba(133, 77, 14, 0.24)",
        textColor: "#92400e",
      },
      data_frozen: {
        label: "data_frozen (数据冻结)",
        shortLabel: "data 冻结",
        chipClass: "data_frozen",
        fillColor: "rgba(109, 40, 217, 0.22)",
        textColor: "#6d28d9",
      },
      unknown: {
        label: "unknown (未知)",
        shortLabel: "unknown",
        chipClass: "unknown",
        fillColor: "rgba(71, 84, 103, 0.10)",
        textColor: "#475467",
      },
    };
    const STATE_ORDER = ["static", "motion", "recover", "gain_freeze", "data_frozen", "unknown"];

    function getStateStyle(state) {
      return STATE_STYLES[state] || {
        ...STATE_STYLES.unknown,
        label: `${state || "unknown"} (未知)`,
        shortLabel: state || "unknown",
      };
    }

    function readXScale(input) {
      const value = Number.parseFloat(input.value);
      return Number.isFinite(value) && value > 0 ? value : 4;
    }

    function updateCanvasWidth(canvas, rows, xScale) {
      const scroller = canvas.parentElement;
      if (!scroller) return;

      const plotPadding = 72;
      const targetWidth = Math.max(scroller.clientWidth, rows.length * xScale + plotPadding);
      const pinnedToRight = scroller.scrollLeft + scroller.clientWidth >= scroller.scrollWidth - 24;
      canvas.style.width = `${Math.ceil(targetWidth)}px`;
      if (pinnedToRight) {
        scroller.scrollLeft = scroller.scrollWidth;
      }
    }

    function resizeCanvas(canvas) {
      const rect = canvas.getBoundingClientRect();
      const ratio = window.devicePixelRatio || 1;
      canvas.width = Math.max(1, Math.floor(rect.width * ratio));
      canvas.height = Math.max(1, Math.floor(rect.height * ratio));
      const ctx = canvas.getContext("2d");
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    }

    function plotArea(w, h) {
      return {left: 48, right: w - 12, top: 14, bottom: h - 38};
    }

    function niceTickStep(rawStep) {
      const magnitude = Math.pow(10, Math.floor(Math.log10(Math.max(1, rawStep))));
      const normalized = rawStep / magnitude;
      const nice = normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10;
      return nice * magnitude;
    }

    function xForIndex(index, rows, area) {
      return area.left + (area.right - area.left) * index / Math.max(1, rows.length - 1);
    }

    function drawGrid(ctx, w, h, rows = []) {
      ctx.clearRect(0, 0, w, h);
      const {left, right, top, bottom} = plotArea(w, h);
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
      ctx.font = "11px 'Segoe UI', Arial";
      ctx.fillStyle = "#667085";
      ctx.textAlign = "center";
      ctx.textBaseline = "top";

      if (rows.length >= 2) {
        let lastTickX = -Infinity;
        let lastLabelX = -Infinity;
        const firstFrame = rows[0].frame;
        const lastFrame = rows[rows.length - 1].frame;
        const frameSpan = Number.isFinite(firstFrame) && Number.isFinite(lastFrame) ? Math.max(1, lastFrame - firstFrame) : rows.length;
        const targetTickCount = Math.max(2, Math.floor((right - left) / 90));
        const tickStep = niceTickStep(frameSpan / targetTickCount);
        let nextTick = Number.isFinite(firstFrame) ? Math.floor(firstFrame / tickStep) * tickStep + tickStep : Infinity;
        const drawFrameTick = (index, forceLabel = false) => {
          const row = rows[index];
          if (!row || !Number.isFinite(row.frame)) return;
          const x = xForIndex(index, rows, {left, right, top, bottom});
          if (x - lastTickX >= 8) {
            ctx.beginPath();
            ctx.moveTo(x, top);
            ctx.lineTo(x, bottom);
            ctx.stroke();
            lastTickX = x;
          }
          if (forceLabel || x - lastLabelX >= 54) {
            ctx.fillText(String(row.frame), x, bottom + 6);
            lastLabelX = x;
          }
        };

        drawFrameTick(0, true);
        for (let i = 1; i < rows.length - 1; i++) {
          const frame = rows[i].frame;
          if (Number.isFinite(frame) && frame >= nextTick) {
            drawFrameTick(i);
            while (nextTick <= frame) nextTick += tickStep;
          }
        }
        drawFrameTick(rows.length - 1, rows.length < 10);
      } else {
        for (let i = 0; i <= 6; i++) {
          const x = left + (right - left) * i / 6;
          ctx.beginPath();
          ctx.moveTo(x, top);
          ctx.lineTo(x, bottom);
          ctx.stroke();
        }
      }
      ctx.strokeStyle = "#cbd5e1";
      ctx.beginPath();
      ctx.moveTo(left, top);
      ctx.lineTo(left, bottom);
      ctx.lineTo(right, bottom);
      ctx.stroke();
      ctx.textAlign = "start";
      ctx.textBaseline = "alphabetic";
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
          for (let i = start; i <= end; i++) sum += rows[i][key];
          next[key] = sum / count;
        }
        return next;
      });
    }

    function percentile(values, ratio) {
      if (!values.length) return 0;
      const sorted = values.slice().sort((a, b) => a - b);
      const index = Math.min(sorted.length - 1, Math.max(0, Math.floor((sorted.length - 1) * ratio)));
      return sorted[index];
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
      rows.forEach((row, i) => {
        const x = left + (right - left) * i / Math.max(1, rows.length - 1);
        const rawY = bottom - (bottom - top) * (row[key] - minY) / span;
        const y = Math.max(top, Math.min(bottom, rawY));
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.stroke();
    }

    function drawStateRegions(ctx, rows, w, h) {
      if (rows.length < 2) return;
      const {left, right, top, bottom} = plotArea(w, h);
      const xForIndex = (index) => left + (right - left) * index / Math.max(1, rows.length - 1);

      let start = 0;
      for (let i = 1; i <= rows.length; i++) {
        const prevState = STATE_ORDER.includes(rows[start].motion_state) ? rows[start].motion_state : "unknown";
        const currentState = i < rows.length && STATE_ORDER.includes(rows[i].motion_state) ? rows[i].motion_state : "unknown";
        if (i === rows.length || currentState !== prevState) {
          const x1 = xForIndex(start);
          const x2 = i === rows.length ? right : xForIndex(i);
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

    function stateText(state) {
      return getStateStyle(state).label;
    }

    function shortStateText(state) {
      return getStateStyle(state).shortLabel;
    }

    function updateHumanState(state) {
      const style = getStateStyle(state);
      humanStateEl.textContent = `State ${shortStateText(state)}`;
      humanStateEl.className = "chip";
      humanStateEl.classList.add(style.chipClass);
    }

    function updateGainState(row) {
      const ready = row.gain_compensated === true;
      const status = ready ? "ready 可用" : "collecting";
      gainStateEl.textContent = `Gain ${status} x${fmt(row.compensate_gain, 3)}`;
      gainStateEl.className = ready ? "chip ready" : "chip collect";
    }

    function fmt(value, digits = 4) {
      return Number.isFinite(value) ? value.toFixed(digits) : "--";
    }

    function updateMetricValues(row, rows) {
      const motionScores = rows.map(item => item.motion_score).filter(Number.isFinite);
      const motionScoreMax = motionScores.length ? Math.max(...motionScores) : NaN;
      const motionScoreMin = motionScores.length ? Math.min(...motionScores) : NaN;

      metricEls.frame.textContent = row.frame;
      metricEls.rssi.textContent = row.rssi;
      metricEls.state.textContent = shortStateText(row.motion_state);
      metricEls.state.style.color = getStateStyle(row.motion_state).textColor;
      metricEls.deltaNorm.textContent = fmt(row.delta_norm);
      metricEls.baselineDelta.textContent = fmt(row.baseline_delta);
      metricEls.motionScore.textContent = fmt(row.motion_score);
      metricEls.motionScoreMax.textContent = fmt(motionScoreMax);
      metricEls.motionScoreMin.textContent = fmt(motionScoreMin);
      metricEls.smoothMotionScore.textContent = fmt(row.smooth_motion_score);
      metricEls.agcGain.textContent = row.agc_gain ?? "--";
      metricEls.fftGain.textContent = row.fft_gain ?? "--";
      metricEls.compensateGain.textContent = fmt(row.compensate_gain, 4);
      metricEls.gainCompensated.textContent = row.gain_compensated ? "1 ready" : "0 collecting";
    }

    function drawFeaturePanel(canvas, rows, title) {
      updateCanvasWidth(canvas, rows, readXScale(rawXScaleEl));
      resizeCanvas(canvas);
      const rect = canvas.getBoundingClientRect();
      const ctx = canvas.getContext("2d");
      drawGrid(ctx, rect.width, rect.height, rows);

      if (!rows.length) {
        drawText(ctx, `waiting for ${title}...`, 58, 36);
        rawReadoutEl.textContent = `waiting for ${title}...`;
        rawReadoutEl.style.color = "#344054";
        return;
      }

      const keys = ["delta_norm", "baseline_delta", "motion_score", "smooth_motion_score"];
      const viewRows = smoothedRows(rows, keys, smoothSamples);
      const scoreValues = [];
      for (const row of viewRows) for (const key of keys) scoreValues.push(row[key]);
      const robustMax = Math.max(0.02, percentile(scoreValues, 0.95) * 1.45);
      const hardMax = Math.max(robustMax, percentile(scoreValues, 0.995) * 1.05);
      const maxScore = scoreMaxOverride > 0 ? scoreMaxOverride : Math.min(hardMax, robustMax * 2.2);

      drawStateRegions(ctx, rows, rect.width, rect.height);
      drawSeries(ctx, viewRows, "delta_norm", "#1f77b4", 0, maxScore, rect.width, rect.height, 1.2);
      drawSeries(ctx, viewRows, "baseline_delta", "#ff7f0e", 0, maxScore, rect.width, rect.height, 1.2);
      drawSeries(ctx, viewRows, "motion_score", "#2ca02c", 0, maxScore, rect.width, rect.height, 1.3);
      drawSeries(ctx, viewRows, "smooth_motion_score", "#9467bd", 0, maxScore, rect.width, rect.height, 2.0);
      drawText(ctx, maxScore.toFixed(3), 10, 20);
      drawText(ctx, "0", 31, rect.height - 27);

      const last = rows[rows.length - 1];
      const stateColor = getStateStyle(last.motion_state).textColor;
      rawReadoutEl.textContent = `${title} f=${last.frame} state=${last.motion_state} gain=${last.gain_compensated ? "ready" : "off"} x${Number(last.compensate_gain).toFixed(3)} rssi=${last.rssi}`;
      rawReadoutEl.style.color = stateColor;
    }

    function rangeFor(rows, key, fallbackMin, fallbackMax) {
      const values = rows.map(row => row[key]).filter(Number.isFinite);
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

    function drawDebugPanel() {
      updateCanvasWidth(gainDebugCanvas, debugData, readXScale(debugXScaleEl));
      resizeCanvas(gainDebugCanvas);
      const rect = gainDebugCanvas.getBoundingClientRect();
      const ctx = gainDebugCanvas.getContext("2d");
      drawGrid(ctx, rect.width, rect.height, debugData);

      if (!debugData.length) {
        drawText(ctx, "waiting for CSI_DBG...", 58, 36);
        debugReadoutEl.textContent = "waiting for CSI_DBG...";
        debugReadoutEl.style.color = "#344054";
        return;
      }

      const scoreRows = smoothedRows(debugData, ["motion_score", "baseline_delta", "compensate_gain", "agc_gain", "fft_gain"], smoothSamples);
      const scoreValues = [];
      for (const row of scoreRows) {
        scoreValues.push(row.motion_score, row.baseline_delta);
      }
      const maxScore = scoreMaxOverride > 0 ? scoreMaxOverride : Math.max(0.02, percentile(scoreValues, 0.98) * 1.3);
      const [compMin, compMax] = rangeFor(scoreRows, "compensate_gain", 0.5, 1.5);
      const [agcMin, agcMax] = rangeFor(scoreRows, "agc_gain", 0, 64);
      const [fftMin, fftMax] = rangeFor(scoreRows, "fft_gain", -32, 32);

      drawSeries(ctx, scoreRows, "motion_score", "#2ca02c", 0, maxScore, rect.width, rect.height, 1.8);
      drawSeries(ctx, scoreRows, "baseline_delta", "#ff7f0e", 0, maxScore, rect.width, rect.height, 1.6);
      drawSeries(ctx, scoreRows, "compensate_gain", "#17becf", compMin, compMax, rect.width, rect.height, 1.4);
      drawSeries(ctx, scoreRows, "agc_gain", "#d62728", agcMin, agcMax, rect.width, rect.height, 1.2);
      drawSeries(ctx, scoreRows, "fft_gain", "#9467bd", fftMin, fftMax, rect.width, rect.height, 1.2);

      const last = debugData[debugData.length - 1];
      debugReadoutEl.textContent = `frame=${last.frame} motion=${fmt(last.motion_score)} base=${fmt(last.baseline_delta)} comp=${fmt(last.compensate_gain, 3)} agc=${last.agc_gain} fft=${last.fft_gain}`;
      debugReadoutEl.style.color = "#344054";
    }

    function drawRssiPanel() {
      updateCanvasWidth(rssiCanvas, rawData, readXScale(rssiXScaleEl));
      resizeCanvas(rssiCanvas);
      const rect = rssiCanvas.getBoundingClientRect();
      const ctx = rssiCanvas.getContext("2d");
      drawGrid(ctx, rect.width, rect.height, rawData);

      if (!rawData.length) {
        drawText(ctx, "waiting for RSSI...", 58, 36);
        rssiReadoutEl.textContent = "waiting for RSSI...";
        rssiReadoutEl.style.color = "#344054";
        return;
      }

      const viewRows = smoothedRows(rawData, ["rssi"], Math.min(5, smoothSamples));
      const [minRssi, maxRssi] = rangeFor(viewRows, "rssi", -95, -25);
      drawStateRegions(ctx, rawData, rect.width, rect.height);
      drawSeries(ctx, viewRows, "rssi", "#d62728", minRssi, maxRssi, rect.width, rect.height, 2.0);

      const last = rawData[rawData.length - 1];
      drawText(ctx, `${Math.round(maxRssi)} dBm`, 10, 20);
      drawText(ctx, `${Math.round(minRssi)} dBm`, 10, rect.height - 27);
      rssiReadoutEl.textContent = `frame=${last.frame} rssi=${last.rssi} dBm state=${last.motion_state}`;
      rssiReadoutEl.style.color = getStateStyle(last.motion_state).textColor;
    }

    function redraw() {
      drawFeaturePanel(rawScoreCanvas, rawData, "RAW");
      drawDebugPanel();
      drawRssiPanel();
    }

    window.addEventListener("resize", redraw);
    [rawXScaleEl, debugXScaleEl, rssiXScaleEl].forEach((input) => {
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
        updateMetricValues(msg, rawData);
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
