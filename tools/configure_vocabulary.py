#!/usr/bin/env python3
"""Provision the vocabulary service and phone hotspot over native USB serial."""

from __future__ import annotations

import argparse
import base64
import getpass
import json
import sys
import time


FRAME_PREFIX = b"VOCAB_CONFIG_V1 "
ACK = b"VOCAB_CONFIG_V1 OK"


def serial_ports() -> list[str]:
    try:
        from serial.tools import list_ports  # type: ignore[import-not-found]
    except ImportError as exc:
        raise RuntimeError(
            "缺少 pyserial；请先激活 ESP-IDF 5.5.3 环境或安装 pyserial"
        ) from exc
    return sorted(port.device for port in list_ports.comports())


def choose_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    candidates = serial_ports()
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise RuntimeError("未发现 USB 串口；请确认设备已开机并重新连接")
    raise RuntimeError("发现多个串口，请使用 --port 指定：" + ", ".join(candidates))


def utf8_length_ok(name: str, value: str, maximum: int, *, allow_empty: bool) -> None:
    length = len(value.encode("utf-8"))
    if (not allow_empty and length == 0) or length > maximum:
        requirement = f"1..{maximum}" if not allow_empty else f"0..{maximum}"
        raise ValueError(f"{name} 的 UTF-8 长度必须为 {requirement} 字节")


def read_config() -> dict[str, str]:
    ssid = input("手机热点 Wi-Fi 名称（SSID）：").strip()
    password = getpass.getpass("手机热点密码（输入时不显示）：")
    base_url = input(
        "服务地址（例如 https://122.51.4.253/ielts）："
    ).strip().rstrip("/")
    pairing_code = getpass.getpass("TXyun 配对码（输入时不显示）：").strip()
    utf8_length_ok("SSID", ssid, 32, allow_empty=False)
    utf8_length_ok("Wi-Fi 密码", password, 64, allow_empty=True)
    utf8_length_ok("服务地址", base_url, 192, allow_empty=False)
    utf8_length_ok("配对码", pairing_code, 64, allow_empty=False)
    if not base_url.startswith("https://"):
        raise ValueError("服务地址必须使用 https://")
    return {
        "ssid": ssid,
        "password": password,
        "base_url": base_url,
        "pairing_code": pairing_code,
    }


def make_frame(config: dict[str, str]) -> bytearray:
    payload = json.dumps(config, ensure_ascii=False, separators=(",", ":")).encode()
    return bytearray(FRAME_PREFIX + base64.b64encode(payload) + b"\n")


def provision(port: str, frame: bytearray) -> None:
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as exc:
        raise RuntimeError("缺少 pyserial；请先激活 ESP-IDF 5.5.3 环境") from exc
    try:
        connection = serial.Serial(
            port=port,
            baudrate=115200,
            timeout=0.25,
            write_timeout=2,
            exclusive=True,
        )
    except (OSError, serial.SerialException) as exc:
        raise RuntimeError(f"无法打开串口 {port}：{exc}") from exc

    received = bytearray()
    try:
        connection.dtr = False
        connection.rts = False
        time.sleep(0.4)
        connection.reset_input_buffer()
        for _ in range(3):
            connection.write(frame)
            connection.flush()
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                block = connection.read(256)
                if block:
                    received.extend(block)
                    if ACK in received:
                        return
            time.sleep(0.2)
    finally:
        for index in range(len(received)):
            received[index] = 0
        connection.close()
    raise RuntimeError("设备未确认配置；请确认已刷入新固件且串口未被监视器占用")


def main() -> int:
    parser = argparse.ArgumentParser(description="配置 AI Passport 雅思背单词功能")
    parser.add_argument("--port", help="USB 串口，例如 COM3 或 /dev/ttyACM0")
    args = parser.parse_args()
    try:
        port = choose_port(args.port)
        config = read_config()
        frame = make_frame(config)
        config.clear()
        print(f"正在写入 {port}…")
        try:
            provision(port, frame)
        finally:
            for index in range(len(frame)):
                frame[index] = 0
        print("配置已保存。打开设备上的 IELTS 页面即可连接手机热点。")
        return 0
    except (RuntimeError, ValueError, KeyboardInterrupt) as exc:
        print(f"配置失败：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
