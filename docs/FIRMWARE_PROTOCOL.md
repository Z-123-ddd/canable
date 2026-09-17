# CANable2-N32 固件说明书（USB 二进制协议）

本文档描述本固件（N32H473 移植版）与上位机之间的 **USB CDC 二进制协议**：命令/数据帧的字节格式、CRC 校验、支持的命令、数据帧编码、波特率、状态查询与错误寄存器，以及 DFU 下载切换。适用于需要对接本固件的上位机开发。

> 说明：早期版本曾使用 slcan ASCII 协议，现已弃用并全部切换为下述二进制协议（发送与接收两个方向都用二进制帧）。CAN **接收上报**（总线→USB）见 [接收上报](#接收上报-bus-to-usb)。

---

## 1. 设备信息

| 项 | 值 |
|---|---|
| USB 类型 | Full-Speed CDC ACM（虚拟串口） |
| VID / PID | `0x16D0` / `0x117E`（与原版 CANable2 一致） |
| CAN 控制器 | FDCAN1 |
| 默认仲裁段波特率 | 1 Mbit/s（可用命令 1 修改） |
| CAN FD 数据段 | 5 Mbit/s |
| 物理引脚 | CAN TX = PB13 (AF25)，CAN RX = PB12 (AF26) |

---

## 2. 通用帧结构

协议为**字节流**解析：USB 包边界被忽略，一帧可跨多个 64 字节 USB 包。每帧以两字节帧头开始：

| 帧头 | 类型 |
|---|---|
| `AA AA` | **命令帧**（host → MCU 控制命令） |
| `AA AB` | **数据帧**（host → MCU 发送一帧 CAN / CAN FD） |
| `AA AC` | **响应帧**（MCU → host；波特率锁定 / 版本 / TEC·REC，见 §14） |
| `AA AD` | **警告/报错帧**（MCU → host；ASCII 错误码，见 §15） |

- 首个字节必须是 `0xAA`；第二个字节为 `0xAA`(命令) 或 `0xAB`(数据)，否则帧被丢弃，解析器在下个 `0xAA` 处重新同步。
- 帧体长度可变，解析器按字段逐字节推进，收完 CRC 后触发处理并回到空闲态。

---

## 3. 命令帧（`AA AA`）

格式：`AA AA | CMD (1B) | VALUE (4B, 小端 uint32)`

| CMD | 名称 | VALUE 含义 | MCU 行为 |
|---|---|---|---|
| `1` | SET_BITRATE | 目标波特率(Hz) | 设置**仲裁段波特率**（默认 1 Mbit/s）；不支持的值置 `ERR_USB_BADFRAME`。总线开启时**热重配** FDCAN 以立即生效：**不清空发送缓存**——已入队的单帧缓存、帧列表、运行中的发送任务与接收队列均保留，可先发数据帧/设波特率再 `TX_START`（顺序无关） |
| `2` | TX_START | `0`=单次 / `1`=连续 | 触发发送缓存的源（单帧或列表）。单次=发一遍即停；连续=按定时器间隔周期发送。连续或列表模式需开启定时器（内部处理）。无缓存可发则置 `ERR_USB_BADFRAME` |
| `3` | TX_STOP | — | 停止当前发送任务并关闭定时器 |
| `4` | GET_STATUS | — | MCU 回一行 ASCII 状态（见 §7），供联机确认解析结果 |
| `5` | ENTER_DFU | — | 停发送与定时器 → 切换系统时钟至 HSI → 跳入出厂 DFU bootloader（`0x1FFF01A1`）。**成功不返回**，主机随后可用 N32 DFU 工具下载固件 |
| `6` | SET_INTERVAL | 发送间隔(ms) | 设置周期发送间隔，范围 **1–1000 ms**（整数）；越界置 `ERR_USB_BADFRAME`。用于命令 2（连续/列表周期发送）的间隔；重载定时器不影响已入队帧或运行中的任务 |
| `7` | SET_MODE | `0`=单帧 / `1`=列表 | 选择发送模式：`0` 单帧（每次数据帧覆盖单帧缓存）/ `1` 列表（数据帧 FIFO 追加到 pending，最多 20 帧）。其他值置 `ERR_USB_BADFRAME`。建议在发数据帧前设置 |
| `8` | SET_DATA_BITRATE | 数据域波特率(Hz) | 设置 **CAN FD 数据段波特率**（`2000000` / `5000000`）；其他值置 `ERR_USB_BADFRAME`。总线开启时**热重配**（同命令 1，不清空发送缓存）。与命令 1（仲裁段）**互相独立** |
| `9` | SET_WORK_MODE | `0`=CAN / `1`=RS485 | 切换调试器工作模式。切到 RS485 会停 CAN 状态机/中断/定时器并拉起 RS485 接收；切回 CAN 则释放 RS485 并重启 FDCAN |
| `10` | GET_VERSION | — | MCU 回固件版本：`AA AC 02 \| LEN(1) \| TEXT(ASCII)`（`GIT_VERSION`） |
| `11` | SET_PAUSE | `1`=暂停接收 / `0`=恢复 | **暂停/恢复接收**。暂停时：停止 CAN 发送任务与定时器、关闭 FDCAN 接收/错误中断、清空软件接收队列（未完成/已缓存帧），避免恢复后出现坏数据；总线仍保持配置。恢复时：丢弃硬件 FIFO 里的滞留帧、清空接收队列、重新使能接收中断。其他值置 `ERR_USB_BADFRAME` |

### 支持的波特率

- **仲裁段（命令 1，Hz）**：`10000 / 20000 / 50000 / 83333 / 100000 / 125000 / 250000 / 500000 / 750000 / 1000000 / 5000000`
- **数据段（命令 8，Hz）**：`2000000 / 5000000`

---

## 4. 数据帧（`AA AB`）

格式（**v30.x 线序**）：

```
AA AB | CTRL (1B) | ID (2B 或 4B) | DATA (0..64B) | CRC (2B 或 3B)
```

> 旧版曾为 `ID | CTRL`；v30.x 起为 **`CTRL | ID`**，CRC 覆盖范围随之变更。

### 4.1 CTRL 控制字节

| 位 | 掩码 | 含义 |
|---|---|---|
| 7 | `0x80` | `FDF`：1 = CAN FD 帧；0 = 经典 CAN |
| 6 | `0x40` | `BRS`：FD 帧位速率切换（仅 FD 有效） |
| 5 | `0x20` | `RTR`：远程帧请求（仅经典帧有效） |
| 4 | `0x10` | **`IDE`**：1 = 扩展 ID（4 字节）；0 = 标准 ID（2 字节） |
| 3..0 | `0x0F` | `DLC`：帧数据长度码（0..15） |

### 4.2 ID 字段

| CTRL.IDE | 字节数 | ID 位宽 | 说明 |
|---|---|---|---|
| 0（标准） | 2 | 11-bit | ID 以 2 字节大端承载 |
| 1（扩展） | 4 | 29-bit | ID 以 4 字节大端承载 |

ID 宽度由**本帧 CTRL.IDE** 决定（收发均如此），不再依赖单独的全局 ID 格式配置命令。

### 4.3 DLC → 字节数

| DLC | 0–8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|
| 字节 | 0–8 | 12 | 16 | 20 | 24 | 32 | 48 | 64 |

- 经典帧（FDF=0）DLC **必须 ≤ 8**，否则帧被拒并置 `ERR_USB_BADFRAME`。
- FD 帧 DLC 9–15 映射到 12/16/…/64 字节（CAN FD 标准表，中间长度不存在）。

### 4.4 DATA 与 CRC 长度

- 经典帧 CRC = 2 字节；FD 帧 CRC = 3 字节。
- 远程帧（经典 RTR=1）无 DATA 字节（`data_bytes=0`），但仍带 CRC。
- CRC 覆盖范围为**线序上的 CTRL + ID + DATA**（不含帧头、不含 CRC 自身）。

---

## 5. CRC 校验

传输层校验（非 CAN 总线 CRC），所有变体：**MSB-first、初值 0、xorout 0、无反射、不做位填充**。上位机必须与 MCU 实现一致。

| 适用 | 位宽 | 多项式寄存器常数 | 字节数 |
|---|---|---|---|
| 经典帧（FDF=0） | CRC-15/CAN | `0x4599` | 2（右对齐） |
| FD，DATA ≤ 16B | CRC-17/CAN-FD | `0x1685B` | 3（右对齐） |
| FD，DATA > 16B | CRC-21/CAN-FD | `0x102899` | 3（右对齐） |

> 多项式常数为去掉最高 x^N 项后的寄存器值。CRC 校验失败置 `ERR_USB_CRC` 并丢弃该帧。

---

## 6. 发送模式与流程

固件维护 **RAM 中的发送源缓存**，数据帧只写入缓存、**不会立即上总线**；实际发送由命令帧触发：

- **单帧模式**（默认）：每次数据帧覆盖"最近一帧"缓存；`TX_START` 后按 SINGLE（发一次）/ CONTINUOUS（定时周期重发最近帧）发送。
- **列表模式**：用命令 7（`SET_MODE`，value=1）切换到 FRAMELIST 后，一串数据帧按 FIFO 追加到 **pending 缓存**（上限 20 帧，超出丢弃并置 `ERR_CAN_TXFAIL`）；`TX_START` 时：
  - pending 非空 → 整体**替换**为新的播放列表（active）；
  - pending 为空（数据未变、仅发命令）→ **沿用上次播放列表**；
  - SINGLE = 列表按 FIFO 发一遍即停；CONTINUOUS = 按定时器间隔循环发整个列表。

> 发送模式可用命令 7（`SET_MODE`）在运行时切换（默认单帧），定时器周期可用命令 6（`SET_INTERVAL`）设置（默认 1ms）。标准/扩展 ID 由每帧 **CTRL.IDE** 决定。

**定时器发送**：周期发送由 GTIM1 驱动，一旦启动即从 RAM 取帧，**不依赖 USB 持续喂数据**（上位机只需发一次数据+一次 `TX_START`）。

---

## 7. 状态查询（命令 4，GET_STATUS）

MCU 回一行 ASCII（以 `\r` 结尾）：

```
ST:bus=1 mode=S job=0 tmr=0 cur=1 pend=0 act=0 err=00000000
```

| 字段 | 含义 |
|---|---|
| `bus` | 0/1：CAN 总线关闭/开启 |
| `mode` | `S`=单帧 / `L`=列表 |
| `job` | 0/1：发送任务是否在运行 |
| `tmr` | 0/1：周期定时器是否使能 |
| `cur` | 0/1：单帧缓存是否有效（有可发的最近帧） |
| `pend` | pending 列表帧数（本次累积未封存） |
| `act` | active 列表帧数（已封存可播放） |
| `err` | 8 位十六进制错误寄存器（见 §8） |

---

## 8. 错误寄存器

GET_STATUS 的 `err` 字段为 32 位错误寄存器的低 8 位十六进制，各 bit 含义：

| bit | 错误 | 含义 |
|---|---|---|
| 0 | ERR_PERIPHINIT | 外设初始化失败 |
| 1 | ERR_USBTX_BUSY | USB TX 忙 |
| 2 | ERR_CAN_TXFAIL | CAN 发送失败 |
| 3 | ERR_CANRXFIFO_OVERFLOW | CAN RX 软件环形缓冲溢出（覆盖最旧） |
| 4 | ERR_FULLBUF_CANTX | （历史保留） |
| 5 | ERR_FULLBUF_USBRX | USB RX 缓冲满 |
| 6 | ERR_FULLBUF_USBTX | USB TX 缓冲满 |
| 7 | ERR_USB_BADFRAME | 未知命令 / 畸形帧 |
| 8 | ERR_USB_CRC | 数据帧 CRC 校验失败 |
| 9 | ERR_FRAMELIST_FULL | （历史保留；列表溢出现走 `ERR_CAN_TXFAIL`） |

错误位一般保持至复位；**`ERR_CANRXFIFO_OVERFLOW` 在 RX 队列排空后可清除**。

---

## 9. DFU 下载（USB 更新固件，命令 5）

### 9.1 进入 DFU（MCU 侧，已实现 `src/dfu.c`)
1. 主机发 `AA AA 05 00 00 00 00`。
2. MCU 停止发送任务与定时器；`dfu_enter_bootloader()` 执行：
   - 关闭并复位 Flash iCache；
   - 探测 bootloader 版本（`Get_NVR`，`0x1FFF02A5`）；
   - 在 **SRAM 顶部 - 0x200** 处重建 128 字向量表，重定向 SysTick/USART3/USB_FS 到 boot 代码；
   - `__disable_irq()`，重装并冻结 IWDG；
   - 系统时钟切回 HSI（`RCC_DeInit` + HSI，`FLASH_SetLatency` 0）；
   - `SCB->VTOR = 向量表地址`，`__set_MSP(SPAddr)`，跳 `0x1FFF01A1`（V1.1 用 `SPAddr=0x200024C0`）。

> 向量表地址 = `0x20000000 + SRAM_SIZE - 0x200`，**`SRAM_SIZE` 按具体芯片**（官方应用笔记
> `AN_N32H47x_48x_Jump_to_BOOT` V1.1.0）。本板 **N32H473CE = 160KB SRAM + 32KB CCM = 192KB**
> → 向量表在 **`0x2002FE00`**（`src/dfu.c` 同时兼容参考地址 `0x20023E00`）。

3. MCU **不再返回**；USB 设备重新枚举为 **DFU 设备**。

### 9.2 主机侧（USB 识别与烧录）
- N32 出厂 bootloader 在 USB-FS 上枚举为 **VID/PID = `0x19F5:0x4537`**（"NationstechDFU Device"，设备名
  `Nationstech DFU`）。**注意不是 ST 的 `0x0483:0xDF11`/"STM32 BOOTLOADER"** —— N32 用自己的 ID。
- **Windows 必须装厂商签名驱动 `NationstechDFU`**（`host/installer/drivers/NationstechDFU/`，用
  `pnputil /add-driver ... /install` 注册）。**不要用 Zadig 把它装成 WinUSB/libusb** ——
  一个 USB 设备同一时刻只能绑一个内核驱动，装成 libusb 后厂商烧录 DLL 反而打不开它。
- **烧录由厂商 32 位 `NsMcuCmdDll.dll` 完成**（协议组帧/CRC 都在 DLL 内部，非 DfuSe 标准协议）。
  上位机 64 位无法同进程加载该 DLL，故随包分发 32 位 CLI `host/vendor_dfu/nsi_dfu_flash.exe`，
  由 `host/src/VendorDfuFlasher.cpp` 用 QProcess 调用：
  `ENUM → CONNECT → GET_INF → FLASH_ERASE → FLASH_DWNLD → APP_GO`，全部成功返回 `0xA000`。
  关键坑：`NS_Dl_File.+0x08` 是**数据指针**（不是 CRC）；`FLASH_DWNLD` 的 `a2..a5` 传 `NULL`。
  跳转运行由 `APP_GO` 完成，无需自行发送 Leave。
- 设备匹配：`kDfuVid=0x19F5 / kDfuPid=0x4537`（`host/src/dfu_device.h`）。
- `host/src/dfu_device.cpp`（libusb DfuSe 自实现）仅作**回退**保留：它要求设备绑定 WinUSB/libusb，
  与 `NationstechDFU` 互斥，正常装机场景下不会用到。
- 点「更新固件」：先经当前连接发命令 5 → 轮询 DFU 设备（libusb 枚举）→ 打开固件对话框 →
  由厂商 DLL 烧录。

### 9.3 验证
- `CyberBeast-ProtocolTool.exe --dfu-selftest`：DfuSe 命令编码 / 内存布局解析 / 扇区计算（无硬件）。
- `CyberBeast-ProtocolTool.exe --dfu-scan`：枚举 USB 并判断 DFU 设备是否在线。
- `CyberBeast-ProtocolTool.exe --vendor-flash <bin> [addr]`：无头走厂商 DLL 烧录（需设备已在 DFU 模式）。

---

## 10. 接收上报（bus → USB，二进制）

总线收到的 CAN / CAN FD 帧以**与发送方向完全相同的二进制数据帧格式**上报：

```
AA AB | CTRL (1B) | ID (2B 或 4B) | DATA (0..64B) | CRC (2B 或 3B)
```

各字段与发送数据帧（§4）一致：

- **帧头**：`AA AB`（与发送数据帧相同；上位机按方向区分收发）。
- **CTRL**：bit7 `FDF`、bit6 `BRS`、bit5 `RTR`、bit4 `IDE`、bits3..0 `DLC`（同 §4.1）。
- **ID**：宽度由本帧 **CTRL.IDE** 决定——标准 2 字节、扩展 4 字节（大端）。
- **DATA**：长度由 DLC 推导（经典 ≤8、FD ≤64，映射见 §4.3）；经典远程帧无 DATA。
- **CRC**：与发送侧相同算法——经典 2 字节（CRC-15），FD 3 字节（CRC-17/21），覆盖 **CTRL + ID + DATA**（见 §5）。

> 硬件过滤器接收全部标准 + 扩展帧；上报按每个帧**自身的类型**编码，上位机据此可还原总线上的原始帧。

---

## 11. 完整示例（字节流）

**发送一帧标准 ID 0x123、经典 2 字节 `DE AD` 并单次发送**

1. 发数据帧（CTRL：FDF=0/BRS=0/RTR=0/IDE=0/DLC=2 → `0x02`，ID 2B = `01 23`，DATA = `DE AD`，CRC15 覆盖 `02 01 23 DE AD`）：
   ```
   AA AB 02 01 23 DE AD <CRC15:2B>
   ```
2. 发命令帧单次发送：
   ```
   AA AA 02 00 00 00 00
   ```

**查询状态**

```
AA AA 04 00 00 00 00      → 回: ST:bus=1 mode=S job=0 tmr=0 cur=1 pend=0 act=0 err=00000000\r
```

**设置周期发送间隔为 100ms（命令 6）**

```
AA AA 06 64 00 00 00      ; 100 = 0x64，小端
```

**切换为列表模式并发送一组帧（命令 7 + 数据帧 + 命令 2）**

```
AA AA 07 01 00 00 00      ; SET_MODE: 1 = 列表模式
AA AB 08 00 01 FF FF FF FF FF FF FF FF <CRC15>   ; 帧1  CTRL=08 ID=1 数据 FF.. 
AA AB 08 00 01 FF FF FF FF FF FF FF FF <CRC15>   ; 帧2 …
AA AA 02 00 00 00 00      ; TX_START value=0 单次：列表按 FIFO 发一遍
```

> 回到单帧模式：`AA AA 07 00 00 00 00`。

**进入 DFU**

```
AA AA 05 00 00 00 00
```

---

## 12. 预留 API（无 USB 命令，供二次开发）

| API | 作用 |
|---|---|
| `can_get_id_format()` | 读取当前 ID 格式（当前固定 STANDARD） |
| `can_timer_enable()` / `can_timer_disable()` | 软件门控周期发送 |
| `can_tx_start()` / `can_tx_stop()` | 直接触发/停止发送任务（命令 2/3 的内部实现） |

---

## 13. 构建与烧录

- 编译：`make`（需 arm-none-eabi-gcc），产物在 `build/`（`.bin` / `.hex` / `.elf`）。
- 联机下载调试：J-Link 等 SWD 工具烧录；或先烧一版含命令 5 的固件后，用 DFU 通道更新。

---

## 14. 自动波特率扫描（总线错误触发）

用途：当总线上有流量、但当前仲裁段波特率无法解码（表现为 FDCAN 错误计数上升）时，
固件自动轮询所有支持的仲裁段波特率，直到某个波特率能收到帧，即“锁定”该波特率并
回报给上位机。

### 14.1 触发与流程

- **触发**：总线开启、FDCAN 报错（接收/发送错误计数 ≥ 阈值，或进入错误被动），
  且最近一段时间未收到有效帧——即“有流量但当前波特率收不到”。
- **扫描**：按顺序尝试各支持速率（1M→500k→250k→125k→100k→83.3k→50k→20k→10k→
  750k→5M），每个速率停留约 120ms。切换速率**不清空发送缓存**。
- **锁定**：某速率在观察窗内收到有效帧 → 锁定该速率、停止扫描;随后正常上报总线帧。
- **重扫**：锁定后若再次出现总线错误且长时间无接收，自动重新扫描。

### 14.2 波特率锁定回报（AA AC）

锁定瞬间，MCU 先发送一帧响应帧，随后同速率下收到的帧按 §10 的二进制数据帧（`AA AB …`）继续上报：

```
AA AC | 01 | BITRATE (4B 小端)
```

| 字段 | 值 |
|---|---|
| 帧头 | `AA AC`（MCU → host 响应帧） |
| 命令码 | `01` = 波特率锁定 |
| BITRATE | 锁定的仲裁段波特率，单位 Hz，4 字节小端 uint32 |

示例（锁定 500k）：`AA AC 01 20 A1 07 00`

### 14.3 错误帧累计上报（AA AC 03）

固件将硬件 TEC/REC 的**增量 ÷8** 累计为错误帧数（`uint32`，饱和到 `0xFFFFFFFF`），主动上报：

```
AA AC | 03 | TEC_FRAMES (4B 小端) | REC_FRAMES (4B 小端) | CRC8(覆盖 8 字节计数)
```

CRC-8：poly `0x07`、init 0。上位机状态栏直接显示原始累计值（不再 ÷8）。硬件 TEC 复位后累计值仍保持（上电清零）。

### 14.4 固件版本（AA AC 02）

主机发命令 10 后：

```
AA AC | 02 | LEN (1B) | TEXT(LEN ASCII)
```

---

## 15. 警告/报错帧（`AA AD`，MCU → host）

MCU 主动上报警告/报错文本，供上位机日志区显示（日志区仅呈现此类告警信息）：

```
AA AD | TEXT(ASCII) | DA AA | CRC
```

| 字段 | 说明 |
|---|---|
| 帧头 | `AA AD`（MCU → host 警告/报错帧） |
| TEXT | ASCII **错误枚举名**（见 §8 各 `ERR_*`），如 `ERR_USB_BADFRAME` |
| 尾标 | `DA AA`（ASCII 正文均为 `<0x80`，此两字节均 `>0x7F`，作可靠终止符） |
| CRC | 1 字节 CRC-8（poly `0x07`, init 0, 无反射），**仅覆盖 TEXT**（`AA AD` 与 `DA AA` 不参与） |

> 触发：`error_assert()` 将错误排入队列，**主循环**取出并发送（不在中断上下文发 USB）。
> 上位机即使 CRC 不符也显示正文并加注“CRC 校验失败”。

