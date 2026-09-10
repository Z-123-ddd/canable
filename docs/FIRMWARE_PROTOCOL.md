# CANable2-N32 固件说明书（USB 二进制协议）

本文档描述本固件（N32H473 移植版）与上位机之间的 **USB CDC 二进制协议**：命令/数据帧的字节格式、CRC 校验、支持的命令、数据帧编码、波特率、状态查询与错误寄存器，以及 DFU 下载切换。适用于需要对接本固件的上位机开发。

> 说明：早期版本曾使用 slcan ASCII 协议，现已弃用并切换为下述二进制协议。CAN **接收上报**（总线→USB）仍为 slcan ASCII（见 [接收上报](#接收上报-bus-to-usb)）。

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

- 首个字节必须是 `0xAA`；第二个字节为 `0xAA`(命令) 或 `0xAB`(数据)，否则帧被丢弃，解析器在下个 `0xAA` 处重新同步。
- 帧体长度可变，解析器按字段逐字节推进，收完 CRC 后触发处理并回到空闲态。

---

## 3. 命令帧（`AA AA`）

格式：`AA AA | CMD (1B) | VALUE (4B, 小端 uint32)`

| CMD | 名称 | VALUE 含义 | MCU 行为 |
|---|---|---|---|
| `1` | SET_BITRATE | 目标波特率(Hz) | 设置仲裁段波特率；不支持的值置 `ERR_USB_BADFRAME`。总线开启时**热重配** FDCAN 以立即生效：**不清空发送缓存**——已入队的单帧缓存、帧列表、运行中的发送任务与接收队列均保留，可先发数据帧/设波特率再 `TX_START`（顺序无关） |
| `2` | TX_START | `0`=单次 / `1`=连续 | 触发发送缓存的源（单帧或列表）。单次=发一遍即停；连续=按定时器间隔周期发送。连续或列表模式需开启定时器（内部处理）。无缓存可发则置 `ERR_USB_BADFRAME` |
| `3` | TX_STOP | — | 停止当前发送任务并关闭定时器 |
| `4` | GET_STATUS | — | MCU 回一行 ASCII 状态（见 §7），供联机确认解析结果 |
| `5` | ENTER_DFU | — | 停发送与定时器 → 切换系统时钟至 HSI → 跳入出厂 DFU bootloader（`0x1FFF01A1`）。**成功不返回**，主机随后可用 N32 DFU 工具下载固件 |
| `6` | SET_INTERVAL | 发送间隔(ms) | 设置周期发送间隔，范围 **1–1000 ms**（整数）；越界置 `ERR_USB_BADFRAME`。用于命令 2（连续/列表周期发送）的间隔；重载定时器不影响已入队帧或运行中的任务 |
| `7` | SET_MODE | `0`=单帧 / `1`=列表 | 选择发送模式：`0` 单帧（每次数据帧覆盖单帧缓存）/ `1` 列表（数据帧 FIFO 追加到 pending，最多 20 帧）。其他值置 `ERR_USB_BADFRAME`。建议在发数据帧前设置 |

### 支持的波特率（命令 1，Hz）

`10000 / 20000 / 50000 / 83333 / 100000 / 125000 / 250000 / 500000 / 750000 / 1000000 / 5000000`

---

## 4. 数据帧（`AA AB`）

格式：

```
AA AB | ID (2B 或 4B) | CTRL (1B) | DATA (0..64B) | CRC (2B 或 3B)
```

### 4.1 ID 字段

ID 字节数由当前 **ID 格式配置**决定（默认标准 11-bit）：

| ID 格式 | 字节数 | ID 位宽 | 说明 |
|---|---|---|---|
| STANDARD（默认） | 2 | 11-bit | ID 以 2 字节大端承载 |
| EXTENDED | 4 | 29-bit | ID 以 4 字节大端承载 |

> ⚠️ ID 格式当前**只能通过 API 切换**（`can_set_id_format()`），尚无对应 USB 命令；同一时刻只能解析一种格式。

### 4.2 CTRL 控制字节

| 位 | 掩码 | 含义 |
|---|---|---|
| 7 | `0x80` | `FDF`：1 = CAN FD 帧；0 = 经典 CAN |
| 6 | `0x40` | `BRS`：FD 帧位速率切换（仅 FD 有效） |
| 5 | `0x20` | `RTR`：远程帧请求（仅经典帧有效） |
| 4 | — | 保留（应为 0） |
| 3..0 | `0x0F` | `DLC`：帧数据长度码（0..15） |

### 4.3 DLC → 字节数

| DLC | 0–8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|
| 字节 | 0–8 | 12 | 16 | 20 | 24 | 32 | 48 | 64 |

- 经典帧（FDF=0）DLC **必须 ≤ 8**，否则帧被拒并置 `ERR_USB_BADFRAME`。
- FD 帧 DLC 9–15 映射到 12/16/…/64 字节（CAN FD 标准表，中间长度不存在）。

### 4.4 DATA 与 CRC 长度

- 经典帧 CRC = 2 字节；FD 帧 CRC = 3 字节。
- 远程帧（经典 RTR=1）无 DATA 字节（`data_bytes=0`），但仍带 CRC。
- CRC 覆盖范围为**线序上的 ID + CTRL + DATA**（不含帧头、不含 CRC 自身）。

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
- **列表模式**：用命令 7（`SET_MODE`，value=1）切换到 FRAMELIST 后，一串数据帧按 FIFO 追加到 **pending 缓存**（上限 20 帧，超出置 `ERR_FRAMELIST_FULL`）；`TX_START` 时：
  - pending 非空 → 整体**替换**为新的播放列表（active）；
  - pending 为空（数据未变、仅发命令）→ **沿用上次播放列表**；
  - SINGLE = 列表按 FIFO 发一遍即停；CONTINUOUS = 按定时器间隔循环发整个列表。

> 发送模式可用命令 7（`SET_MODE`）在运行时切换（默认单帧），定时器周期可用命令 6（`SET_INTERVAL`）设置（默认 1ms）。仅 ID 格式当前仍**只有预留 API**（`can_set_id_format()`），尚无 USB 命令码。

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
| 9 | ERR_FRAMELIST_FULL | 列表模式一组长于 20 帧被拒 |

错误位一旦置位即**保持**（直到复位/重新上电），可作调试指示。

---

## 9. DFU 下载（命令 5）

1. 主机发 `AA AA 05 00 00 00 00`。
2. MCU 停止发送任务与定时器，切 HSI 时钟，跳入 N32 出厂 bootloader（`0x1FFF01A1`）。
3. MCU **不再返回**；USB 设备重新枚举为 N32 DFU 设备。
4. 主机用 N32 DFU 下载工具烧写新固件。

---

## 10. 接收上报（bus → USB）

CAN 接收帧仍以 **slcan ASCII** 格式上报（一行以 `\r` 结尾）：

| 帧类型 | 首字符 | 示例 |
|---|---|---|
| 经典标准数据 | `t` | `t1238DEADBEEF`（`t`+3位ID+DLC+hex数据） |
| 经典扩展数据 | `T` | `T012345678...`（`T`+8位ID） |
| 经典标准/扩展远程 | `r` / `R` | 同上但无数据 |
| FD（无 BRS）标准/扩展 | `d` / `D` | |
| FD（带 BRS）标准/扩展 | `b` / `B` | |

> 接收方向不受发送侧 ID 格式配置影响：硬件过滤器接收全部标准+扩展帧，并按**实际收到帧**的类型生成对应字符（上位机配置不影响被调试设备的总线协议）。

---

## 11. 完整示例（字节流）

**发送一帧标准 ID 0x123、经典 2 字节 `DE AD` 并单次发送**

1. 发数据帧（ID 2B = `01 23`，CTRL：FDF=0/BRS=0/RTR=0/DLC=2 → `0x02`，DATA = `DE AD`，CRC15 覆盖 `01 23 02 DE AD`）：
   ```
   AA AB 01 23 02 DE AD <CRC15:2B>
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
AA AB 00 01 08 FF FF FF FF FF FF FF FA 79 F1   ; 帧1  ID=1 数据 FF..FA
AA AB 00 01 08 FF FF FF FF FF FF FF FB 3C 68   ; 帧2  FF..FB
AA AB 00 01 08 FF FF FF FF FF FF FF FC 6F 95   ; 帧3  FF..FC
AA AB 00 01 08 FF FF FF FF FF FF FF FD 2A 0C   ; 帧4  FF..FD
AA AB 00 01 08 FF FF FF FF FF FF FF FE 21 3E   ; 帧5  FF..FE
AA AB 00 01 08 FF FF FF FF FF FF FF FF 64 A7   ; 帧6  FF..FF
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
| `can_set_id_format()` / `can_get_id_format()` | 切换标准/扩展 ID 宽度（影响数据帧 ID 字节数） |
| `can_timer_enable()` / `can_timer_disable()` | 软件门控周期发送 |
| `can_tx_start()/stop()/job_running()` | 直接触发/停止发送任务（命令 2/3 的内部实现） |

---

## 13. 构建与烧录

- 编译：`make`（需 arm-none-eabi-gcc），产物在 `build/`（`.bin` / `.hex` / `.elf`）。
- 联机下载调试：J-Link 等 SWD 工具烧录；或先烧一版含命令 5 的固件后，用 DFU 通道更新。
