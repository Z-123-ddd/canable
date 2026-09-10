# CANable2-N32 上位机 (host)

基于 Qt6 (Widgets + SerialPort) 的 CANable2-N32 上位机。**支持两条通信路径**：
- **串口(CDC)**：设备以 USB CDC 虚拟串口呈现时，用 `QSerialPort` 通信；
- **USB(libusb)**：设备被 libusb/WinUSB 驱动接管（无 COM 口）时，用 libusb 直接走 Bulk 端点
  （OUT `0x01` / IN `0x81`）通信。

界面顶部「**传输**」下拉切换两条路径；切换时自动刷新设备列表。两条路径共用同一套协议
解析与 UI，协议与固件 `inc/usb_proto.h` 逐字节对齐。

## 控件约定
- **传输** → 下拉框：`串口(CDC)` / `USB(libusb)`（切换自动刷新下方设备列表）
- **端口/设备** → 下拉框：串口模式列 COM 口，USB 模式列全部可用 libusb 设备
  （本应用设备 `16D0:117E` 排首位）；两模式下均可选
- **刷新** → 按当前传输重新枚举
- **CAN 波特率** → 下拉框（选择即下发命令 1）
- **列表模式**   → 按钮（可勾选切换 单帧/列表，下发命令 7）
- **循环发送**   → 复选框（决定 TX_START 的 value：勾=1 连续 / 不勾=0 单次）
- 周期(ms) 输入框 → 命令 6（1..1000）
- 帧编辑（ID/扩展/FD/BRS/RTR/DLC/数据）+ 加入列表 / 清空列表
- 发送 / 停止 / 查询状态 → 命令 2 / 3 / 4

## 构建（Windows，工具链在 D 盘）
```bash
source /d/Qt/qt-tools-env.sh          # 或 cmd: call D:\Qt\qt-tools-env.bat
cd host
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/mingw_64
cmake --build build
# 产物: host/build/canable2_host.exe
```

运行需要 Qt DLL 与 `libusb-1.0.dll`（构建后自动拷到 exe 同目录）：
```bash
./build/canable2_host.exe
```

无头自检：
```bash
./build/canable2_host.exe --selftest     # 协议编解码自检
./build/canable2_host.exe --uitest       # 界面逻辑自检（含传输切换）
./build/canable2_host.exe --usb-open     # 枚举 libusb 设备并尝试打开
./build/canable2_host.exe --dfu-scan     # 枚举 USB 设备(读 DFU 设备 VID/PID)
```

> libusb 路径需设备的驱动为 WinUSB/libusbK（可用 [Zadig](https://zadig.akeo.ie/) 安装）。
> 装成 libusb 驱动后该设备不再有 COM 口，此时在「传输」里选 `USB(libusb)`。


## 协议速览
- 命令帧 `AA AA | CMD | VALUE(4, LE)`
  - 1 SET_BITRATE(Hz) / 2 TX_START(0单次,1连续) / 3 TX_STOP
  - 4 GET_STATUS / 5 ENTER_DFU / 6 SET_INTERVAL(ms) / 7 SET_MODE(0单帧,1列表)
- 数据帧 `AA AB | ID(2/4) | CTRL | DATA | CRC`
  - CTRL: bit7 FDF, bit6 BRS, bit5 RTR, bits3..0 DLC
  - CRC: 经典 CRC-15 / FD<=16 CRC-17 / FD>16 CRC-21（MSB-first, init0, xorout0）
- 接收上报：MCU 以同样的 `AA AB` 二进制帧上报；`AA AC 01 <hz:4B LE>` 为波特率锁定；
  GET_STATUS 回一行 ASCII `ST:bus=.. mode=S/L .. err=.....`

详见仓库 `docs/FIRMWARE_PROTOCOL.md`。
