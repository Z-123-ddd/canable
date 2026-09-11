# N32H473 出厂 DFU 烧录协议（逆向自厂商工具运行时）

> 来源：frida 挂钩 `NsMcuCmdDll.dll` + `DeviceIoControl`，在厂商 `NZDownloadTool.exe`
> 真实烧录 `canable2.bin` 时**逐调用录制**得到。用于在我方上位机内复现（不依赖厂商 GUI）。

## 1. 传输层

设备：`VID 0x19F5 / PID 0x4537`（"Nationstech DFU"，标准 USB DFU 类 0xFE/0x01/0x02）。
需**厂商驱动 `NationstechDFU.sys`**（WHQL 签名，随安装包分发）。

DLL 通过 `SetupAPI`(`SetupDiGetClassDevs/EnumDeviceInterfaces/GetDeviceInterfaceDetailW`) 定位设备接口
（类 GUID `{88bae032-...}`，接口 GUID `{2bc34ecc-...}`），`CreateFile` 打开后用**私有 IOCTL `0x222000`** 收发。

IOCTL 输入 = **8 字节头 + Nations 命令帧**：
- 发命令：`21 01 00 00 00 00 <len:u16LE> 00` + 帧
- 取应答：`a1 03 00 00 <cmdH> <cmdL> <caplo> <cahi>`
- 帧 = `CMD_H + CMD_L + LEN(u16LE) + Par(u32LE) + DAT`
- 应答 = `CMD_H + CMD_L + LEN(u16) + DAT + CR1 + CR2`，**CR1=0xA0 成功 / 0xB0 失败**
  （CR1 在 `resp[4 + respLEN]`）

> 等价：IOCTL 头就是 DFU 的 `bmRequestType/bRequest`（`21 01`=DNLOAD、`a1 03`=GETSTATUS），
> 故也可用 **libusb 标准 DFU** 直接收发（`DFU_DNLOAD` / `DFU_GETSTATUS`，wIndex = cmdH|cmdL<<8）。

## 2. 命令码（Nations BOOT）

| cmd | 名称 | Par / DAT |
|----|------|-----------|
| 0x10 | GET_INF | 读芯片信息，应答 51B DAT（型号索引/BOOT版本/UCID/UID） |
| 0x20 | KEY_RNG | 取 16B 随机数 |
| 0x30 | FLASH_ERASE | Par = 页号(0..63) \| 页数(1..64)<<16；页=8KB(0x2000) |
| 0x31 | FLASH_DWNLD | Par = 起始地址；DAT = 16B认证 + 数据 + 4B尾 |
| 0x41 | USERX_OP | 读/配分区（CMD_L=0 读、1 配） |
| 0x51 | APP_GO | 跳转运行 |

## 3. 高层 API 调用序列（DLL 导出，x64 由 MotorWizard 版提供）

```
NS_CMD_ENUM_DEV(type=1 USB, &nameOut)          -> 1 ; nameOut = "NationstechDFU Device 0"
NS_CMD_CONNECT_DEV(name, ?, 0)                 -> True
NS_CMD_GET_INF(...) / USERX_OP / OPT_RW        (读芯片信息、分区、选项)
NS_CMD_FLASH_ERASE(handle, startAddr, endAddr, ctx)  -> 0xA000  // 注意：传地址范围而非页号
NS_CMD_FLASH_DWNLD(handle, NS_Dl_File*, ctx, ctx2)   -> 0xA000
NS_CMD_APP_GO(handle, 0, 0, ctx)               -> 0xA000  // 运行新固件
```

**NS_Dl_File 结构**（实测抓到，32 位布局）：
```
+0x00 : u32  保留(0)
+0x04 : u32  下载起始地址 (如 0x08000000)
+0x08 : u32  文件 CRC (非标准算法；见 §5)
+0x0C : u32  数据长度 (16 对齐；如 20296 -> 20304)
+0x10 : u32  保留(0)
+0x14 : ptr  数据缓冲区
```

## 4. FLASH_DWNLD 的实际 IOCTL 帧（逐字节抓到）

```
IOCTL 0x222000 输入(164B)：
 21 01 00 00 00 00 9c 00 | 31 00 94 00 <addr:u32LE> <DAT:148>
其中 DAT = [16B 认证=0x00*16] [128B 数据] [4B 尾]
```
- 按 **128 字节**一块，`addr` 每块 +0x80（0x08000000, 0x08000080, …）
- 例 block0: DAT 数据段 = `bin[0:128]`（以 `78 57 00 20` 开头 = 向量表 SP 0x20005778），尾 4B = `f3 9f b8 aa`

## 5. 未解：尾部 4 字节 / 文件 CRC 算法

- 每块尾 4B（如 block0=`0xAAB89FF3`）与 `NS_Dl_File.crc`（`0x091585d8`）都是**非标准 CRC32**
  —— 已穷举 zlib/CRC32/BZIP2/MPEG-2/Castagnoli、反射/非反射、init 0/0xFFFFFFFF、xorout 0/0xFFFFFFFF、
  多种输入范围（data、auth+data、addr+auth+data…），**均不匹配**。
- **因此纯自实现（IOCTL/libusb 手拼帧）会因该 4B 不对而被设备拒（CR1=0xB0）**。
- **正确做法：用厂商 `NsMcuCmdDll.dll`** —— 高层 `NS_CMD_FLASH_DWNLD` 内部负责该编码。

## 6. 两条可行集成路径

1. **依赖 DLL（推荐，符合"依赖 dll 文件"）**：上位机随包 `NsMcuCmdDll.dll`(x64) + 厂商驱动；
   用 `QLibrary` 加载并调 §3 的 `NS_CMD_*` 序列。DLL 处理帧/CRC。
   注意：本机用 ctypes 直调 x64 DLL 时 `GET_INF/ERASE` 返回 `0xE000`（DLL 走 overlapped IO，
   IOCTL 返回 pending），**x64 ABI 仍需一次实机标定**（建议 hook 一个 x64 刷机过程对齐参数）。
2. **自实现 IOCTL/libusb**：`GET_INF/ERASE/KEY_RNG/USERX_OP` 已 100% 打通；**仅差 §5 的 4B 尾算法**。

## 7. 已验证可用（本次实测）

- 厂商 32 位 `NsMcuCmdDll.dll` 路径（现上位机采用）：见 `host/vendor_dfu/` 与
  `host/src/VendorDfuFlasher.cpp`，**成功烧录并运行**。
- 固件 ENTER_DFU（命令 5）经串口触发 → 设备进 DFU，实测有效。

## 8. 设备与驱动切换备忘

- 换厂商驱动：`pnputil /add-driver <...>\NationstechDFU.inf /install`
- 换 libusb0：`C:\Users\ASUS\usb_driver\Nationstech_DFU.inf`（libwdi 签名包）
- 两驱动竞争时：`pnputil /delete-driver <oem>.inf /uninstall /force` 再 `/scan-devices`
