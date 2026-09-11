#!/usr/bin/env python3
"""DM 电机基本协议和运动功能验证工具。

默认使用 ``hardware_cpp`` 的 DM 经典 CAN 协议，经当前固件（CANable2-N32）的
二进制 USB 协议（AA AA 命令 / AA AB 数据 / AA AC 响应）与总线交互。工具包含
离线帧向量检查、在线只读参数/状态检查、设置零点后的状态复核，以及 CSP/PVT
位置、MIT 前馈力矩和 CSV 正反转速度稳定性测试。

MIT 测试默认正、反两个方向分别逐步增加前馈力矩，扫描范围为 ``0～0.5 Nm``；
当前只记录启动力矩，不自动判定范围是否合格。速度测试默认也执行
``+target_velocity`` 和 ``-target_velocity``。

参数通过运行时交互输入提供（不使用命令行传参）::

    python3 dm_function_validation.py

运行后按提示依次输入：运行模式、串口、仲裁段波特率、协议 profile、节点、
测试项、是否确认发送、报告路径等；直接回车即采用方括号中的默认值。
串口默认 COM16。

运动测试会驱动电机。开始前确认机械范围、负载和急停条件；在线测试需在
“确认发送主动/运动命令”处输入 yes，才会发送使能、模式切换和运动帧。
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import struct
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Optional, Sequence

try:
    import can as python_can  # type: ignore
except ImportError:
    python_can = None

try:
    import serial  # type: ignore
except ImportError:
    serial = None


PARAM_FRAME_ID = 0x7FF
READ_PARAM = 0x33
WRITE_PARAM = 0x55
QUERY_STATUS = 0xCC
K_CUR_MAX = 10.261194
DEFAULT_POS_MAX = 12.5
DEFAULT_VEL_MAX = 30.0
DEFAULT_TORQUE_MAX = 10.0

PARAM_IDS = tuple((set(range(0x00, 0x25)) - {0x0F, 0x24}) |
                  set(range(0x32, 0x38)) | {0x50, 0x51})

SPECIAL_PAYLOADS = {
    "enable": bytes.fromhex("FFFFFFFFFFFFFFFC"),
    "disable": bytes.fromhex("FFFFFFFFFFFFFFFD"),
    "zero": bytes.fromhex("FFFFFFFFFFFFFFFE"),
    "clear-error": bytes.fromhex("FFFFFFFFFFFFFFFB"),
}

STATUS_NAMES = {
    0x00: "NO_ERROR/DISABLED",
    0x01: "ENABLED",
    0x08: "OVER_VOLTAGE",
    0x09: "UNDER_VOLTAGE",
    0x0A: "OVER_CURRENT",
    0x0B: "MOS_TEMP_HIGH",
    0x0C: "MOTOR_COIL_TEMP_HIGH",
    0x0D: "COMM_LOST",
    0x0E: "OVER_LOAD",
}


@dataclass(frozen=True)
class Profile:
    name: str
    status_query_on_config_id: bool
    velocity_float64: bool
    pvt_current_normalized: bool
    read_response_codes: tuple[int, ...]
    write_response_codes: tuple[int, ...]
    write_response_id: Optional[int]
    read_requires_dlc8: bool


PROFILES = {
    "hardware_cpp": Profile(
        "hardware_cpp", False, False, False, (READ_PARAM,), (WRITE_PARAM,),
        None, True),
    "production_python": Profile(
        "production_python", True, True, True, (0x00, READ_PARAM),
        (0x00, WRITE_PARAM), 0x701, False),
}


@dataclass(frozen=True)
class Frame:
    can_id: int
    data: bytes
    is_fd: bool = False
    is_extended_id: bool = False
    timestamp: float = 0.0


@dataclass
class Status:
    status_code: int
    status: str
    node: int
    position_raw: int
    velocity_raw: int
    torque_raw: int
    position: float
    velocity: float
    torque: float
    motor_temp: int
    mos_temp: int
    timestamp: float = 0.0


@dataclass
class TestResult:
    name: str
    status: str
    detail: str = ""
    metrics: dict = field(default_factory=dict)


class ValidationError(RuntimeError):
    """协议、响应或运动判定失败。"""


def parse_nodes(value: str) -> list[int]:
    result = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        try:
            node = int(item, 0)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"非法节点 ID: {item}") from exc
        if not 1 <= node <= 0x7F:
            raise argparse.ArgumentTypeError("节点 ID 必须在 1..127")
        if node not in result:
            result.append(node)
    if not result:
        raise argparse.ArgumentTypeError("至少指定一个节点")
    return result


def finite(value: float, name: str) -> float:
    if not math.isfinite(value):
        raise ValueError(f"{name} 必须是有限数")
    return value


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def encode_unsigned(value: float, low: float, high: float, bits: int) -> int:
    finite(value, "控制参数")
    if low >= high:
        raise ValueError("映射范围非法")
    return int(math.floor((clamp(value, low, high) - low) *
                         ((1 << bits) - 1) / (high - low)))


def decode_unsigned(raw: int, low: float, high: float, bits: int) -> float:
    return low + raw * (high - low) / ((1 << bits) - 1)


def node_bytes(node: int) -> bytes:
    return struct.pack("<H", node)


def mode_frame(node: int, mode: int) -> Frame:
    if mode not in (1, 2, 3, 4):
        raise ValueError("control_mode 只能是 1..4")
    return Frame(PARAM_FRAME_ID, node_bytes(node) + bytes((WRITE_PARAM, 0x0A)) +
                 struct.pack("<I", mode))


def query_frame(node: int, profile: Profile) -> Frame:
    can_id = PARAM_FRAME_ID if profile.status_query_on_config_id else node
    return Frame(can_id, node_bytes(node) + bytes((QUERY_STATUS, 0x00)))


def read_frame(node: int, pid: int) -> Frame:
    return Frame(PARAM_FRAME_ID, node_bytes(node) + bytes((READ_PARAM, pid)))


def special_frame(node: int, name: str) -> Frame:
    try:
        data = SPECIAL_PAYLOADS[name]
    except KeyError as exc:
        raise ValueError(f"未知特殊命令: {name}") from exc
    return Frame(node, data)


def pack_mit(position: float, velocity: float, kp: float, kd: float,
             torque: float, pos_max: float, vel_max: float,
             torque_max: float) -> bytes:
    p = encode_unsigned(position, -pos_max, pos_max, 16)
    v = encode_unsigned(velocity, -vel_max, vel_max, 12)
    kp_raw = encode_unsigned(kp, 0.0, 500.0, 12)
    kd_raw = encode_unsigned(kd, 0.0, 5.0, 12)
    tq = encode_unsigned(torque, -torque_max, torque_max, 12)
    return bytes(((p >> 8) & 0xFF, p & 0xFF, (v >> 4) & 0xFF,
                  ((v & 0x0F) << 4) | (kp_raw >> 8), kp_raw & 0xFF,
                  (kd_raw >> 4) & 0xFF,
                  ((kd_raw & 0x0F) << 4) | (tq >> 8), tq & 0xFF))


def pack_pvt(position: float, max_velocity: float, current: float,
             profile: Profile) -> bytes:
    velocity_raw = int(clamp(max_velocity * 100.0, 0.0, 10000.0))
    if profile.pvt_current_normalized:
        current_raw = int(clamp(current * 10000.0, 0.0, 10000.0))
    else:
        current_raw = int(clamp(current / K_CUR_MAX * 10000.0, 0.0, 10000.0))
    return struct.pack("<fHH", position, velocity_raw, current_raw)


def pack_csp(position: float, max_velocity: float) -> bytes:
    """Encode hardware_cpp CSP: little-endian float position and max velocity."""
    finite(position, "CSP目标位置")
    finite(max_velocity, "CSP最大速度")
    if max_velocity < 0:
        raise ValueError("CSP最大速度不能为负数")
    return struct.pack("<ff", float(position), float(max_velocity))


def pack_velocity(velocity: float, profile: Profile) -> bytes:
    return struct.pack("<d" if profile.velocity_float64 else "<f", velocity)


def decode_status(node: int, frame: Frame, limits: tuple[float, float, float]) -> Status:
    if frame.can_id != (node | 0x700) or len(frame.data) != 8:
        raise ValidationError("状态反馈 ID/DLC 不符合 DM 协议")
    if frame.is_fd or frame.is_extended_id:
        raise ValidationError("状态反馈必须是标准经典 CAN 帧")
    data = frame.data
    if (data[0] & 0x0F) != node:
        raise ValidationError(
            f"状态反馈节点={data[0] & 0x0F}，期望={node}")
    pos_raw = (data[1] << 8) | data[2]
    vel_raw = (data[3] << 4) | (data[4] >> 4)
    tq_raw = ((data[4] & 0x0F) << 8) | data[5]
    pos_max, vel_max, torque_max = limits
    return Status(
        data[0] >> 4, STATUS_NAMES.get(data[0] >> 4, f"UNKNOWN(0x{data[0] >> 4:X})"),
        node, pos_raw, vel_raw, tq_raw,
        decode_unsigned(pos_raw, -pos_max, pos_max, 16),
        decode_unsigned(vel_raw, -vel_max, vel_max, 12),
        decode_unsigned(tq_raw, -torque_max, torque_max, 12),
        data[6], data[7], frame.timestamp)


class Transport:
    expected_is_fd = False

    def open(self):
        return self

    def close(self):
        return None

    def send(self, frame: Frame):
        raise NotImplementedError

    def poll(self, timeout: float) -> list[Frame]:
        raise NotImplementedError


class SocketCanTransport(Transport):
    def __init__(self, interface: str):
        self.interface = interface
        self.bus = None

    def open(self):
        if python_can is None:
            raise RuntimeError("缺少 python-can，请安装 python-can")
        try:
            self.bus = python_can.Bus(channel=self.interface,
                                      interface="socketcan", fd=False)
        except TypeError:
            self.bus = python_can.Bus(channel=self.interface,
                                      bustype="socketcan", fd=False)
        return self

    def close(self):
        if self.bus is not None:
            self.bus.shutdown()
            self.bus = None

    def send(self, frame: Frame):
        if self.bus is None:
            raise RuntimeError("SocketCAN 尚未打开")
        message = python_can.Message(
            arbitration_id=frame.can_id, data=frame.data,
            is_extended_id=False, is_fd=False)
        self.bus.send(message)

    def poll(self, timeout: float) -> list[Frame]:
        if self.bus is None:
            raise RuntimeError("SocketCAN 尚未打开")
        result = []
        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            remaining = max(0.0, deadline - time.monotonic())
            message = self.bus.recv(timeout=remaining)
            if message is None:
                break
            if not message.is_error_frame:
                result.append(Frame(message.arbitration_id, bytes(message.data),
                                    bool(message.is_fd),
                                    bool(message.is_extended_id),
                                    float(message.timestamp or time.time())))
            if time.monotonic() >= deadline:
                break
        return result


DLC2BYTES = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 8,
             9: 12, 10: 16, 11: 20, 12: 24, 13: 32, 14: 48, 15: 64}


class BinaryTransport(Transport):
    """当前固件（CANable2-N32）的二进制 USB 协议传输层。

    帧格式（与固件 inc/usb_proto.h 一致）：
        发送 数据帧 (host->MCU)：AA AB | ID(2B 大端) | CTRL(1B) | DATA | CRC
        发送 命令帧 (host->MCU)：AA AA | CMD(1B) | VALUE(4B 小端)
        回读 数据帧 (MCU->host)：AA AB | ID(2B) | CTRL(1B) | DATA | CRC
        响应 帧     (MCU->host)：AA AC | ID(1B) | VALUE(4B 小端)

    本脚本只发送标准 11 位、经典 CAN、DLC<=8 的帧。固件里一个数据帧只写入
    缓存、不会自动上总线，因此每次 send() 后紧接一条 TX_START(单次, 命令 2,
    value=0) 触发一次总线发送。DLC 与数据内容与原 SLCAN 版本保持一致。
    """

    CMD_SET_BITRATE = 1
    CMD_TX_START = 2
    CMD_TX_STOP = 3
    CMD_SET_MODE = 7

    def __init__(self, port: str, can_bitrate_hz: int = 1_000_000):
        self.port = port
        self.can_bitrate_hz = can_bitrate_hz
        self.ser = None
        self.buffer = bytearray()

    def open(self):
        if serial is None:
            raise RuntimeError("缺少 pyserial，请安装 pyserial")
        self.ser = serial.Serial(self.port, 115200, timeout=0.001)
        self.ser.reset_input_buffer()
        # 设置仲裁段波特率，并确保处于单帧模式（每次发送立即上总线）
        self._cmd(self.CMD_SET_BITRATE, self.can_bitrate_hz)
        self._cmd(self.CMD_SET_MODE, 0)
        return self

    def close(self):
        if self.ser is not None:
            try:
                self._cmd(self.CMD_TX_STOP, 0)   # 停止任何周期发送任务
            except Exception:
                pass
            finally:
                self.ser.close()
                self.ser = None

    def _cmd(self, cmd_id: int, value: int = 0):
        if self.ser is None:
            raise RuntimeError("二进制传输尚未打开")
        v = value & 0xFFFFFFFF
        self.ser.write(bytes((0xAA, 0xAA, cmd_id,
                              v & 0xFF, (v >> 8) & 0xFF,
                              (v >> 16) & 0xFF, (v >> 24) & 0xFF)))
        self.ser.flush()

    @staticmethod
    def _crc15(payload: bytes) -> int:
        """CRC-15/CAN，MSB-first、初值0，与固件实现一致。"""
        crc = 0
        poly = 0x4599
        top = 1 << 14
        for byte in payload:
            crc ^= (byte << 7)
            for _ in range(8):
                crc = ((crc << 1) ^ poly) if (crc & top) else (crc << 1)
        return crc & 0x7FFF

    def send(self, frame: Frame):
        if self.ser is None:
            raise RuntimeError("二进制传输尚未打开")
        if frame.is_fd or frame.is_extended_id or len(frame.data) > 8:
            raise ValueError("当前脚本只支持标准经典 CAN，DLC 最大为8")
        ctrl = len(frame.data) & 0x0F        # FDF=0 BRS=0 RTR=0, DLC=len
        payload = bytes(((frame.can_id >> 8) & 0xFF,
                         frame.can_id & 0xFF, ctrl)) + bytes(frame.data)
        crc = self._crc15(payload)
        wire = bytes((0xAA, 0xAB)) + payload + bytes(((crc >> 8) & 0xFF, crc & 0xFF))
        self.ser.write(wire)
        # 固件需显式 TX_START 才把缓存帧发上总线（单次发送）
        self.ser.write(bytes((0xAA, 0xAA, self.CMD_TX_START, 0, 0, 0, 0)))
        self.ser.flush()

    def poll(self, timeout: float) -> list[Frame]:
        if self.ser is None:
            raise RuntimeError("二进制传输尚未打开")
        result: list[Frame] = []
        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                self.buffer.extend(chunk)
                result.extend(self._drain())
            if time.monotonic() >= deadline:
                break
        return result

    def _drain(self) -> list[Frame]:
        """从接收缓冲解析回读帧（AA AB 数据帧 / AA AC 响应），其余字节跳过。"""
        frames: list[Frame] = []
        buf = self.buffer
        i = 0
        n = len(buf)
        while i < n:
            if buf[i] != 0xAA:
                i += 1
                continue
            if i + 1 >= n:
                break
            nxt = buf[i + 1]
            if nxt == 0xAB:                       # 回读数据帧
                if i + 5 > n:
                    break
                can_id = (buf[i + 2] << 8) | buf[i + 3]
                ctrl = buf[i + 4]
                length = DLC2BYTES.get(ctrl & 0x0F, 0)
                is_fd = bool(ctrl & 0x80)
                crc_n = 3 if is_fd else 2
                total = 2 + 2 + 1 + length + crc_n
                if i + total > n:
                    break
                data = bytes(buf[i + 5:i + 5 + length])
                frames.append(Frame(can_id, data, is_fd, False, time.time()))
                i += total
            elif nxt == 0xAC:                     # MCU->host 响应帧，跳过
                if i + 7 > n:
                    break
                i += 7
            else:
                i += 1
        self.buffer = bytearray(buf[i:])
        return frames


class MotorClient:
    def __init__(self, transport: Transport, node: int, profile: Profile,
                 timeout: float, retries: int):
        self.transport = transport
        self.node = node
        self.profile = profile
        self.timeout = timeout
        self.retries = retries
        self.limits = (DEFAULT_POS_MAX, DEFAULT_VEL_MAX, DEFAULT_TORQUE_MAX)
        self.params: dict[int, bytes] = {}

    def send(self, frame: Frame):
        if not 0 <= frame.can_id <= 0x7FF:
            raise ValueError("CAN ID 超出11位标准帧范围")
        if len(frame.data) > 8:
            raise ValueError("经典 CAN 数据长度不能超过8")
        self.transport.send(frame)

    def _wait(self, predicate: Callable[[Frame], object], timeout: Optional[float] = None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            for frame in self.transport.poll(min(0.005, max(0.0, deadline - time.monotonic()))):
                value = predicate(frame)
                if value is not None and value is not False:
                    return value
        return None

    def _valid_frame(self, frame: Frame) -> bool:
        return not frame.is_fd and not frame.is_extended_id

    def query_status(self, timeout: Optional[float] = None) -> Optional[Status]:
        response_id = self.node | 0x700
        for _ in range(self.retries):
            self.send(query_frame(self.node, self.profile))

            def match(frame: Frame):
                if not self._valid_frame(frame) or frame.can_id != response_id:
                    return None
                try:
                    return decode_status(self.node, frame, self.limits)
                except ValidationError:
                    return None

            status = self._wait(match, timeout)
            if status is not None:
                return status
        return None

    def read_param(self, pid: int, timeout: Optional[float] = None) -> Optional[bytes]:
        response_id = self.node | 0x700
        for _ in range(self.retries):
            self.send(read_frame(self.node, pid))

            def match(frame: Frame):
                if not self._valid_frame(frame) or frame.can_id != response_id:
                    return None
                if self.profile.read_requires_dlc8 and len(frame.data) != 8:
                    return None
                if not 5 <= len(frame.data) <= 8:
                    return None
                data = frame.data
                if (int.from_bytes(data[:2], "little") == self.node and
                        data[2] in self.profile.read_response_codes and
                        data[3] == pid):
                    return data[4:]
                return None

            raw = self._wait(match, timeout)
            if raw is not None:
                self.params[pid] = bytes(raw)
                return bytes(raw)
        return None

    def write_mode(self, mode: int, timeout: Optional[float] = None) -> Optional[bytes]:
        response_id = (self.profile.write_response_id
                       if self.profile.write_response_id is not None
                       else self.node | 0x700)
        expected = struct.pack("<I", mode)
        for _ in range(self.retries):
            self.send(mode_frame(self.node, mode))

            def match(frame: Frame):
                if (not self._valid_frame(frame) or frame.can_id != response_id or
                        len(frame.data) != 8):
                    return None
                data = frame.data
                if (int.from_bytes(data[:2], "little") == self.node and
                        data[2] in self.profile.write_response_codes and
                        data[3] == 0x0A):
                    return data[4:8]
                return None

            raw = self._wait(match, timeout)
            if raw is not None:
                self.params[0x0A] = bytes(raw)
                return bytes(raw)
        return None

    def special(self, name: str):
        self.send(special_frame(self.node, name))

    def decode_frame_status(self, frame: Frame) -> Optional[Status]:
        if frame.can_id != (self.node | 0x700) or len(frame.data) != 8:
            return None
        try:
            return decode_status(self.node, frame, self.limits)
        except ValidationError:
            return None


def format_frame(frame: Frame) -> str:
    return f"{frame.can_id:03X}#{frame.data.hex().upper()}"


def offline_vectors(profile: Profile) -> str:
    checks = []
    checks.append(format_frame(special_frame(4, "enable")) ==
                 "004#FFFFFFFFFFFFFFFC")
    checks.append(format_frame(mode_frame(4, 1)) ==
                 "7FF#0400550A01000000")
    checks.append(format_frame(query_frame(4, profile)) ==
                 ("7FF#0400CC00" if profile.status_query_on_config_id else
                  "004#0400CC00"))
    checks.append(format_frame(read_frame(4, 0x15)) == "7FF#04003315")
    checks.append(len(pack_mit(0.0, 0.0, 0.0, 0.0, 0.0,
                               12.5, 30.0, 10.0)) == 8)
    checks.append(len(pack_pvt(0.0, 0.2, 1.0, profile)) == 8)
    checks.append(pack_csp(1.0, 0.78) == bytes.fromhex("0000803F14AE473F"))
    checks.append(len(pack_velocity(1.0, profile)) == (8 if profile.velocity_float64 else 4))
    synthetic = Frame(0x704, bytes.fromhex("1400008007FF2020"))
    status = decode_status(4, synthetic, (12.5, 30.0, 10.0))
    checks.append(status.status_code == 1 and status.node == 4)
    if not all(checks):
        raise ValidationError("离线协议向量检查失败")
    return f"{profile.name}：特殊命令、模式、查询、参数和四种控制帧向量正确"


def status_detail(status: Status) -> str:
    return (f"code={status.status_code} {status.status}, pos={status.position:.5f}rad, "
            f"vel={status.velocity:.5f}rad/s, torque={status.torque:.5f}Nm")


def read_basic_params(client: MotorClient) -> tuple[int, list[str]]:
    errors = []
    for pid in PARAM_IDS:
        raw = client.read_param(pid)
        if raw is None:
            errors.append(f"0x{pid:02X}无响应")
            continue
        if len(raw) != 4:
            errors.append(f"0x{pid:02X}返回{len(raw)}字节")
            continue
        if pid in (0x15, 0x16, 0x17):
            value = struct.unpack("<f", raw)[0]
            if not math.isfinite(value) or value <= 0:
                errors.append(f"0x{pid:02X}值非法:{value}")
    if all(pid in client.params for pid in (0x15, 0x16, 0x17)):
        client.limits = tuple(struct.unpack("<f", client.params[pid])[0]
                              for pid in (0x15, 0x16, 0x17))
    return len(PARAM_IDS) - len(errors), errors


def run_rate_loop(client: MotorClient, duration: float, rate_hz: float,
                  command: Callable[[], None],
                  on_status: Callable[[Status, float], None]):
    period = 1.0 / rate_hz
    started = time.monotonic()
    next_send = started
    while time.monotonic() - started < duration:
        now = time.monotonic()
        if now >= next_send:
            command()
            next_send += period
            if next_send < now - period:
                next_send = now + period
        for frame in client.transport.poll(min(0.001, max(0.0, next_send - time.monotonic()))):
            status = client.decode_frame_status(frame)
            if status is not None:
                on_status(status, time.monotonic() - started)
                if status.status_code not in (0, 1):
                    raise ValidationError(f"运动期间故障: {status_detail(status)}")
        time.sleep(0.0002)


def stop_motor(client: MotorClient, rate_hz: float, hold: float = 0.2):
    try:
        client.special("disable")
    except Exception:
        pass
    time.sleep(max(0.0, hold))


def zero_test(client: MotorClient, args) -> dict:
    """设置当前位置为零，等待后主动查询状态确认零点已生效。"""
    safe_disable(client)
    client.special("zero")
    time.sleep(args.zero_wait)
    status = client.query_status()
    if status is None:
        raise ValidationError("设置零点后状态查询无响应")
    if status.status_code not in (0, 1):
        raise ValidationError(f"设置零点后状态故障:{status_detail(status)}")
    if abs(status.position) > args.zero_tolerance:
        raise ValidationError(
            f"设置零点后位置{status.position:.6f}rad超过"
            f"容差{args.zero_tolerance:.6f}rad")
    print(f"  node={client.node} 零点复核 position={status.position:.6f}rad "
          f"(wait={args.zero_wait:.1f}s)")
    return {"wait_s": args.zero_wait,
            "position_rad": status.position,
            "tolerance_rad": args.zero_tolerance,
            "status_code": status.status_code}


def pvt_test(client: MotorClient, args) -> dict:
    initial = client.query_status()
    if initial is None:
        raise ValidationError("PVT测试前无法读取状态")
    if initial.status_code not in (0, 1):
        raise ValidationError(f"PVT测试前存在故障: {status_detail(initial)}")
    pos_max, _, _ = client.limits
    offset = min(abs(args.pvt_offset), max(0.01, pos_max * 0.25))
    target_a = clamp(initial.position + offset, -pos_max + 0.02, pos_max - 0.02)
    target_b = clamp(initial.position - offset, -pos_max + 0.02, pos_max - 0.02)
    if abs(target_a - target_b) < 0.02:
        raise ValidationError("当前位置附近没有足够的 PVT 安全运动空间")
    if client.write_mode(4) != struct.pack("<I", 4):
        raise ValidationError("切换 PVT 模式无确认")
    client.special("enable")
    time.sleep(0.05)
    arrivals: dict[str, list[float]] = {"a": [], "b": []}
    ripples: dict[str, list[float]] = {"a": [], "b": []}
    targets = (("a", target_a), ("b", target_b))
    for cycle in range(args.pvt_cycles):
        for label, target in targets:
            samples: list[Status] = []
            consecutive = 0
            approach_started = time.monotonic()

            def command():
                client.send(Frame(client.node | 0x300,
                                  pack_pvt(target, args.pvt_max_velocity,
                                           args.pvt_current, client.profile)))

            def collect(status: Status, _elapsed: float):
                nonlocal consecutive
                samples.append(status)
                if (abs(status.position - target) <= args.position_tolerance and
                        abs(status.velocity) <= args.arrival_velocity_tolerance):
                    consecutive += 1
                else:
                    consecutive = 0

            run_rate_loop(client, min(args.pvt_approach_time, args.motion_time),
                          args.rate_hz, command, collect)
            if consecutive < args.arrival_consecutive:
                raise ValidationError(
                    f"PVT目标{label}未到位: samples={len(samples)}, "
                    f"last={status_detail(samples[-1]) if samples else '无反馈'}")
            settle: list[Status] = []
            run_rate_loop(client, args.pvt_settle_time, args.rate_hz, command,
                          lambda status, _elapsed: settle.append(status))
            if not settle:
                raise ValidationError(f"PVT目标{label}稳态阶段无反馈")
            arrivals[label].append(samples[-1].position)
            positions = [sample.position for sample in settle]
            ripples[label].append(max(positions) - min(positions))
            elapsed = time.monotonic() - approach_started
            print(f"  node={client.node} PVT {cycle + 1}/{args.pvt_cycles} {label} "
                  f"target={target:.5f} arrival={samples[-1].position:.5f} "
                  f"ripple={ripples[label][-1]:.6f}rad time={elapsed:.2f}s")
    stop_motor(client, args.rate_hz)
    repeat_ranges = {
        label: (max(values) - min(values) if values else float("inf"))
        for label, values in arrivals.items()
    }
    max_repeat = max(repeat_ranges.values())
    max_ripple = max(max(values) for values in ripples.values())
    if max_repeat > args.pvt_repeat_tolerance:
        raise ValidationError(
            f"重复定位极差{max_repeat:.6f}rad超过{args.pvt_repeat_tolerance:.6f}rad")
    if max_ripple > args.pvt_ripple_tolerance:
        raise ValidationError(
            f"到位后位置跳动峰峰值{max_ripple:.6f}rad超过"
            f"{args.pvt_ripple_tolerance:.6f}rad")
    return {"target_a": target_a, "target_b": target_b,
            "repeat_range_rad": repeat_ranges, "max_repeat_range_rad": max_repeat,
            "max_settle_ripple_rad": max_ripple}


def csp_test(client: MotorClient, args) -> dict:
    """Exercise hardware CSP by commanding the existing position reference to 0 rad."""
    initial = client.query_status()
    if initial is None:
        raise ValidationError("CSP测试前无法读取状态")
    if initial.status_code not in (0, 1):
        raise ValidationError(f"CSP测试前存在故障: {status_detail(initial)}")
    if not math.isfinite(args.csp_max_velocity) or args.csp_max_velocity <= 0:
        raise ValidationError("CSP最大速度必须为正数")
    pos_max, vel_max, _ = client.limits
    if args.csp_max_velocity > vel_max:
        raise ValidationError(
            f"CSP最大速度{args.csp_max_velocity}超过vel_max={vel_max}")
    if not math.isfinite(pos_max) or pos_max <= 0:
        raise ValidationError(f"CSP位置范围非法: pos_max={pos_max}")
    target = 0.0
    if client.write_mode(2) != struct.pack("<I", 2):
        raise ValidationError("切换 CSP 模式无确认")
    client.special("enable")
    time.sleep(0.05)

    samples: list[Status] = []
    consecutive = 0

    def command():
        client.send(Frame(client.node | 0x100,
                          pack_csp(target, args.csp_max_velocity)))

    def collect(status: Status, _elapsed: float):
        nonlocal consecutive
        samples.append(status)
        if (abs(status.position - target) <= args.position_tolerance and
                abs(status.velocity) <= args.arrival_velocity_tolerance):
            consecutive += 1
        else:
            consecutive = 0

    run_rate_loop(client, min(args.csp_approach_time, args.motion_time),
                  args.rate_hz, command, collect)
    if consecutive < args.arrival_consecutive:
        raise ValidationError(
            f"CSP目标0rad未到位: samples={len(samples)}, "
            f"last={status_detail(samples[-1]) if samples else '无反馈'}")
    settle: list[Status] = []
    run_rate_loop(client, args.csp_settle_time, args.rate_hz, command,
                  lambda status, _elapsed: settle.append(status))
    if not settle:
        raise ValidationError("CSP目标0rad稳态阶段无反馈")
    error = settle[-1].position - target
    ripple = max(s.position for s in settle) - min(s.position for s in settle)
    print(f"  node={client.node} CSP target=0.00000 arrival={samples[-1].position:.5f} "
          f"error={error:+.6f} ripple={ripple:.6f}rad")
    return {"max_velocity_rad_s": args.csp_max_velocity,
            "target_rad": target,
            "arrival_rad": samples[-1].position,
            "final_error_rad": error,
            "ripple_rad": ripple,
            "samples": len(settle)}


def mit_direction_test(client: MotorClient, args, sign: float) -> dict:
    baseline = client.query_status()
    if baseline is None:
        raise ValidationError("MIT测试前无法读取状态")
    if baseline.status_code not in (0, 1):
        raise ValidationError(f"MIT测试前存在故障: {status_detail(baseline)}")
    if client.write_mode(1) != struct.pack("<I", 1):
        raise ValidationError("切换 MIT 模式无确认")
    client.special("enable")
    time.sleep(0.05)
    torque = 0.0
    detected = None
    peak_displacement = 0.0
    peak_velocity = 0.0
    for _ in range(int(math.ceil(args.mit_max_torque / args.mit_step))):
        torque = min(args.mit_max_torque, torque + args.mit_step)
        samples: list[Status] = []

        def command():
            data = pack_mit(baseline.position, 0.0, 0.0, 0.0,
                            sign * torque, *client.limits)
            client.send(Frame(client.node, data))

        run_rate_loop(client, args.mit_hold_time, args.rate_hz, command,
                      lambda status, _elapsed: samples.append(status))
        if samples:
            displacement = max(abs(status.position - baseline.position)
                               for status in samples)
            velocity = max(abs(status.velocity) for status in samples)
            peak_displacement = max(peak_displacement, displacement)
            peak_velocity = max(peak_velocity, velocity)
            if (displacement >= args.mit_motion_position and
                    velocity >= args.mit_motion_velocity):
                detected = torque
                break
        if peak_displacement >= args.mit_max_displacement:
            raise ValidationError(
                f"MIT位移保护触发: {peak_displacement:.5f}rad")
    client.special("disable")
    direction_name = "正" if sign > 0 else "反"
    if detected is None:
        if args.mit_check_range:
            raise ValidationError(
                f"MIT{direction_name}向在最大{args.mit_max_torque:.3f}Nm内未检测到转动，"
                "无法满足启动力矩范围判定")
        print(f"  node={client.node} MIT{direction_name}向在扫描范围内未检测到转动，"
              "仅记录，不判定")
        return {
            "direction": direction_name,
            "start_torque_nm": None,
            "motion_detected": False,
            "peak_displacement_rad": peak_displacement,
            "peak_velocity_rad_s": peak_velocity,
            "range_nm": [args.mit_min_torque, args.mit_max_torque],
        }
    if (args.mit_check_range and
            (detected < args.mit_min_torque or detected > args.mit_max_torque)):
        raise ValidationError(
            f"MIT{direction_name}向启动力矩{detected:.3f}Nm不在 "
            f"[{args.mit_min_torque:.3f}, {args.mit_max_torque:.3f}]Nm")
    return {"direction": direction_name, "start_torque_nm": detected,
            "peak_displacement_rad": peak_displacement,
            "peak_velocity_rad_s": peak_velocity,
            "range_nm": [args.mit_min_torque, args.mit_max_torque]}


def wait_for_mit_stop(client: MotorClient, args) -> dict:
    """Keep the full post-MIT interval and prove the motor has stopped."""
    started = time.monotonic()
    deadline = started + args.mit_stop_wait
    samples: list[Status] = []
    missed = 0
    consecutive = 0
    while True:
        status = client.query_status()
        if status is None:
            missed += 1
            consecutive = 0
        else:
            if status.status_code not in (0, 1):
                raise ValidationError(
                    f"MIT停止确认期间状态故障: {status_detail(status)}")
            samples.append(status)
            if abs(status.velocity) <= args.mit_stop_velocity_tolerance:
                consecutive += 1
            else:
                consecutive = 0
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(args.mit_stop_poll_interval, remaining))
    if not samples:
        raise ValidationError(
            f"MIT停止确认{args.mit_stop_wait:.1f}s内状态查询无响应")
    if consecutive < args.mit_stop_consecutive:
        last = samples[-1]
        raise ValidationError(
            f"MIT停止确认{args.mit_stop_wait:.1f}s后未确认停止: "
            f"末速度={last.velocity:+.5f}rad/s，"
            f"要求连续{args.mit_stop_consecutive}帧"
            f"|velocity|<={args.mit_stop_velocity_tolerance:.5f}rad/s")
    peak_velocity = max(abs(status.velocity) for status in samples)
    last = samples[-1]
    print(f"  MIT停止确认{args.mit_stop_wait:.1f}s，已确认停止: "
          f"last_vel={last.velocity:+.5f}rad/s, "
          f"stable={consecutive}, missed={missed}")
    return {"wait_s": args.mit_stop_wait,
            "samples": len(samples),
            "missed_queries": missed,
            "peak_velocity_rad_s": peak_velocity,
            "last_velocity_rad_s": last.velocity,
            "consecutive_stopped_samples": consecutive,
            "velocity_tolerance_rad_s": args.mit_stop_velocity_tolerance}


def mit_test(client: MotorClient, args) -> dict:
    results = []
    directions = (1.0,) if args.mit_direction == "positive" else (-1.0,) if args.mit_direction == "negative" else (1.0, -1.0)
    for index, sign in enumerate(directions):
        result = mit_direction_test(client, args, sign)
        results.append(result)
        start_torque = result["start_torque_nm"]
        measured = (f"{start_torque:.3f}Nm"
                    if start_torque is not None else "未检测到")
        print(f"  node={client.node} MIT{result['direction']}向启动力矩 {measured}")
        result["stop_confirmation"] = wait_for_mit_stop(client, args)
    return {
        "directions": results,
        "scan_range_nm": [args.mit_min_torque, args.mit_max_torque],
        "range_check_enabled": args.mit_check_range,
    }


def velocity_phase(client: MotorClient, args, target: float) -> dict:
    if abs(target) > client.limits[1]:
        raise ValidationError(
            f"CSV目标速度{target}超过vel_max={client.limits[1]}")
    if client.write_mode(3) != struct.pack("<I", 3):
        raise ValidationError("切换 CSV 模式无确认")
    client.special("enable")
    samples: list[float] = []
    started = time.monotonic()
    total = args.velocity_ramp_time + args.velocity_warmup_time + args.velocity_sample_time

    def command():
        elapsed = time.monotonic() - started
        ramp = min(1.0, elapsed / max(args.velocity_ramp_time, 1e-6))
        client.send(Frame(client.node | 0x200,
                          pack_velocity(target * ramp, client.profile)))

    def collect(status: Status, elapsed: float):
        if elapsed >= args.velocity_ramp_time + args.velocity_warmup_time:
            samples.append(status.velocity)

    try:
        run_rate_loop(client, total, args.rate_hz, command, collect)
    finally:
        client.send(Frame(client.node | 0x200, pack_velocity(0.0, client.profile)))
        time.sleep(0.2)
    if not samples:
        raise ValidationError(f"CSV目标{target:+.3f}rad/s无有效反馈")
    mean = statistics.fmean(samples)
    ripple = max(samples) - min(samples)
    stdev = statistics.pstdev(samples) if len(samples) > 1 else 0.0
    if abs(mean - target) > args.velocity_tracking_tolerance:
        raise ValidationError(
            f"CSV目标{target:+.3f}rad/s平均反馈{mean:+.3f}偏差过大")
    if ripple > args.velocity_ripple_tolerance:
        raise ValidationError(
            f"CSV目标{target:+.3f}rad/s波动峰峰值{ripple:.6f}超过"
            f"{args.velocity_ripple_tolerance:.6f}")
    return {"target_rad_s": target, "samples": len(samples),
            "mean_rad_s": mean, "min_rad_s": min(samples),
            "max_rad_s": max(samples), "ripple_rad_s": ripple,
            "stdev_rad_s": stdev}


def velocity_test(client: MotorClient, args) -> dict:
    results = []
    failures = []
    try:
        for target in (abs(args.target_velocity), -abs(args.target_velocity)):
            try:
                result = velocity_phase(client, args, target)
                results.append(result)
                print(f"  node={client.node} CSV {target:+.3f}rad/s "
                      f"mean={result['mean_rad_s']:+.5f} "
                      f"ripple={result['ripple_rad_s']:.6f}")
            except Exception as exc:
                failures.append(f"{target:+.3f}rad/s: {exc}")
                results.append({"target_rad_s": target, "error": str(exc)})
                print(f"  node={client.node} CSV {target:+.3f}rad/s FAIL: {exc}")
    finally:
        try:
            client.special("disable")
        except Exception:
            pass
    if failures:
        raise ValidationError("CSV双向测试失败: " + "；".join(failures))
    return {"directions": results}


def safe_disable(client: MotorClient):
    """Best-effort stop used on every online test boundary."""
    try:
        client.special("disable")
    except Exception:
        pass


def safe_pvt_test(client: MotorClient, args) -> dict:
    try:
        return pvt_test(client, args)
    finally:
        safe_disable(client)


def safe_csp_test(client: MotorClient, args) -> dict:
    try:
        return csp_test(client, args)
    finally:
        safe_disable(client)


def safe_zero_test(client: MotorClient, args) -> dict:
    try:
        return zero_test(client, args)
    finally:
        safe_disable(client)


def safe_mit_test(client: MotorClient, args) -> dict:
    try:
        return mit_test(client, args)
    finally:
        safe_disable(client)


class Runner:
    def __init__(self):
        self.results: list[TestResult] = []

    def run(self, name: str, function: Callable[[], object]):
        print(f"\n[开始] {name}", flush=True)
        try:
            detail = function()
            metrics = detail if isinstance(detail, dict) else {}
            text = "" if isinstance(detail, dict) else str(detail or "")
            result = TestResult(name, "PASS", text, metrics)
        except Exception as exc:
            result = TestResult(name, "FAIL", str(exc), {})
        self.results.append(result)
        print(f"[{result.status}] {name}")
        if result.detail:
            print(f"       {result.detail}")
        return result

    def skip(self, name: str, reason: str):
        result = TestResult(name, "SKIP", reason, {})
        self.results.append(result)
        print(f"[SKIP] {name}: {reason}")

    def write(self, path: Path, profile: str, args):
        summary = {key: sum(item.status == key for item in self.results)
                   for key in ("PASS", "FAIL", "SKIP")}
        arguments = {
            key: (str(value) if isinstance(value, Path) else value)
            for key, value in vars(args).items()
        }
        payload = {"profile": profile, "summary": summary,
                   "results": [asdict(item) for item in self.results],
                   "arguments": arguments}
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                        encoding="utf-8")
        return summary


DEFAULT_PORT = "COM16"


def build_transport(args) -> Transport:
    if args.transport == "socketcan":
        return SocketCanTransport(args.interface or "can0")
    if args.interface:
        return SocketCanTransport(args.interface)
    # 默认（含 --transport slcan/auto）：使用当前固件的二进制协议
    return BinaryTransport(args.port or DEFAULT_PORT, args.can_bitrate)


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="DM 电机基本协议和运动功能验证")
    p.add_argument("--transport", choices=("auto", "socketcan", "slcan"), default="auto")
    p.add_argument("--interface", help="SocketCAN 接口，默认can0")
    p.add_argument("--port", default="COM16", help="CAN 设备串口，默认 COM16")
    p.add_argument("--serial-baud", type=int, default=1_000_000)
    p.add_argument("--can-bitrate", type=int, default=1_000_000,
                   help="总线仲裁段波特率(Hz)，默认1M")
    p.add_argument("--profile", choices=tuple(PROFILES), default="hardware_cpp")
    p.add_argument("--nodes", type=parse_nodes, default=[4])
    p.add_argument("--timeout", type=float, default=0.05)
    p.add_argument("--retries", type=int, default=2)
    p.add_argument("--offline", action="store_true", help="仅执行离线协议帧检查")
    p.add_argument("--basic", action="store_true", help="在线基本协议检查")
    p.add_argument("--zero", action="store_true", help="设置零点后等待并查询位置")
    p.add_argument("--pvt", action="store_true", help="PVT重复定位和到位跳动")
    p.add_argument("--mit", action="store_true", help="MIT前馈启动力矩")
    p.add_argument("--velocity", action="store_true", help="CSV正反转速度稳定性")
    p.add_argument("--csp", action="store_true", help="CSP位置速度模式，目标位置0rad")
    p.add_argument("--all", action="store_true", help="执行基本协议、零点、CSP、PVT、MIT、CSV")
    p.add_argument("--yes", action="store_true", help="确认发送主动控制和运动消息")
    p.add_argument("--report", type=Path, default=Path("dm_function_validation_report.json"))
    p.add_argument("--rate-hz", type=float, default=250.0)
    p.add_argument("--pvt-cycles", type=int, default=5)
    p.add_argument("--pvt-offset", type=float, default=0.2)
    p.add_argument("--pvt-max-velocity", type=float, default=0.3)
    p.add_argument("--pvt-current", type=float, default=1.0)
    p.add_argument("--pvt-approach-time", type=float, default=5.0)
    p.add_argument("--pvt-settle-time", type=float, default=1.0)
    p.add_argument("--csp-max-velocity", type=float, default=0.78,
                   help="CSP目标位置0rad的最大速度(rad/s)，默认0.78")
    p.add_argument("--csp-approach-time", type=float, default=5.0)
    p.add_argument("--csp-settle-time", type=float, default=1.0)
    p.add_argument("--motion-time", type=float, default=5.0)
    p.add_argument("--position-tolerance", type=float, default=0.01)
    p.add_argument("--zero-wait", type=float, default=3.0,
                   help="设置零点后的等待时间(s)，默认3")
    p.add_argument("--zero-tolerance", type=float, default=0.01,
                   help="设置零点后位置绝对值允许误差(rad)，默认0.01")
    p.add_argument("--arrival-velocity-tolerance", type=float, default=0.05)
    p.add_argument("--arrival-consecutive", type=int, default=5)
    p.add_argument("--pvt-repeat-tolerance", type=float, default=0.01)
    p.add_argument("--pvt-ripple-tolerance", type=float, default=0.01)
    p.add_argument(
        "--mit-direction", choices=("positive", "negative", "both"),
        default="both",
        help="MIT前馈力矩测试方向，默认正反转都测试")
    p.add_argument("--mit-min-torque", type=float, default=0.0)
    p.add_argument("--mit-max-torque", type=float, default=0.5)
    p.add_argument(
        "--mit-check-range", action="store_true",
        help="启用MIT启动力矩范围合格判定（默认只记录，不判定）")
    p.add_argument("--mit-step", type=float, default=0.05)
    p.add_argument("--mit-hold-time", type=float, default=0.5)
    p.add_argument("--mit-motion-position", type=float, default=0.03)
    p.add_argument("--mit-motion-velocity", type=float, default=0.1)
    p.add_argument("--mit-max-displacement", type=float, default=0.2)
    p.add_argument("--mit-stop-wait", type=float, default=5.0,
                   help="每个MIT方向测试结束后的停止确认时长(s)，默认5")
    p.add_argument("--mit-stop-poll-interval", type=float, default=0.1,
                   help="MIT停止确认的状态查询周期(s)，默认0.1")
    p.add_argument("--mit-stop-velocity-tolerance", type=float, default=0.05,
                   help="MIT停止确认速度阈值(rad/s)，默认0.05")
    p.add_argument("--mit-stop-consecutive", type=int, default=5,
                   help="MIT停止确认所需连续低速状态帧数，默认5")
    p.add_argument("--target-velocity", type=float, default=1.0)
    p.add_argument("--velocity-ramp-time", type=float, default=1.0)
    p.add_argument("--velocity-warmup-time", type=float, default=2.0)
    p.add_argument("--velocity-sample-time", type=float, default=5.0)
    p.add_argument("--velocity-tracking-tolerance", type=float, default=0.2)
    p.add_argument("--velocity-ripple-tolerance", type=float, default=0.1)
    return p


def _prompt(label: str, default: str = "") -> str:
    """读取一行输入；空输入返回默认值。"""
    suffix = f" [{default}]" if default != "" else ""
    try:
        raw = input(f"{label}{suffix}: ").strip()
    except EOFError:
        raw = ""
    return raw if raw else default


def _prompt_int(label: str, default: int) -> int:
    while True:
        raw = _prompt(label, str(default))
        try:
            return int(raw, 0)
        except ValueError:
            print("  请输入整数。")


def _prompt_yesno(label: str, default: bool = False) -> bool:
    while True:
        raw = _prompt(label + " (yes/no)", "Y" if default else "N").strip().lower()
        if raw in ("y", "yes", "是", "1"):
            return True
        if raw in ("n", "no", "否", "0"):
            return False
        print("  请输入 yes 或 no。")


def _prompt_choice(label: str, choices: Sequence[str], default: str) -> str:
    while True:
        raw = _prompt(label + " (" + "/".join(choices) + ")", default).strip()
        if raw in choices:
            return raw
        print("  可选值: " + ", ".join(choices))


def interactive_args():
    """通过运行时输入构建参数（不使用命令行传参）。"""
    args = parser().parse_args([])   # 仅取各参数默认值，不读取命令行

    print("=== DM 电机功能验证 —— 交互式参数输入（回车采用默认值）===")
    mode = _prompt("运行模式  1=离线协议检查  2=在线功能测试", "2").strip()
    args.offline = (mode == "1")

    if args.offline:
        args.report = Path(_prompt("报告路径", str(args.report)))
        return args

    args.port = _prompt("串口", args.port or DEFAULT_PORT)
    args.can_bitrate = _prompt_int("仲裁段波特率(Hz)", args.can_bitrate)
    args.profile = _prompt_choice("协议 profile", tuple(PROFILES), args.profile)

    while True:
        nodes_raw = _prompt("节点 ID(逗号分隔)", ",".join(str(n) for n in args.nodes))
        try:
            args.nodes = parse_nodes(nodes_raw)
            break
        except argparse.ArgumentTypeError as exc:
            print(f"  非法节点: {exc}")

    tests = _prompt("测试项(basic,zero,csp,pvt,mit,velocity) 或 all", "all").lower()
    selected = {t.strip() for t in tests.replace("，", ",").split(",") if t.strip()}
    if "all" in selected or not selected:
        args.basic = args.zero = args.csp = args.pvt = args.mit = args.velocity = True
    else:
        args.basic = "basic" in selected
        args.zero = "zero" in selected
        args.csp = "csp" in selected
        args.pvt = "pvt" in selected
        args.mit = "mit" in selected
        args.velocity = "velocity" in selected
        if not any((args.basic, args.zero, args.csp, args.pvt, args.mit, args.velocity)):
            print("  未识别测试项，改为执行全部。")
            args.basic = args.zero = args.csp = args.pvt = args.mit = args.velocity = True

    args.yes = _prompt_yesno("确认发送主动/运动命令", False)
    args.report = Path(_prompt("报告路径", str(args.report)))
    return args


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = interactive_args()
    profile = PROFILES[args.profile]
    runner = Runner()
    if not any((args.basic, args.zero, args.csp, args.pvt, args.mit, args.velocity, args.all)):
        args.all = True
    if args.all:
        args.basic = args.zero = args.csp = args.pvt = args.mit = args.velocity = True
    try:
        if args.rate_hz <= 0 or args.retries <= 0 or args.pvt_cycles <= 0:
            raise ValueError("rate-hz、retries、pvt-cycles 必须为正数")
        if args.mit_min_torque < 0 or args.mit_max_torque <= args.mit_min_torque:
            raise ValueError("MIT力矩范围必须满足 0 <= min < max")
        if (not math.isfinite(args.mit_stop_wait) or
                not math.isfinite(args.mit_stop_poll_interval) or
                not math.isfinite(args.mit_stop_velocity_tolerance) or
                args.mit_stop_wait <= 0 or args.mit_stop_poll_interval <= 0 or
                args.mit_stop_velocity_tolerance < 0 or args.mit_stop_consecutive <= 0):
            raise ValueError(
                "MIT停止确认参数必须满足：等待/查询周期为正、速度阈值非负、连续帧数为正")
        if (not math.isfinite(args.zero_wait) or
                not math.isfinite(args.zero_tolerance) or
                args.zero_wait < 0 or args.zero_tolerance < 0):
            raise ValueError("zero-wait、zero-tolerance 必须是非负有限数")
        if args.offline:
            runner.run(f"离线协议向量/{args.profile}", lambda: offline_vectors(profile))
            summary = runner.write(args.report, args.profile, args)
            print(f"\n结论: {'PASS' if summary['FAIL'] == 0 else 'FAIL'}，报告: {args.report}")
            return 0 if summary["FAIL"] == 0 else 1
        if not args.yes:
            raise ValueError("在线测试包含主动命令，需在“确认发送主动/运动命令”处输入 yes")
        runner.run(f"离线协议向量/{args.profile}", lambda: offline_vectors(profile))
        transport = build_transport(args)
        transport.open()
        clients = [MotorClient(transport, node, profile, args.timeout, args.retries)
                    for node in args.nodes]
        try:
            for client in clients:
                if args.basic:
                    def basic(client=client):
                        count, errors = read_basic_params(client)
                        status = client.query_status()
                        if status is None:
                            errors.append("状态查询无响应")
                        elif status.status_code not in (0, 1):
                            errors.append(f"状态故障:{status_detail(status)}")
                        client.special("clear-error")
                        client.special("enable")
                        time.sleep(0.05)
                        enabled = client.query_status()
                        if enabled is not None and enabled.status_code not in (0, 1):
                            errors.append(f"使能后状态故障:{status_detail(enabled)}")
                        mode_checks = {}
                        for mode in (1, 2, 3, 4):
                            ack = client.write_mode(mode)
                            mode_checks[str(mode)] = ack.hex(" ").upper() if ack else None
                            if ack != struct.pack("<I", mode):
                                errors.append(f"control_mode={mode}写入无确认")
                        # 基本检查结束后回到 MIT，避免把后续人工操作留在速度/PVT模式。
                        restore = client.write_mode(1)
                        if restore != struct.pack("<I", 1):
                            errors.append("control_mode恢复MIT失败")
                        client.special("disable")
                        if enabled is None:
                            errors.append("使能后状态无响应")
                        if errors:
                            raise ValidationError("；".join(errors))
                        return {"parameters_passed": count,
                                "parameters_total": len(PARAM_IDS),
                                "mode_checks": mode_checks,
                                "enabled_status": enabled.status_code if enabled else None}
                    def basic_safe():
                        try:
                            return basic()
                        finally:
                            safe_disable(client)
                    runner.run(f"node {client.node} 基本协议", basic_safe)
                if args.pvt:
                    runner.run(f"node {client.node} PVT重复定位", lambda client=client: safe_pvt_test(client, args))
                else:
                    runner.skip(f"node {client.node} PVT重复定位", "未选择PVT测试")
                if args.csp:
                    runner.run(f"node {client.node} CSP位置速度模式", lambda client=client: safe_csp_test(client, args))
                else:
                    runner.skip(f"node {client.node} CSP位置速度模式", "未选择CSP测试")
                if args.mit:
                    runner.run(f"node {client.node} MIT前馈力矩", lambda client=client: safe_mit_test(client, args))
                else:
                    runner.skip(f"node {client.node} MIT前馈力矩", "未选择MIT测试")
                if args.velocity:
                    runner.run(f"node {client.node} CSV正反转速度", lambda client=client: velocity_test(client, args))
                else:
                    runner.skip(f"node {client.node} CSV正反转速度", "未选择速度测试")
                if args.zero:
                    runner.run(f"node {client.node} 零点设置后位置复核",
                               lambda client=client: safe_zero_test(client, args))
                else:
                    runner.skip(f"node {client.node} 零点设置后位置复核", "未选择零点测试")
        finally:
            for client in clients:
                try:
                    client.special("disable")
                except Exception:
                    pass
            transport.close()
        summary = runner.write(args.report, args.profile, args)
        print(f"\n总结: PASS={summary['PASS']} FAIL={summary['FAIL']} SKIP={summary['SKIP']}")
        print(f"报告: {args.report}")
        return 0 if summary["FAIL"] == 0 else 1
    except KeyboardInterrupt:
        print("\n已中断，电机应保持失能。", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"验证失败: {exc}", file=sys.stderr)
        try:
            runner.write(args.report, args.profile, args)
        except Exception:
            pass
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
