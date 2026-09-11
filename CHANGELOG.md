# 更新日志 (CHANGELOG)

本仓库包含两个独立版本化的构件：

| 构件 | 版本 | 版本来源 |
|---|---|---|
| 固件 | **v1.1.1** | `Makefile` 的 `FW_VERSION`；Keil 构建见 `inc/git_version.h` |
| 上位机 | **v1.0.1** | `host/package.sh` 的 `APPVER`；安装包见 `host/installer/canable2_host.nsi` |

---

## 固件 v1.1.1

### 变更
- **新增显式固件版本**：`Makefile` 引入 `FW_VERSION := v1.1.1`，固件上报版本串为
  `v1.1.1-<git 修订>`（`src/main.c` 的 `fw_id`）；Keil 构建对应 `inc/git_version.h`
  的 `GIT_VERSION = "v1.1.1-keil"`。

### 清理（行为不变）
删除调试期遗留的杂乱/冗余代码共约 **293 行**：
- 整份**未被调用**的 `src/printf.c` + `inc/printf.h`（含 `Makefile`、Keil 工程引用）；
- **13 个无调用点**的函数/API 及其头声明：`can_set_id_format`、`can_set_silent`、
  `can_set_autoretransmit`、`can_get_bitrate_hz`、`can_tx_job_running`、`system_irq_disable`、
  `system_irq_enable`、`system_hex32`、`can_frame_list_pending_clear`、`can_timer_set_enabled`、
  `error_timestamp`、`error_occurred`、`usb_proto_reset`；
- `can.c` 中注释掉的 GPIO 死代码块与重复的文档注释、一处重复赋值；
- 3 处无用 `#include`（`main.c`/`interrupts.c`/`usbd_cdc_if.c`）与 1 处冗余前向声明。

> 保留 `inc/error.h` 的错误位 `enum`（其值即对外错误寄存器的 bit 位置，属协议 ABI，不可删改）。

### 文档
- 订正 `docs/FIRMWARE_PROTOCOL.md` 中引用已删函数的过时描述（ID 格式固定为 STANDARD）。

---

## 上位机 v1.0.1

### 新增
- **DFU 固件升级不再依赖厂商 GUI，也不再自实现 DfuSe 协议**：改由厂商 32 位
  `NsMcuCmdDll.dll` 完成烧录（协议组帧与 CRC 全部在 DLL 内部）。
  - 随包分发 32 位 CLI `host/vendor_dfu/nsi_dfu_flash.exe`（源码 `nsi_dfu_flash.c`），
    由 `host/src/VendorDfuFlasher.cpp` 经 `QProcess` 调用，序列：
    `ENUM → CONNECT → GET_INF → FLASH_ERASE → FLASH_DWNLD → APP_GO`（成功返回 `0xA000`）。
  - `host/src/dfu_device.cpp`（libusb DfuSe 自实现）降级为**回退**路径。
  - 无头验证命令：`CyberBeast-CANable2.exe --vendor-flash <bin> [addr]`。
- **可选品牌串口驱动 `CyberBeast USB2CAN`**：把设备（`VID_16D0&PID_117E`）在设备管理器/
  端口列表里的显示名改为 “CyberBeast USB2CAN”。驱动包位于
  `host/installer/drivers/CyberBeast_USB2CAN/`（INF + 签名 `.cat` + 自签名证书 `.cer`），
  安装包中作为**可选组件**：先 `certutil` 装入受信任存储，再 `pnputil /add-driver /install`。
  - 该组件**纯改名、非必需**：设备是标准 CDC-ACM，Win10/11 内置 `usbser.inf` 已能免驱出 COM 口。

### 变更 / 修正
- 订正文档中关于 DFU 的旧结论：DFU 设备实际为 **`0x19F5:0x4537`（"Nationstech DFU"）**，
  而非 ST 的 `0x0483:0xDF11`；并明确 **不要** 用 Zadig 给 DFU 设备装 libusb/WinUSB
  （一个设备同一时刻只能绑定一个内核驱动，装了 libusb 反而使厂商烧录 DLL 打不开它）。
- 安装包版本 1.0.0 → **1.0.1**（`VIProductVersion 1.0.1.0`）。
- **上位机更名为 “CyberBeast CANable 2”**：
  - 显示名（窗口标题、应用名、安装包产品名/开始菜单/桌面快捷方式）为 **`CyberBeast CANable 2`**（空格分隔）；
  - 文件名与系统标识为 **`CyberBeast-CANable2`**：可执行文件 `CyberBeast-CANable2.exe`、
    安装包 `CyberBeast-CANable2-Setup-<ver>.exe`、安装目录 `D:\Programs\CyberBeast-CANable2`、
    卸载注册表键 `...\Uninstall\CyberBeast-CANable2`；
  - 源文件 `host/installer/canable2_host.nsi`、内部 CMake target 名、以及依赖/扩展文件保持不变。

### 仓库清理
- 移除调试期产物：`ErrorLog/`、`_vt.txt`、`host/tools/`（逆向探测脚本）、
  `tools/dm_function_validation.py`、旧安装包、厂商下载与参考文档、临时日志等。
- `docs/` 中指向已删脚本的引用已同步更新。

---

## 历史

- **固件 v1.0.0 / 上位机 v1.0.0**：从 slcan ASCII 切换到**二进制 USB 协议**（`AA AA` 命令 /
  `AA AB` 数据 / `AA AC` 响应 / `AA AD` 告警）；新增帧列表模式、周期发送、自动波特率扫描、
  `GET_STATUS`、`ENTER_DFU`（命令 5）等。协议详见 `docs/FIRMWARE_PROTOCOL.md`。
