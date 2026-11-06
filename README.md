# CyberBeast CANable2-N32

基于 **N32H473** 的 USB–CAN/CAN FD 调试器固件，以及配套 **Qt6 上位机**（CyberBeast 协议调试工具）。

| 构件 | 版本 | 说明 |
|------|------|------|
| 固件 | **v30.0.7** | `Makefile` → `FW_VERSION`；Keil 见 `inc/git_version.h` |
| 上位机 | **30.0.9** | `host/CMakeLists.txt` / `host/package.sh` |

发行物：

- 固件：`release/canable2-n32-v30.0.7.bin`（及 `.hex`）
- 上位机：`host/dist/CyberBeast-ProtocolTool-Setup-30.0.9.exe`、`host/dist/CyberBeast-ProtocolTool-30.0.9-portable.zip`

---

> ## 📖 关于本文件
>
> 本 README 同时是**帧打包/解包协议的唯一权威规范**——如果你要脱离本仓库上位机、
> 自己写脚本或工具与设备通信，看这一份就够了（含 CMD 1–12 完整字典、
> 8 种帧组合矩阵、逐字节示例、参考实现与常见坑清单）。
>
> - 同一份规范的独立文件：[`docs/USB_FRAME_PROTOCOL.md`](docs/USB_FRAME_PROTOCOL.md)
> - 固件整体设计（DFU 流程、自动波特率扫描、架构细节）：[`docs/FIRMWARE_PROTOCOL.md`](docs/FIRMWARE_PROTOCOL.md)
> - 上位机说明：[`host/README.md`](host/README.md)
>
> 所有字节级细节均以 `inc/usb_proto.h` / `src/usb_proto.c` 为准，并已在实机上逐条验证。

---

## 0. 快速上手（先读这一节）

最短可用流程 —— **发一帧 CAN 至少要 3 步，只发 `AA AB` 不会上总线**：

```text
# 1) 选单帧模式（默认就是单帧，首次连接建议显式发一次）
AA AA 07 00 00 00 00          # SET_MODE = 0 (单帧)

# 2) 把要发的 CAN 帧交给固件（只进缓存，不上总线！）
AA AB 02 01 23 DE AD          # 标准ID 0x123, 经典帧, 数据 DE AD

# 3) 触发发送（这一步才真正上总线）
AA AA 02 00 00 00 00          # TX_START = 0 (单次)
```

任何一步都不能省。**第 2 步只写缓存是这套协议最容易踩的坑。**

---

## 1. 传输层

| 项 | 值 |
|---|---|
| USB 类型 | Full-Speed **Vendor Specific (bDeviceClass = 0xFF)** |
| VID / PID | `0x16D0` / `0x117E` |
| 配置 | 单接口（bInterfaceNumber = 0），2 个 Bulk 端点 |
| 端点 | **OUT = `0x01`**（host → device），**IN = `0x81`**（device → host） |
| 单包上限 | 64 字节（Full-Speed bulk `wMaxPacketSize`） |
| 驱动 | Windows 下需绑定 WinUSB 或 libusb；固件带 MS OS 2.0 (WCID) 描述符，Windows 8.1+ 会自动绑定 WinUSB |

### 1.1 字节流语义（重要）

**USB 包边界没有任何含义。** 固件端的解析器是一个逐字节推进的状态机，因此：

- 一个协议帧可以**跨越多个 64 字节 USB 包**（例如 69 字节的 FD 帧必然跨 2 包）
- 一个 USB 包里也可以**包含多个完整的协议帧**
- 接收方向（device → host）同理：一次 `read` 可能返回半帧，也可能返回若干整帧

**写代码时不要假设"一次 USB 读 = 一个协议帧"，必须实现缓冲 + 增量解析。**

### 1.2 解析器同步规则

- 首个字节必须是 `0xAA`；否则丢弃该字节，继续找下一个 `0xAA`
- 收到 `0xAA` 后，第二字节必须是 `0xAA` 或 `0xAB`；
  若又是 `0xAA` 则保持等待（容忍 `AA AA AA ...` 前缀），否则丢弃并回到空闲态
- 帧体收完即触发处理并回到空闲态
- 畸形帧（如经典帧 DLC > 8）会被丢弃，并置 `ERR_USB_BADFRAME`（见 §9）

> 固件**不校验**数据帧内容（没有应用层 CRC，见 §10），只校验 CTRL 字段的合法性。
> 因此上位机侧写错 ID 字节序不会被拒绝，只会发出错误的 CAN 帧 —— 请仔细核对 §5.2。

---

## 2. 帧分类总表

所有帧都以 `0xAA` 开头。第二字节区分帧类型与方向：

| 帧头 | 名称 | 方向 | 总长 | 用途 |
|---|---|---|---|---|
| `AA AA` | 命令帧 | host → device | **固定 7 字节** | 控制：设波特率、启停发送、查状态等 |
| `AA AB` | 数据帧 | **双向** | `3 + ID字节数 + 数据字节数`（5–71） | 发送一帧 CAN/CAN FD（host→dev）；上报一帧（dev→host） |
| `AA AC` | 响应帧 | device → host | 7 / 4+LEN / 12 | 波特率锁定、固件版本、错误帧计数 |
| `AA AD` | 告警帧 | device → host | `5 + LEN(TEXT)` | ASCII 错误名，供日志显示 |

> `AA AB` 是**双向共用**的同一格式：设备不需要知道方向，靠"谁发的"区分。
> 上位机解析 IN 数据时按 `AA AB` 解；发 OUT 时也用 `AA AB`。

---

## 3. 命令帧（`AA AA`）

### 3.1 格式

```text
AA AA | CMD (1B) | VALUE (4B, 小端 uint32)
```

- 总长**固定 7 字节**
- `VALUE` 为 32 位无符号整数，**小端序**（低字节在前）
- 无需 VALUE 的命令也必须发 4 个 `0x00` 占位

### 3.2 命令字典

| CMD | 名称 | VALUE | 合法值 | 非法值行为 |
|---|---|---|---|---|
| `0x01` | SET_BITRATE | 仲裁段波特率 (Hz) | 见 §3.3 | 置 `ERR_USB_BADFRAME` |
| `0x02` | TX_START | `0`=单次 / `1`=连续 | `0`, `1` | 非 0 即按"连续"处理（`value & 1`） |
| `0x03` | TX_STOP | — | 任意 | 无 |
| `0x04` | GET_STATUS | — | 任意 | 无（总是回一行 ASCII） |
| `0x05` | ENTER_DFU | — | 任意 | 无（成功后**不再返回**） |
| `0x06` | SET_INTERVAL | 周期 (ms) | `1`–`1000` | 置 `ERR_USB_BADFRAME` |
| `0x07` | SET_MODE | `0`=单帧 / `1`=列表 | `0`, `1` | 置 `ERR_USB_BADFRAME` |
| `0x08` | SET_DATA_BITRATE | FD 数据段波特率 (Hz) | `2000000`, `5000000` | 置 `ERR_USB_BADFRAME` |
| `0x09` | SET_WORK_MODE | `0`=CAN / `1`=RS485 | `0`, `1` | 置 `ERR_USB_BADFRAME` |
| `0x0A` | GET_VERSION | — | 任意 | 无（回 `AA AC 02`） |
| `0x0B` | SET_PAUSE | `1`=暂停 / `0`=恢复 | `0`, `1` | 置 `ERR_USB_BADFRAME` |
| `0x0C` | CLEAR_ERRORS | — | 任意 | 无 |
| 其它 | — | — | — | 置 `ERR_USB_BADFRAME` |

**逐条说明**

- **`0x01` SET_BITRATE** —— 设置仲裁段波特率。总线开启时**热重配**立即生效，且**不清空发送缓存**（已入队的单帧缓存、帧列表、运行中的任务、接收队列全部保留）。因此"先发数据帧再设波特率"或"先设波特率再发数据帧"都可行。
- **`0x02` TX_START** —— 触发发送缓存。`0` = 单次（列表模式下发一遍即停），`1` = 连续（按 `SET_INTERVAL` 周期重发）。
  - **缓存为空时不报错，静默忽略**（`rc == -1` 分支不打错误位）。这意味着忘记先发数据帧只会"什么都没发生"，不会告诉你原因。
  - 硬件级拒绝（TX FIFO 满 / 无 ACK）会置 `ERR_CAN_TXFAIL`。
- **`0x03` TX_STOP** —— 停止发送任务并关闭周期定时器。
- **`0x04` GET_STATUS** —— 返回一行 ASCII 状态（见 §8）。
- **`0x05` ENTER_DFU** —— 停发送与定时器后跳入出厂 bootloader。**成功不返回**，设备重新枚举为 `0x19F5:0x4537`。
- **`0x06` SET_INTERVAL** —— 周期发送间隔，`1`–`1000` ms。只重载定时器，不影响已入队帧。
- **`0x07` SET_MODE** —— `0` 单帧（每个数据帧覆盖"最近一帧"缓存）；`1` 列表（数据帧 FIFO 追加，最多 **20 帧**，超出丢弃并置 `ERR_CAN_TXFAIL`）。
- **`0x08` SET_DATA_BITRATE** —— CAN FD 数据段波特率，仅 `2 MHz` / `5 MHz`。与 `0x01` 互相独立。
- **`0x09` SET_WORK_MODE** —— 切 RS485 会停 CAN 状态机/中断/定时器；切回 CAN 释放 RS485 并重启 FDCAN。
- **`0x0A` GET_VERSION** —— 回 `AA AC 02 | LEN | TEXT`。
- **`0x0B` SET_PAUSE** —— 暂停时停发送任务与定时器、关接收中断、清空接收队列；恢复时丢弃硬件 FIFO 滞留帧后重新使能。
- **`0x0C` CLEAR_ERRORS** —— 清零粘滞错误寄存器并丢弃未上报的 `AA AD`。

### 3.3 支持的波特率

**仲裁段（CMD `0x01`，单位 Hz）**

```text
10000  20000  50000  83333  100000  125000
250000 500000 750000 1000000 5000000
```

**数据段（CMD `0x08`，单位 Hz）**

```text
2000000  5000000
```

> 其余值一律置 `ERR_USB_BADFRAME`，波特率保持不变。

### 3.4 命令帧字节示例

```text
AA AA 01 40 42 0F 00     SET_BITRATE    1 000 000 Hz  (0x000F4240 小端)
AA AA 01 20 A1 07 00     SET_BITRATE      500 000 Hz  (0x0007A120 小端)
AA AA 02 00 00 00 00     TX_START       单次
AA AA 02 01 00 00 00     TX_START       连续
AA AA 03 00 00 00 00     TX_STOP
AA AA 04 00 00 00 00     GET_STATUS
AA AA 05 00 00 00 00     ENTER_DFU
AA AA 06 64 00 00 00     SET_INTERVAL   100 ms
AA AA 07 00 00 00 00     SET_MODE       单帧
AA AA 07 01 00 00 00     SET_MODE       列表
AA AA 08 40 4B 4C 00     SET_DATA_BITRATE  5 000 000 Hz
AA AA 09 01 00 00 00     SET_WORK_MODE  RS485
AA AA 0A 00 00 00 00     GET_VERSION
AA AA 0B 01 00 00 00     SET_PAUSE      暂停
AA AA 0C 00 00 00 00     CLEAR_ERRORS
```

---

## 4. 数据帧（`AA AB`）

### 4.1 格式

```text
AA AB | CTRL (1B) | ID (2B 或 4B, 大端) | DATA (0..64B)
```

**没有应用层 CRC。** USB bulk 传输本身每包带 CRC16 并自动重传，链路无损，应用层再加校验只是每帧白花 2–3 字节。

总长公式：

```text
总字节数 = 2 (帧头) + 1 (CTRL) + ID字节数 + 数据字节数
ID字节数 = 2 (标准) 或 4 (扩展)
数据字节数 = DLC 查表所得，远程帧强制为 0
```

### 4.2 CTRL 控制字节（逐位）

| 位 | 掩码 | 名称 | 含义 |
|---|---|---|---|
| 7 | `0x80` | `FDF` | `1` = CAN FD 帧；`0` = 经典 CAN |
| 6 | `0x40` | `BRS` | `1` = 启用位速率切换。**仅 FD 有效**，经典帧必须为 `0` |
| 5 | `0x20` | `RTR` | `1` = 远程帧。**仅经典有效**，FD 帧必须为 `0` |
| 4 | `0x10` | `IDE` | `1` = 扩展 ID（29 bit，4 字节）；`0` = 标准 ID（11 bit，2 字节） |
| 3..0 | `0x0F` | `DLC` | 数据长度码，见 §4.4 |

### 4.3 ID 字段编码

| `CTRL.IDE` | 字节数 | 位宽 | 字节序 |
|---|---|---|---|
| `0`（标准） | 2 | 11 bit（有效值 ≤ `0x7FF`） | **大端**（高字节在前） |
| `1`（扩展） | 4 | 29 bit（有效值 ≤ `0x1FFFFFFF`） | **大端**（高字节在前） |

示例：标准 ID `0x123` → `01 23`；扩展 ID `0x18FF50E5` → `18 FF 50 E5`。

> ID 宽度**由本帧自己的 `CTRL.IDE` 决定**，与其它帧无关。同一串数据里可以混发标准帧和扩展帧。
> 固件不校验 ID 是否超出位宽范围，写错会原样发给 FDCAN。

### 4.4 DLC → 数据字节数

| DLC | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 字节 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 12 | 16 | 20 | 24 | 32 | 48 | 64 |

**合法性约束**（违反则整帧丢弃 + 置 `ERR_USB_BADFRAME`）：

- 经典帧（`FDF=0`）：`DLC` 必须 **≤ 8**
- 任何帧：数据字节数必须 ≤ 64
- 远程帧（经典 `RTR=1`）：**无 DATA 字节**，`DLC` 仍表示请求的数据长度

> FD 的 DLC 9–15 只对应 12/16/20/24/32/48/64 这 7 种长度，**不存在中间长度**。
> 例如要发 20 字节，DLC 必须是 `11`，而不是"20"。

### 4.5 标准/扩展 × 经典/FD × 数据/远程 组合矩阵

| # | ID 类型 | 帧类型 | CTRL 值 | ID 字节数 | 数据字节数 | 总长 |
|---|---|---|---|---|---|---|
| 1 | 标准 | 经典 · 数据 | `DLC` (`00`–`08`) | 2 | `DLC` (0–8) | `5 + DLC` |
| 2 | 标准 | 经典 · 远程 | `20 \| DLC` | 2 | **0** | **5** |
| 3 | 标准 | FD · 无 BRS | `80 \| DLC` | 2 | 查表 (0–64) | `5 + 字节数` |
| 4 | 标准 | FD · BRS | `C0 \| DLC` | 2 | 查表 (0–64) | `5 + 字节数` |
| 5 | 扩展 | 经典 · 数据 | `10 \| DLC` | 4 | `DLC` (0–8) | `7 + DLC` |
| 6 | 扩展 | 经典 · 远程 | `30 \| DLC` | 4 | **0** | **7** |
| 7 | 扩展 | FD · 无 BRS | `90 \| DLC` | 4 | 查表 (0–64) | `7 + 字节数` |
| 8 | 扩展 | FD · BRS | `D0 \| DLC` | 4 | 查表 (0–64) | `7 + 字节数` |

### 4.6 数据帧字节示例（全部 8 种组合，均已实机验证格式）

```text
#1  标准 · 经典 · 数据  ID=0x123, 2B  DE AD                     (7 字节)
    AA AB 02 01 23 DE AD

#1  标准 · 经典 · 数据  ID=0x123, 8B  00..07                    (13 字节)
    AA AB 08 01 23 00 01 02 03 04 05 06 07

#1  标准 · 经典 · 空帧  ID=0x123, DLC=0                          (5 字节)
    AA AB 00 01 23

#2  标准 · 经典 · 远程  ID=0x123, DLC=2 (无 DATA)                (5 字节)
    AA AB 20 01 23

#5  扩展 · 经典 · 数据  ID=0x18FF50E5, 8B 01..08                 (15 字节)
    AA AB 18 18 FF 50 E5 01 02 03 04 05 06 07 08

#8  扩展 · FD · BRS     ID=0x18FF50E5, 8B 01..08                 (15 字节)
    AA AB D8 18 FF 50 E5 01 02 03 04 05 06 07 08

#7  扩展 · FD · 无 BRS  ID=0x0ABCDEF0, 12B 00..0B (DLC=9)        (19 字节)
    AA AB 99 0A BC DE F0 00 01 02 03 04 05 06 07 08 09 0A 0B

#4  标准 · FD · BRS     ID=0x100, 64B 00..3F (DLC=15)            (69 字节)
    AA AB CF 01 00 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
    10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 20 21 22 23 24
    25 26 27 28 29 2A 2B 2C 2D 2E 2F 30 31 32 33 34 35 36 37 38 39
    3A 3B 3C 3D 3E 3F

#4  标准 · FD · BRS     ID=0x7FF, 20B 00..13 (DLC=11)            (25 字节)
    AA AB CB 07 FF 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
    10 11 12 13
```

> 注意 69 字节的 FD 帧**必然跨越 2 个 USB 包**（64 + 5）。上位机解析时必须容忍跨包。

---

## 5. 响应帧（`AA AC`，device → host）

三种 id，靠第 3 字节区分。

### 5.1 波特率锁定（id = `0x01`）

```text
AA AC | 01 | BITRATE (4B, 小端)
```

总长固定 **7 字节**。自动波特率扫描锁定某个速率时主动发出一次。

```text
锁定 500 kbit/s:  AA AC 01 20 A1 07 00
```

| 字段 | 说明 |
|---|---|
| `BITRATE` | 锁定的仲裁段波特率，单位 Hz，小端 uint32 |

### 5.2 固件版本（id = `0x02`）

```text
AA AC | 02 | LEN (1B) | TEXT (LEN 字节 ASCII)
```

总长 `4 + LEN`，`LEN` 上限 60（固件侧截断）。

```text
v30.0.7:  AA AC 02 07 76 33 30 2E 30 2E 37        (11 字节)
```

### 5.3 错误帧累计（id = `0x03`）

```text
AA AC | 03 | TEC_FRAMES (4B 小端) | REC_FRAMES (4B 小端) | CRC8 (1B)
```

总长固定 **12 字节**。`CRC8` 覆盖**前面 8 个计数字节**（不含帧头与 id）。

```text
TEC=0, REC=0        : AA AC 03 00 00 00 00 00 00 00 00 00
TEC=1234, REC=56    : AA AC 03 D2 04 00 00 38 00 00 00 1E
                        └─1234 小端─┘ └─56 小端──┘ └ CRC
```

| 字段 | 说明 |
|---|---|
| `TEC_FRAMES` | 发送错误帧累计数（硬件 TEC 增量 ÷8 累加，饱和到 `0xFFFFFFFF`） |
| `REC_FRAMES` | 接收错误帧累计数（同上） |
| `CRC8` | poly `0x07`、init 0，覆盖 8 字节计数 |

> 该帧**非请求主动上报**。硬件 TEC 复位后累计值仍保持，上电清零。

---

## 6. 告警帧（`AA AD`，device → host）

```text
AA AD | TEXT (ASCII) | DA AA | CRC8 (1B)
```

总长 `2 + LEN + 2 + 1`。`TEXT` 是错误枚举名（见 §9）。

```text
AA AD 45 52 52 5F 55 53 42 5F 42 41 44 46 52 41 4D 45 DA AA 81
      └────────── "ERR_USB_BADFRAME" ──────────┘ └─尾标─┘ └CRC
```

| 字段 | 说明 |
|---|---|
| `TEXT` | ASCII 错误枚举名，如 `ERR_USB_BADFRAME`。全部字节 `< 0x80` |
| 尾标 `DA AA` | 两个字节**都 > `0x7F`**。因为 `TEXT` 全是 ASCII（`< 0x80`），所以这个组合不可能出现在正文里，是可靠的终止符 |
| `CRC8` | poly `0x07`、init 0，**只覆盖 `TEXT`**（`AA AD` 和 `DA AA` 都不参与） |

> 触发：`error_assert()` 将错误排队，由**主循环**取出发送（不在中断上下文发 USB），因此可能有延迟。
> 建议：即使 CRC 不符也把正文显示出来并标注"CRC 校验失败"，比直接丢弃更有用。

---

## 7. 解析建议（device → host 状态机）

给你一份正确的解析骨架逻辑：

```text
loop:
  byte = next()
  if state == IDLE:
      if byte == 0xAA: state = SOF1
  elif state == SOF1:
      if   byte == 0xAA: state = CMD        # 命令回显（设备不会发，但保持健壮）
      elif byte == 0xAB: state = DATA_CTRL
      elif byte == 0xAC: state = RSP_ID
      elif byte == 0xAD: state = LOG_TEXT
      else: state = IDLE
  elif state == DATA_CTRL:
      ctrl = byte
      dlc  = ctrl & 0x0F
      n    = DLC2BYTES[dlc]
      if (ctrl & 0x80) == 0 and dlc > 8: state = IDLE; continue   # 非法
      if (ctrl & 0x20) and (ctrl & 0x80) == 0: n = 0              # 远程帧无 DATA
      idb  = 4 if (ctrl & 0x10) else 2
      ... 收集 idb + n 字节后派发，然后 state = IDLE
  # RSP_ID / LOG_TEXT 按 §5 / §6 收齐后派发
```

**关键点**：

1. `DLC` 要在收到 `CTRL` 后立刻查表，才能知道后面还要收多少字节
2. 远程帧的数据字节数要强制为 0，否则会把下一帧的字节吃掉
3. 缓冲区要能跨 USB 读保留半帧，不能每次 `read` 都清空

---

## 8. 状态行（CMD `0x04` 的应答）

`AA AC` 之外的另一种应答：**纯 ASCII 一行，以 `\r`（`0x0D`）结尾**，没有帧头也没有 CRC。
它通过**数据帧通道**（`AA AB` 之后的普通字节流）发出，因此解析器需要在遇到非 `AA` 开头的数据时按 ASCII 处理。

```text
ST:bus=1 mode=S job=0 tmr=0 cur=1 pend=0 act=0 drop=0 err=00000000\r
```

**共 9 个字段**：

| 字段 | 类型 | 含义 |
|---|---|---|
| `bus` | `0`/`1` | CAN 总线是否开启（`1` = 已开启） |
| `mode` | `S`/`L` | 发送模式：`S` = 单帧，`L` = 列表 |
| `job` | `0`/`1` | 发送任务是否在运行 |
| `tmr` | `0`/`1` | 周期定时器是否使能 |
| `cur` | `0`/`1` | 单帧缓存是否有效（有可发的"最近一帧"） |
| `pend` | 十进制 | pending 列表帧数（本次累积、尚未封存） |
| `act` | 十进制 | active 列表帧数（已封存、可播放） |
| `drop` | 十进制 | **发送帧被丢弃的累计数**（TX 队列满导致） |
| `err` | 8 位十六进制 | 粘滞错误寄存器，见 §9 |

> `drop` 字段在旧文档里缺失，解析时务必按 9 个字段处理，不要按 8 个。

---

## 9. 错误寄存器（`err` 字段，共 12 位）

`err` 是最多 8 位十六进制（32 位寄存器的低 8 位十六进制表示，但只有 bit 0–11 有意义）。

| bit | 名称 | 含义 | 可否自动清除 |
|---|---|---|---|
| 0 | `ERR_PERIPHINIT` | 外设初始化失败 | 否 |
| 1 | `ERR_USBTX_BUSY` | USB TX 忙 | 否 |
| 2 | `ERR_CAN_TXFAIL` | CAN 发送失败（FIFO 满 / 列表溢出） | 否 |
| 3 | `ERR_CANRXFIFO_OVERFLOW` | CAN 接收软环溢出（覆盖最旧帧） | **是**（队列排空后清除） |
| 4 | `ERR_FULLBUF_CANTX` | 历史保留 | — |
| 5 | `ERR_FULLBUF_USBRX` | USB 接收缓冲满 | 否 |
| 6 | `ERR_FULLBUF_USBTX` | USB 发送缓冲满 | 否 |
| 7 | `ERR_USB_BADFRAME` | 未知命令 / 畸形帧 | 否 |
| 8 | `ERR_USB_CRC` | 诊断帧（`AA AC`/`AA AD`）CRC-8 失败 | 否 |
| 9 | `ERR_FRAMELIST_FULL` | 历史保留（现走 `ERR_CAN_TXFAIL`） | — |
| 10 | `ERR_CAN_ACK` | CAN 发送无节点应答（总线上没有其它节点） | 否 |
| 11 | `ERR_CAN_BUSOFF` | 严重协议错误导致 Bus-Off | 否 |

- 除 `bit 3` 外，错误位**粘滞保持至复位**（或发 CMD `0x0C` 清除）
- `bit 10` / `bit 11` 在旧文档里缺失，调试"发不出去"时优先看这两位

---

## 10. CRC-8 算法（仅用于 `AA AC` / `AA AD`）

| 参数 | 值 |
|---|---|
| 多项式 | `0x07`（x⁷ + x² + x + 1） |
| 初值 | `0x00` |
| 反射 | 无 |
| 异或输出 | 无 |
| 位序 | MSB-first |

**数据帧 `AA AB` 不使用这个 CRC**（见 §4.1）。

**测试向量**（可直接用来验证你的实现）：

| 输入 | CRC-8 |
|---|---|
| 空串 | `0x00` |
| `00` | `0x00` |
| `"123456789"` | `0xF4` |
| `"ERR_USB_BADFRAME"` | `0x81` |

> `"123456789"` → `0xF4` 是 CRC-8/SMBUS 的标准检验值，可作为实现正确性的判据。

---

## 11. 参考实现

### 11.1 Python

```python
import struct

DLC2BYTES = [0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64]
BYTES2DLC = {n: i for i, n in enumerate(DLC2BYTES)}

def pack_data_frame(can_id, data=b"", ext=False, fd=False, brs=False, rtr=False):
    """组一个 AA AB 数据帧（host -> device 发送用）"""
    n = len(data)
    if n not in BYTES2DLC:
        raise ValueError("data length %d not representable in DLC" % n)
    dlc = BYTES2DLC[n]
    if not fd and dlc > 8:
        raise ValueError("classic frame DLC must be <= 8")
    ctrl = 0
    if fd:                 ctrl |= 0x80
    if fd and brs:         ctrl |= 0x40
    if (not fd) and rtr:   ctrl |= 0x20
    if ext:                ctrl |= 0x10
    ctrl |= dlc & 0x0F
    idb = 4 if ext else 2
    payload = b"" if (rtr and not fd) else bytes(data)
    return bytes([0xAA, 0xAB, ctrl]) + can_id.to_bytes(idb, "big") + payload

def pack_command(cmd, value=0):
    """组一个 AA AA 命令帧"""
    return bytes([0xAA, 0xAA, cmd]) + struct.pack("<I", value)

def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if (c & 0x80) else (c << 1) & 0xFF
    return c

class FrameParser:
    """device -> host 的增量解析器（可跨 USB 读）"""
    IDLE, SOF1, DCTRL, DID, DBODY, RSPID, RSPVER, RSPERR, LOGTEXT = range(9)

    def __init__(self):
        self.st = self.IDLE
        self.buf = bytearray()
        self.need = 0
        self.ctrl = 0
        self.idb = 2
        self.nbytes = 0

    def feed(self, chunk):
        out = []
        for byte in chunk:
            self._step(byte, out)
        return out

    def _step(self, b, out):
        st = self.st
        if st == self.IDLE:
            if b == 0xAA: self.st = self.SOF1
        elif st == self.SOF1:
            if   b == 0xAB: self.st, self.buf = self.DCTRL, bytearray()
            elif b == 0xAC: self.st, self.buf = self.RSPID, bytearray()
            elif b == 0xAD: self.st, self.buf = self.LOGTEXT, bytearray()
            elif b != 0xAA: self.st = self.IDLE
        elif st == self.DCTRL:
            self.ctrl = b
            dlc = b & 0x0F
            fd  = bool(b & 0x80)
            if not fd and dlc > 8:
                self.st = self.IDLE; return
            self.nbytes = 0 if ((b & 0x20) and not fd) else DLC2BYTES[dlc]
            self.idb = 4 if (b & 0x10) else 2
            self.buf = bytearray([b])
            self.need = self.idb
            self.st = self.DID
        elif st == self.DID:
            self.buf.append(b)
            self.need -= 1
            if self.need == 0:
                if self.nbytes > 0:
                    self.need = self.nbytes; self.st = self.DBODY
                else:
                    out.append(self._emit_data()); self.st = self.IDLE
        elif st == self.DBODY:
            self.buf.append(b)
            self.need -= 1
            if self.need == 0:
                out.append(self._emit_data()); self.st = self.IDLE
        elif st in (self.RSPID, self.RSPVER, self.RSPERR, self.LOGTEXT):
            self._step_asc(b, out)

    def _emit_data(self):
        ctrl = self.buf[0]
        idb  = 4 if (ctrl & 0x10) else 2
        raw  = bytes(self.buf[1:1+idb])
        can_id = int.from_bytes(raw, "big")
        return dict(type="can", ctrl=ctrl, id=can_id,
                    ext=bool(ctrl & 0x10), fd=bool(ctrl & 0x80),
                    brs=bool(ctrl & 0x40), rtr=bool(ctrl & 0x20),
                    data=bytes(self.buf[1+idb:]))
```

### 11.2 发送一帧的最小完整调用

```python
def send_one_can(dev_out, can_id, data, ext=False, fd=False, brs=False):
    dev_out(pack_command(0x07, 0))                       # SET_MODE 单帧（可省，默认即单帧）
    dev_out(pack_data_frame(can_id, data, ext, fd, brs))  # 放入缓存
    dev_out(pack_command(0x02, 0))                       # TX_START 单次 ← 必须
```

---

## 12. 常见坑清单

| # | 坑 | 症状 | 正确做法 |
|---|---|---|---|
| 1 | 只发 `AA AB` 不触发 | 什么都没发生，状态行 `cur=1` 但总线无帧 | 必须补发 `AA AA 02 …` |
| 2 | 经典帧 `DLC > 8` | 帧被静默丢弃，`err` 出现 `0x80`（bit 7） | 经典帧最多 8 字节；要发更多必须用 FD |
| 3 | FD 帧用了"字节数"当 DLC | 数据长度错乱或帧被拒 | 必须查表：20 字节 → DLC `11`，不是 `20` |
| 4 | ID 字节序写反 | 总线上出现错误的 ID | 大端（高字节在前） |
| 5 | 用 4 字节 ID 却没置 `IDE` | 固件只收 2 字节 ID，后面数据整体错位 | 扩展帧必须置 `CTRL.IDE = 1` |
| 6 | 经典帧置了 `BRS` | 行为未定义 | `BRS` 只在 `FDF=1` 时置位 |
| 7 | 远程帧还带了 DATA | 会吃掉下一帧的字节 | 远程帧数据字节数强制为 0（固件按 `RTR` 自动处理，但**上位机解析上报帧时也要这样判**） |
| 8 | 假设"一次 USB 读 = 一帧" | 69 字节 FD 帧必然跨包 → 解析错乱 | 实现跨包缓冲 |
| 9 | 按 8 个字段解析状态行 | 从 `drop` 开始全部错位 | 实际是 **9 个**字段 |
| 10 | 误以为 `AA AD` 的 CRC 覆盖整帧 | 校验永远失败 | 只覆盖 `TEXT`，不含帧头与 `DA AA` |
| 11 | 误以为数据帧带 CRC | 多发 2–3 字节 → 后续帧全部错位 | `AA AB` **没有**应用层 CRC |
| 12 | `TX_START` 前没设波特率 | 用默认 1 Mbit/s 发帧，总线上没人能解 | 先发 CMD `0x01` 明确目标速率 |

---

## 13. 容量与节奏指南

| 项 | 值 | 说明 |
|---|---|---|
| 单包上限 | 64 字节 | Full-Speed bulk |
| 最大协议帧 | 71 字节 | 扩展 ID + 64 字节 FD 数据 = `2+1+4+64` |
| 帧列表深度 | **20 帧** | 列表模式下超出丢弃并置 `ERR_CAN_TXFAIL` |
| 周期间隔 | 1–1000 ms | CMD `0x06` |
| 固件 TX 队列 | 192 个包槽 | 满则丢弃最旧，置 `ERR_FULLBUF_USBTX` |
| 固件 RX 软环 | 64 帧 | 满则覆盖最旧，置 `ERR_CANRXFIFO_OVERFLOW` |
| 实测上行能力 | ~8000 帧/s 零丢帧 | 总线 8000 帧/s 下连续 300 s 验证通过 |
| USB 带宽占用 | 8000 帧/s ≈ 117 KB/s | 占 Full-Speed 理论 1.19 MB/s 的约 10% |

**节奏建议**

- 批量发送时不必逐帧等待应答；`AA AB` 只是入缓存，可以连续灌
- 连续发送留出 ≥ 1 ms 间隔可显著降低主机侧调度抖动
- 长时间不读上行会导致固件 TX 环积压（`ERR_FULLBUF_USBTX`），主机需持续 `read`
- 若上行突然中断，先查 CMD `0x04` 的 `err` 字段（重点看 bit 3 / 6 / 10）

---

## 14. 版本与兼容性

| 固件 | 变化 | 与本文档 |
|---|---|---|
| ≤ v30.0.5 | **数据帧带应用层 CRC**（经典 2B / FD 3B）；USB 为 CDC ACM 类 | **不兼容**，请勿混用 |
| **v30.0.7+** | 数据帧**去掉** CRC；USB 改 **vendor specific** 类 + WCID；上行改包队列 | **本文档适用** |

判断当前设备固件版本：发 `AA AA 0A 00 00 00 00`，读 `AA AC 02 | LEN | TEXT`。

---

## 附录 A：与旧文档的差异（修订记录）

`docs/FIRMWARE_PROTOCOL.md` **曾**存在以下错误。本文档自始即按正确行为编写，
旧文档也已同步修正（见其附录 A）：

| # | 旧文档问题 | 本文档对应 |
|---|---|---|
| 1 | §11 示例仍带 `<CRC15:2B>` | §4.6 全部示例无 CRC |
| 2 | §7 状态行只列 8 字段，缺 `drop=` | §8 明确 **9 字段** |
| 3 | §8 错误表缺 bit 10 / bit 11 | §9 补全 12 位 |
| 4 | §3 称 CMD 2 无缓存会置 `ERR_USB_BADFRAME` | §3.2 修正：**静默忽略** |
| 5 | §12 称 ID 格式"固定 STANDARD" | §4.3 按每帧 `CTRL.IDE` 决定 |
| 6 | 未强调"数据帧只入缓存" | §0 快速上手 / §12 坑 #1 |

---

## 构建与烧录

### 固件（GNU Arm Embedded / make）

```bash
make -j
# 产物在 build/canable2-n32-<版本>-<gitrev>.bin / .hex / .elf
```

Keil：打开 `MDK-ARM/canable2.uvprojx`。

### 上位机（Windows）

```bash
cd host
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./package.sh          # 打包安装包 + 便携 zip（需 NSIS）
```

详细说明见 [host/README.md](host/README.md)。

---

## 目录结构

```
src/ inc/          固件源码
Drivers/           N32 HAL / CMSIS
host/              Qt6 上位机与安装脚本
docs/              协议文档（USB_FRAME_PROTOCOL / FIRMWARE_PROTOCOL / EDS）
release/           已发布固件 bin/hex
```

---

## 许可证

本项目沿用原 CANable / 厂商 SDK 相关许可；应用层代码以仓库内声明为准。
