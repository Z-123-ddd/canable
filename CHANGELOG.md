# 更新日志 (CHANGELOG) — CANable2-N32

本仓库同时包含 **固件**（N32H473）与 **上位机**（`host/`）。

| 构件 | 版本 | 来源 |
|---|---|---|
| 固件 | **v30.0.2** | `Makefile` `FW_VERSION`；Keil 见 `inc/git_version.h` |
| 上位机 | **30.0.2** | `host/CMakeLists.txt` / `host/package.sh` / NSIS |

---

## 未发布 (Unreleased) — 上位机：EtherCAT 主站（内嵌 SOEM）

- **模式菜单新增「EtherCAT」**：独立页面，不走串口/USB（AA AA 协议），而是直接扫描主机网卡。
- 内嵌 **SOEM v2.0.0**（`host/third_party/soem/`，GPLv3；见 LOCAL_PATCHES.md）作为静态库编译进上位机：
  仅一处迁移——`osal/win32/osal.c` 的 `osal_current_time()` 改用纯 Win32
  `GetSystemTimeAsFileTime()`（去掉 UCRT 依赖）。
- Win32 网络层用 WinPcap API；因 MinGW ld 无 `--delayload`、直接链接会让无 Npcap 的机器
  连启动都失败，改为 **运行时动态加载 wpcap.dll**（`wpcap_dynload.c`）：未装 Npcap 也能启动，
  用 EtherCAT 时再提示。安装包内含可选 **Npcap 静默安装**段（放 `installer/Npcap/Npcap-installer.exe` 即随包安装）。
- EtherCAT 页功能：**扫描网卡 / 打开关闭 / 扫描从站（ecx_config_init）/ 从站列表
  / 过程数据映射与周期收发（WKC）/ CoE(SDO) 读写**。

---

## v30.0.2（固件 + 上位机）

### 协议
- `AA AC 03` 改为累计错误帧数：`TEC_u32 LE | REC_u32 LE | CRC8`（固件对硬件 TEC/REC 增量÷8 后饱和累加至 `uint32`）。
- 上位机状态栏直接显示原始累计值，不再 ÷8。

---

## v30.0.1（固件 + 上位机）

### 协议
- 数据帧线序统一为 **`AA AB | CTRL | ID | DATA | CRC`**（CTRL.IDE 选 2B/4B ID）。
- CRC 覆盖 **CTRL+ID+DATA**；旧 `ID|CTRL` 上位机不可混用。
- 新增主动上报 **`AA AC 03 | TEC | REC | CRC8`**；上位机状态栏按帧显示 TEC÷8。
- 单帧硬件 DAR（无重试）；无 ACK / Bus-Off / 入队失败 → `AA AD`（如 `ERR_CAN_TXFAIL`）。
- 命令 **10 GET_VERSION**、命令 **9 工作模式** 已与上位机对齐。

### 固件稳定性
- 主循环限制 RX/错误突发处理，避免对端洪泛时饿死 USB（表现为“上位机打开后卡死”）。
- RX 软队列溢出位在排空后可清除；USB NVIC 优先级高于 FDCAN。

### 上位机
- UI 自适应布局；列表按钮行固定在表格下方。
- NSIS：已安装时可选择修改 / 修复 / 覆盖。
- 发行包：`Setup-30.0.1.exe` + `30.0.1-portable.zip`；固件 `release/canable2-n32-v30.0.1.bin`。

### 清理
- 移除未使用的 `fw_id` 全局变量；修正文档中过时的“命令 10 未实现”说明。

---

## 固件 v20.0.3

### 新增
- USB 命令 **`10` = GET_VERSION**：响应 `AA AC 02 | LEN(1) | TEXT(LEN)`。

### 变更
- 版本号 `v20.0.2 → v20.0.3`。

---

## 固件 v20.0.1

### 新增
- USB 命令 **`8` = SET_DATA_BITRATE**（`2000000` / `5000000` Hz）。

### 变更 / 清理
- `can_init()` 移除冗余位时序赋值；版本 → **v20.0.1**。

---

## 历史

- **v1.1.2**：错误主动上报帧 `AA AD`。
- **v1.1.1**：DFU 改走厂商 DLL；清理冗余代码。
- **v1.0.0**：从 slcan ASCII 切换为二进制 USB 协议。
