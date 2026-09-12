# 更新日志 (CHANGELOG) — CANable2-N32 固件

本仓库为 CANable2-N32 **固件**（N32H473 移植版）。上位机见另一分支 `host-v10.0.1`（本分支不含上位机代码）。

| 构件 | 版本 | 来源 |
|---|---|---|
| 固件 | **v20.0.1** | `Makefile` 的 `FW_VERSION`；Keil 构建见 `inc/git_version.h` |

---

## 固件 v20.0.1

### 新增
- USB 命令 **`8` = SET_DATA_BITRATE**：设置 **CAN FD 数据域波特率**（`2000000` / `5000000` Hz）。
  经 `can_apply_data_bitrate_hz()` **热重配**（同命令 1：`can_hw_stop → can_set_data_bitrate → can_hw_start`，
  不清空发送缓存）；不支持的值置 `ERR_USB_BADFRAME`。
  至此 **仲裁段（命令 1）与数据段（命令 8）互相独立**可配。

### 变更 / 清理
- `can_init()` 移除冗余的 FDCAN 位时序赋值——真正生效的是 `can_hw_start()` 在 `FDCAN_Init()` 前把
  `can_set_bitrate()` / `can_set_data_bitrate()` 写入的静态变量拷贝进 `can_handle`。
- 版本 v1.1.2 → **v20.0.1**（`Makefile` / `inc/git_version.h`）。

### 涉及文件
`inc/can.h`、`src/can.c`、`inc/usb_proto.h`、`src/usb_proto.c`、`Makefile`、`inc/git_version.h`、
`docs/FIRMWARE_PROTOCOL.md`（§3 命令 8 + 速率表）。

---

## 历史

- **v1.1.2**：错误主动上报帧 `AA AD`（`AA AD | ASCII(ERR_*) | DA AA | CRC-8`，CRC 仅覆盖正文）。
- **v1.1.1**：DFU 升级改走厂商 32 位 `NsMcuCmdDll.dll`（随包 32 位 helper）；删除约 293 行
  冗余/死代码（`printf.c`、无调用 API 等）。
- **v1.0.0**：从 slcan ASCII 切换为**二进制 USB 协议**（`AA AA` 命令 / `AA AB` 数据 / `AA AC` 响应 /
  `AA AD` 告警）；新增帧列表模式、周期发送、自动波特率扫描、`GET_STATUS`、`ENTER_DFU`。
  协议详见 `docs/FIRMWARE_PROTOCOL.md`。
