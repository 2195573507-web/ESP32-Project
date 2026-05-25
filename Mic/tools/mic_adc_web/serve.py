#!/usr/bin/env python3
"""启动 Mic ADC Web Serial 本地页面。"""

from __future__ import annotations

import functools
import http.server
import json
import os
import queue
import socket
import socketserver
import sys
import termios
import threading
import time
import webbrowser
from pathlib import Path

HOST = "127.0.0.1"
START_PORT = 8787
MAX_PORT_TRIES = 20
SERIAL_BAUD = 115200
SERIAL_RETRY_SECONDS = 1.0
SERIAL_READ_CHUNK = 4096
SSE_KEEPALIVE_SECONDS = 5.0


def find_serial_port() -> str | None:
    """调用方法：串口读取线程每次准备连接硬件前调用，返回优先级最高的 ESP32 串口。"""
    explicit_port = os.environ.get("MIC_ADC_SERIAL_PORT")
    if explicit_port:
        return explicit_port if Path(explicit_port).exists() else None

    dev_dir = Path("/dev")
    candidates: list[str] = []
    for pattern in ("cu.usbmodem*", "cu.usbserial*", "cu.wchusbserial*", "tty.usbmodem*", "tty.usbserial*"):
        candidates.extend(str(path) for path in sorted(dev_dir.glob(pattern)))

    filtered = [
        path for path in candidates
        if "Bluetooth" not in path and "debug-console" not in path
    ]
    return filtered[0] if filtered else None


def baud_constant(baud: int) -> int:
    """调用方法：open_serial_port() 配置 termios 波特率前调用。"""
    baud_map = {
        115200: termios.B115200,
        230400: getattr(termios, "B230400", termios.B115200),
        460800: getattr(termios, "B460800", termios.B115200),
        921600: getattr(termios, "B921600", termios.B115200),
    }
    return baud_map.get(baud, termios.B115200)


def open_serial_port(port: str, baud: int) -> int:
    """调用方法：串口读取线程找到 ESP32 设备后调用，返回已设置 raw 模式的 fd。"""
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
                  termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    attrs[1] &= ~termios.OPOST
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~(termios.CSIZE | termios.PARENB)
    attrs[2] |= termios.CS8
    attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    attrs[4] = baud_constant(baud)
    attrs[5] = baud_constant(baud)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


class SerialBridge:
    """把 ESP32 串口行广播给网页 SSE 客户端。"""

    def __init__(self) -> None:
        """调用方法：模块加载时创建单例，由 HTTP handler 使用。"""
        self._clients: list[queue.Queue[dict[str, str]]] = []
        self._lock = threading.Lock()
        self._started = False

    def add_client(self) -> queue.Queue[dict[str, str]]:
        """调用方法：网页访问 /events 时调用，为该网页创建一个消息队列。"""
        client: queue.Queue[dict[str, str]] = queue.Queue(maxsize=256)
        with self._lock:
            self._clients.append(client)
            if not self._started:
                self._started = True
                threading.Thread(target=self._read_loop, name="mic_adc_serial", daemon=True).start()
        client.put({"type": "status", "message": "bridge-client-ready"})
        return client

    def remove_client(self, client: queue.Queue[dict[str, str]]) -> None:
        """调用方法：网页断开 /events 连接时调用。"""
        with self._lock:
            if client in self._clients:
                self._clients.remove(client)

    def broadcast(self, message: dict[str, str]) -> None:
        """调用方法：串口读取线程读到状态或数据行时调用。"""
        with self._lock:
            clients = list(self._clients)

        for client in clients:
            try:
                client.put_nowait(message)
            except queue.Full:
                try:
                    client.get_nowait()
                    client.put_nowait(message)
                except queue.Empty:
                    pass

    def _read_loop(self) -> None:
        """调用方法：add_client() 首次调用时在后台线程自动启动。"""
        partial = ""
        while True:
            port = find_serial_port()
            if port is None:
                self.broadcast({"type": "status", "message": "serial-port-not-found"})
                time.sleep(SERIAL_RETRY_SECONDS)
                continue

            fd: int | None = None
            try:
                fd = open_serial_port(port, SERIAL_BAUD)
                self.broadcast({"type": "status", "message": "serial-open", "port": port})

                while True:
                    try:
                        data = os.read(fd, SERIAL_READ_CHUNK)
                    except BlockingIOError:
                        time.sleep(0.02)
                        continue

                    if not data:
                        time.sleep(0.02)
                        continue

                    partial += data.decode("utf-8", errors="replace")
                    lines = partial.splitlines(keepends=True)
                    partial = ""
                    for item in lines:
                        if item.endswith(("\n", "\r")):
                            line = item.strip()
                            if line:
                                self.broadcast({"type": "line", "line": line})
                        else:
                            partial = item
            except OSError as exc:
                self.broadcast({"type": "status", "message": f"serial-error: {exc}"})
                time.sleep(SERIAL_RETRY_SECONDS)
            finally:
                if fd is not None:
                    os.close(fd)


SERIAL_BRIDGE = SerialBridge()


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """只输出较少日志的静态文件处理器。"""

    def handle(self) -> None:
        """调用方法：http.server 为每个 TCP 客户端创建 handler 后自动调用。"""
        try:
            super().handle()
        except ConnectionResetError:
            # 浏览器关闭页面或刷新时会主动重置连接，这是网页重连过程中的正常现象。
            pass

    def do_GET(self) -> None:
        """调用方法：http.server 收到 GET 请求时自动调用。"""
        if self.path.split("?", 1)[0] == "/events":
            self.handle_events()
            return
        super().do_GET()

    def handle_events(self) -> None:
        """调用方法：do_GET() 识别到 /events 时调用，用 SSE 向网页推送串口行。"""
        client = SERIAL_BRIDGE.add_client()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        try:
            while True:
                try:
                    message = client.get(timeout=SSE_KEEPALIVE_SECONDS)
                except queue.Empty:
                    # SSE 注释行不会触发浏览器 onmessage，只用于保持连接活性。
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    continue

                payload = json.dumps(message, ensure_ascii=False)
                self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            SERIAL_BRIDGE.remove_client(client)

    def log_message(self, format: str, *args: object) -> None:
        """调用方法：http.server 在每次请求结束后自动调用。"""
        sys.stderr.write("[mic_adc_web] " + (format % args) + "\n")


class ReusableTCPServer(socketserver.ThreadingTCPServer):
    """允许快速重启的本地 HTTP server。"""

    allow_reuse_address = True
    daemon_threads = True


def find_free_port(start_port: int) -> int:
    """调用方法：main() 启动 server 前调用，返回一个可绑定的本地端口。"""
    for port in range(start_port, start_port + MAX_PORT_TRIES):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            try:
                sock.bind((HOST, port))
            except OSError:
                continue
            return port
    raise RuntimeError(f"no free port from {start_port} to {start_port + MAX_PORT_TRIES - 1}")


def main() -> None:
    """调用方法：在项目根目录执行 `python3 tools/mic_adc_web/serve.py`。"""
    web_dir = Path(__file__).resolve().parent
    port = find_free_port(START_PORT)
    handler = functools.partial(QuietHandler, directory=str(web_dir))
    url = f"http://{HOST}:{port}/"

    with ReusableTCPServer((HOST, port), handler) as httpd:
        print(f"Mic ADC web monitor: {url}")
        print("Press Ctrl+C to stop.")
        webbrowser.open(url)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nStopped.")


if __name__ == "__main__":
    main()
