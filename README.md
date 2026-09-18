# CyberBeast CANable2-N32

基于 **N32H473** 的 USB–CAN/CAN FD 调试器固件，以及配套 **Qt6 上位机**（CyberBeast 协议调试工具）。

| 构件 | 版本 | 说明 |
|------|------|------|
| 固件 | **v30.0.5** | `Makefile` → `FW_VERSION`；Keil 见 `inc/git_version.h` |
| 上位机 | **30.0.5** | `host/CMakeLists.txt` / `host/package.sh` |

发行物（本仓库）：

- 固件：`release/canable2-n32-v30.0.5.bin`（及 `.hex`）
- 上位机：`host/dist/CyberBeast-ProtocolTool-Setup-30.0.5.exe`、`host/dist/CyberBeast-ProtocolTool-30.0.5-portable.zip`

---

## 功能概览

- USB CDC（VID/PID `16D0:117E`）二进制协议：命令 `AA AA`、数据 `AA AB`、响应 `AA AC`、告警 `AA AD`
- 经典 CAN / CAN FD（仲裁段 + 数据段独立波特率）
- 单帧 / 列表发送、周期发送、状态查询、DFU 升级
- 自动波特率扫描（锁定后 `AA AC 01`）、TEC/REC 上报（`AA AC 03`）
- 上位机：串口 CDC / libusb、通用 CAN / Simple / CyberBeast / 对象字典、固件更新

---

## 快速开始

### 固件（GNU Arm Embedded / make）

```bash
make -j
# 产物在 build/canable2-n32-v30.0.1-<gitrev>.bin
```

Keil：打开 `MDK-ARM/canable2.uvprojx`。

### 上位机（Windows）

```bash
cd host
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 打包安装包 + 便携 zip（需 NSIS）
./package.sh
```

详细说明见 [host/README.md](host/README.md)。

---

## USB 协议要点（v30.0.1）

完整说明见 **[docs/FIRMWARE_PROTOCOL.md](docs/FIRMWARE_PROTOCOL.md)**。

### 数据帧线序（收发相同）

```
AA AB | CTRL (1B) | ID (2B 或 4B, 大端) | DATA (0..64B) | CRC (2B 或 3B)
```

- **CTRL**：bit7 `FDF`、bit6 `BRS`、bit5 `RTR`、bit4 **`IDE`**（1=扩展 4B ID / 0=标准 2B）、bit3..0 `DLC`
- **CRC** 覆盖线序 **CTRL + ID + DATA**（不含帧头）
- 经典 CRC-15（2B）；FD CRC-17/21（3B）

> 旧版曾为 `ID|CTRL`；**v30.x 起为 `CTRL|ID`**，上位机与固件必须同版本配对，否则会出现 `ERR_USB_CRC`。

### 常用命令（`AA AA | CMD | VALUE[4] LE`）

| CMD | 名称 | 说明 |
|-----|------|------|
| 1 | SET_BITRATE | 仲裁段波特率 (Hz) |
| 2 / 3 | TX_START / TX_STOP | 启动 / 停止发送 |
| 4 | GET_STATUS | ASCII 状态行 |
| 5 | ENTER_DFU | 进入 N32 DFU |
| 6 | SET_INTERVAL | 周期间隔 1–1000 ms |
| 7 | SET_MODE | 0 单帧 / 1 列表 |
| 8 | SET_DATA_BITRATE | FD 数据段 2M/5M |
| 9 | SET_WORK_MODE | 0 CAN / 1 RS485 |
| 10 | GET_VERSION | 回 `AA AC 02 \| LEN \| ASCII` |

### 响应与告警

| 帧 | 含义 |
|----|------|
| `AA AC 01 \| bitrate LE` | 自动波特率锁定 |
| `AA AC 02 \| LEN \| TEXT` | 固件版本 |
| `AA AC 03 \| TEC_u32 \| REC_u32 \| CRC8` | 累计错误帧数（固件对硬件 TEC/REC 增量÷8 后饱和累加至 uint32；上位机直接显示） |
| `AA AD \| ERR_* \| DA AA \| CRC8` | 错误名（如 `ERR_CAN_TXFAIL`、`ERR_USB_CRC`） |

单帧发送默认硬件不重试（DAR）；连续无 ACK 会累计 TEC（常见 +8），并可能上报 `ERR_CAN_TXFAIL`。

---

## 目录结构

```
src/ inc/          固件源码
Drivers/           N32 HAL / CMSIS
host/              Qt6 上位机与安装脚本
docs/              协议文档、EDS 等
release/           已发布固件 bin/hex
```

---

## 许可证

本项目沿用原 CANable / 厂商 SDK 相关许可；应用层代码以仓库内声明为准。
