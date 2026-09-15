# CANable2-N32 上位机 (host)

基于 Qt6 (Widgets + SerialPort) 的 CANable2-N32 上位机。**支持两条通信路径**：
- **串口(CDC)**：设备以 USB CDC 虚拟串口呈现时，用 `QSerialPort` 通信；
- **USB(libusb)**：设备被 libusb/WinUSB 驱动接管（无 COM 口）时，用 libusb 直接走 Bulk 端点
  （OUT `0x01` / IN `0x81`）通信。

界面顶部「**传输**」下拉切换两条路径；切换时自动刷新设备列表。两条路径共用同一套协议
解析与 UI，协议与固件 `inc/usb_proto.h` 逐字节对齐。

## 顶部菜单栏
- 快捷键：**Ctrl+P** 打开「协议」、**Ctrl+M** 打开「模式」、**Ctrl+H** 打开「帮助」
- **协议** → `CyberBeast` / `Simple` / `Open` / `通用CAN`（切换 CAN 页内的协议调试页）。**通用CAN** 为默认（即通用 CAN 调试 UI）；其余为独立页面
- **Simple 协议页**：按 **CAN Simple**（ODrive，11-bit 标准帧/8 字节/小端，`CAN ID=(node_id<<5)+cmd_id`）封装命令下拉；选命令后按所需参数输入**十进制**，上位机换算（如位置模式：输入「度」→ 结合减速比 → 电机轴转数 → 编码）并组帧；左命令区 / 右接收区；发送语义与通用 UI 一致（数据变则打包、不变只发触发）
- **模式** → `CAN` / `RS485`：切换工作模式（两个独立页面，QStackedWidget）。连接后切换即下发**命令 9**（切换固件工作模式）；未连接仅切页面
- **帮助** → 「查询固件版本」（发**命令 10**）/「关于」（显示软件版本 + 设备固件版本）

> 说明：命令 **10（GET_VERSION）** 已实现（回 `AA AC 02 | LEN | ASCII`）。RS485 页当前为**布局占位**，无具体收发功能。

## 控件约定
- **传输** → 下拉框：`串口(CDC)` / `USB(libusb)`（切换自动刷新下方设备列表）
- **端口/设备** → 下拉框：串口模式列 COM 口，USB 模式列全部可用 libusb 设备
  （本应用设备 `16D0:117E` 排首位）；两模式下均可选
- **刷新** → 按当前传输重新枚举
- **仲裁域波特率** → 下拉框（默认 1M；CAN 仲裁段速率，下发命令 1）
- **数据域波特率** → 下拉框（默认 5M，可选 2M/5M；CAN FD 数据段速率，下发命令 8）
- **列表模式**   → 按钮（可勾选切换 单帧/列表，下发命令 7）
- **循环发送**   → 复选框（决定 TX_START 的 value：勾=1 连续 / 不勾=0 单次）
- 周期(ms) 输入框 → 命令 6（1..1000）
- 帧编辑（ID/扩展/FD/BRS/RTR/DLC/数据）+ 加入列表 / 清空列表
- 导入记录 / 导出CSV → 其他 CAN 工具记录文件的导入/导出（见下）
- 发送 / 停止 / 查询状态 → 命令 2 / 3 / 4
- **发送列表按空格** → 发送光标所在行（单帧，忽略勾选）

## 导入其他 CAN 工具的记录文件

「发送列表」旁的**导入记录**按钮可导入其他 CAN 调试/分析软件导出的记录，解析成 CAN/CAN FD
帧填入列表并勾选，点「发送」即重放（复用现有列表模式，不改变发送协议）。

自动识别以下格式：

| 格式 | 来源 | 说明 |
|---|---|---|
| Vector ASC (`.asc`) | CANalyzer / CANoe | 经典帧 / CANFD 行 / 扩展帧 `x` 后缀 |
| PEAK TRC (`.trc`) | PCAN-View | v1.0 / v1.1 / v1.3 / v2.x；按 `;$COLUMNS` 列名 + `;$DELIMITER` 声明符解析，声明与实际不符时自动探测 `, ; \|` 制表符/空白 |
| candump | SocketCAN | 可读格式 与 `-L` 日志格式 |
| 通用 CSV (`.csv`) | SavvyCAN 等 | 单列十六进制 或 每字节一列；兼容 python-can Base64 |

> 当前列表不支持远程帧(RTR)：导入时跳过并计数提示；错误帧/注释行同样跳过。
> 无头验证：`CyberBeast-CANable2.exe --import-trace <file>`。

## 构建（Windows，工具链在 D 盘）
```bash
source /d/Qt/qt-tools-env.sh          # 或 cmd: call D:\Qt\qt-tools-env.bat
cd host
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/mingw_64
cmake --build build
# 产物: host/build/CyberBeast-CANable2.exe
```

运行需要 Qt DLL 与 `libusb-1.0.dll`（构建后自动拷到 exe 同目录）：
```bash
./build/CyberBeast-CANable2.exe
```

无头自检：
```bash
./build/CyberBeast-CANable2.exe --selftest     # 协议编解码自检
./build/CyberBeast-CANable2.exe --uitest       # 界面逻辑自检（含传输切换）
./build/CyberBeast-CANable2.exe --guitest      # GUI 烧机自检：打开主窗口→短暂运行→自动关闭退出
./build/CyberBeast-CANable2.exe --usb-open     # 枚举 libusb 设备并尝试打开
./build/CyberBeast-CANable2.exe --dfu-scan       # 枚举 USB 并判断 DFU 设备是否在线
./build/CyberBeast-CANable2.exe --dfu-selftest   # DFU(DfuSe) 编码/布局解析离线自测
```

> libusb 路径需设备的驱动为 WinUSB/libusbK（可用 [Zadig](https://zadig.akeo.ie/) 安装）。
> 装成 libusb 驱动后该设备不再有 COM 口，此时在「传输」里选 `USB(libusb)`。

## USB 更新固件 (DFU)

「更新固件」流程：经当前连接发送命令 5（`ENTER_DFU`）→ MCU 跳入 N32 出厂 bootloader →
USB 重新枚举为 **DFU 设备 `0x19F5:0x4537`（"N32-BOOTLOADER"）** → 弹出对话框烧录 `.bin`
到 `0x08000000`，成功后自动跳转运行新固件。

- 固件侧跳转见仓库 `docs/FIRMWARE_PROTOCOL.md` §9（向量表在 SRAM 顶 `0x2002FE00`）。
- **烧录由厂商 32 位 `NsMcuCmdDll.dll` 完成**（协议组帧/CRC 都在 DLL 内部，无需自行实现）。
  因上位机是 64 位、无法同进程加载该 DLL，故随包分发一个 32 位 CLI
  `vendor_dfu/nsi_dfu_flash.exe`，由 `src/VendorDfuFlasher.cpp` 用 QProcess 调用：
  `ENUM → CONNECT → GET_INF → FLASH_ERASE → FLASH_DWNLD → APP_GO`。
  关键：`NS_Dl_File.+0x08` 是**数据指针**（不是 CRC），`FLASH_DWNLD` 的 `a2..a5` 传 `NULL`。
- 若 `vendor_dfu/` 缺失，自动回退到 `src/dfu_device.cpp` 的 libusb DfuSe 实现。
- Windows 需安装厂商签名驱动 **NationstechDFU**（安装包「DFU 驱动」可选组件，`pnputil` 注册）；
  否则 32 位 DLL 打不开 DFU 设备。
- 无头验证：`CyberBeast-CANable2.exe --vendor-flash <bin> [addr]`。

## 串口显示名（可选品牌化驱动）

设备是标准 USB CDC-ACM（`bDeviceClass=0x02/SubClass=0x02`），Win10/11 内置 `usbser.inf`
已能自动装 `usbser.sys` 并分配 COM 口 —— **不装任何驱动即可当 CAN 用**，只是端口显示为
系统默认的 “USB Serial Device”。

安装包里的**可选组件「品牌串口驱动 (CyberBeast USB2CAN)」**会把端口显示名改为
**“CyberBeast USB2CAN (COMx)”**。它由 `installer/drivers/CyberBeast_USB2CAN/` 提供
（`CyberBeast_USB2CAN.inf` + 签名 `.cat` + 自签名证书 `.cer`），安装时用 `certutil` 把证书
装入本机受信任存储、再 `pnputil /add-driver` 注册；**需管理员**。详见该目录下 `README.md`。

> 注意：CD 串口驱动与 DFU 驱动是**两个独立组件**，互不影响。

## 打包安装程序 (NSIS)

需要 NSIS（默认 `D:\NSIS`）与 Qt 的 `windeployqt`（随 Qt 安装）。
`package.sh` 一键完成：Release 构建 → `windeployqt` 收集 Qt 运行库 + 拷贝
`libusb-1.0.dll` → `makensis` 生成安装程序。

```bash
source /d/Qt/qt-tools-env.sh
cd host
./package.sh
# 产物: host/dist/CANable2-Host-Setup-<版本>.exe
```

- 安装程序默认装到 `D:\Programs\CANable2-Host`（可在向导里更改），带开始菜单与桌面
  快捷方式、卸载程序与注册表卸载登记。
- 卸载会删除安装目录、快捷方式与注册表项。

要点：
- `host/installer/canable2_host.nsi` 为 NSIS 脚本（UTF-8 **带 BOM**，简体中文界面）。
- 可用 `/DAPPVER=`、`/DINSTALLDIR=`、`/DREQLEVEL=`（`admin`/`user`）、`/DDEPLOYDIR=`、
  `/DOUTFILE=` 覆盖脚本变量，便于自定义版本或做免提权的安装验证。


## 协议速览
- 命令帧 `AA AA | CMD | VALUE(4, LE)`
  - 1 SET_BITRATE(Hz) / 2 TX_START(0单次,1连续) / 3 TX_STOP
  - 4 GET_STATUS / 5 ENTER_DFU / 6 SET_INTERVAL(ms) / 7 SET_MODE(0单帧,1列表)
- 数据帧 `AA AB | ID(2/4) | CTRL | DATA | CRC`
  - CTRL: bit7 FDF, bit6 BRS, bit5 RTR, bits3..0 DLC
  - CRC: 经典 CRC-15 / FD<=16 CRC-17 / FD>16 CRC-21（MSB-first, init0, xorout0）
- 接收上报：MCU 以同样的 `AA AB` 二进制帧上报；`AA AC 01 <hz:4B LE>` 为波特率锁定；
  GET_STATUS 回一行 ASCII `ST:bus=.. mode=S/L .. err=.....`
- `AA AD` 为 MCU 主动上报的警告/报错帧：`AA AD | <ASCII 错误枚举名> | DA AA | CRC`（CRC-8 仅覆盖正文，`AA AD`/`DA AA` 不参与）

详见仓库 `docs/FIRMWARE_PROTOCOL.md`。

## 日志 / 接收区 / 持久化

- **日志区仅显示“警告 / 报错”**：不再打印任何发送/接收的数据或命令十六进制；连接、枚举等
  状态提示改到状态栏。MCU 主动上报的 `AA AD` 帧以“警告/报错”形式写入日志区（CRC 不符仍
  显示并加注）。
- **接收区两种视图**（顶部「切换图标」按钮 ⇄ 点击切换，上方以「滚动 / 就地」两字提示当前模式）：
  - 默认「就地更新」：相同帧 ID 汇总到一行并累计次数；
  - 「滚动显示」：**带固定表头**（方向/帧ID/类型/长度/数据），发一条收一条逐行追加、自动滚到底部
    （表头不动、数据行滚动），可勾选「显示时间戳」。仅当竖直滑条已在最底端时才自动跟随；若手动把
    滑条上移查看历史，则新数据仍在下方追加但**不强制拉到底部**。
  - 「清空接收」会**同时清空**就地更新表与滚动表。
  - 发送时把「要发送的帧」打印到滚动区；**数据未变化**再次点击发送时仍会在滚动区打印这些帧，
    但实际只向 MCU 下发一条发送命令（TX_START），不重新打包数据帧（MCU 重发其缓存帧）。
- **参数持久化**：关闭窗口时把发送列表（勾选/ID/标准扩展/长度/帧类型/数据）、单帧编辑区、
  波特率、循环发送、周期、传输方式与端口、接收区视图与时戳选项写入 QSettings，下次启动自动
  恢复（不含窗口几何）。
