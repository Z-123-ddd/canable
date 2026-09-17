# 更新日志 (CHANGELOG) — CANable2-N32

本仓库同时包含 **固件**（N32H473）与 **上位机**（`host/`）。

| 构件 | 版本 | 来源 |
|---|---|---|
| 固件 | **v30.0.4** | `Makefile` `FW_VERSION`；Keil 见 `inc/git_version.h` |
| 上位机 | **30.0.4** | `host/CMakeLists.txt` / `host/package.sh` / NSIS |

---

## v30.0.4（固件 + 上位机）

- **固件与上位机版本统一为 v30.0.4**；重建固件 release 产物为 v30.0.4。
- 固件：新增接收暂停命令码 11（SET_PAUSE）——暂停时停 CAN 收发中断并清未完成帧，恢复时丢弃滞留帧。
- 上位机：接收区「暂停/继续接收」按钮、四协议接收解析区、CyberBeast 配置/接收区。

---

## v30.0.3（上位机）

### 安装包内置 Npcap（调用 GUI 安装）

- 安装包**内置 Npcap 官方安装程序**（`host/installer/Npcap/npcap-1.89.exe`）；
  安装过程中以**图形界面**方式调用（ExecWait，非静默），用户按 Npcap 向导完成。
  调用参数 `/winpcap_mode=yes`（仅预勾选 WinPcap 兼容模式）。组件可取消/跳过。
- `wpcap_dynload.c` 增加 `<System32>\Npcap\` 全路径兜底加载，未启用 WinPcap 兼容模式也能工作。

---

### 上位机改名：CyberBeast 协议调试工具

- 上位机产品名由「CyberBeast CANable 2」改为 **CyberBeast 协议调试工具**：
  窗口标题、关于对话框、安装器产品名、开始菜单/桌面快捷方式、卸载项显示名同步更新。
- 构建产物与安装目录重命名：exe `CyberBeast-ProtocolTool.exe`、安装目录
  `D:\Programs\CyberBeast-ProtocolTool`、包名 `CyberBeast-ProtocolTool-Setup-<ver>.exe`
  与 `CyberBeast-ProtocolTool-<ver>-portable.zip`。
- QSettings 标识随应用名变化，已加入**一次性迁移**：首次启动把旧位置
  (`CANable2-N32`/`CyberBeast CANable 2`) 的全部键值搬到新位置，老用户配置不丢。

---

### EtherCAT 主站（内嵌 SOEM）

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

### 接收暂停（命令 11）+ 协议解析区 + CyberBeast 配置/接收区

- **固件新增命令 11（SET_PAUSE）**：暂停时停止 CAN 发送任务/定时器与 FDCAN 接收/错误中断，
  清空软件接收队列（未完成/已缓存帧）；恢复时丢弃硬件 FIFO 滞留帧、清空队列并重新使能中断，
  避免恢复后出现坏数据。见 `docs/FIRMWARE_PROTOCOL.md` 命令表。
- **上位机接收区新增「暂停/继续接收」按钮**：点击下发命令 11、冻结本机接收、重置字节流解析器
  并清空未完成帧；各页接收区各一枚，状态同步。
- **接收区下方 1/3 新增「解析区」**（Simple / Open / CyberBeast / RS485 四个协议页）：
  按《守护兽协议手册》把每帧解析为 **ID号 / 命令功能 / 数据含义（整数或浮点）**，逐帧追加。
- **CyberBeast 协议页**由骨架补齐：命令按**类别下拉**（实时控制/配置管理/状态查询/系统管理/广播紧急）
  → 命令下拉 → 参数（仿 simple/open），含预览与接收区（就地/滚动/暂停/解析）。
- 新增 `host/src/FrameParse.{h,cpp}`：四协议 + 通用的接收帧解析。

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
