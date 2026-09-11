# vendor_dfu — 厂商 32 位 DFU 运行时（随上位机分发）

上位机是 64 位，无法同进程加载厂商 32 位 `NsMcuCmdDll.dll`，故这里放一个
32 位 CLI `nsi_dfu_flash.exe`，由 `CyberBeast-CANable2.exe` 用 QProcess 调用。

## 文件
- `nsi_dfu_flash.c` / `build.bat` — helper 源码与构建脚本（MSVC x86）
- `nsi_dfu_flash.exe` — 已构建的 32 位 helper
- `NsMcuCmdDll.dll` / `USB2UARTSPIIICDLL.dll` — 厂商 Nationstech 下载工具的 DLL

## 用法
```
nsi_dfu_flash.exe <dllDir> <bin> [startAddrHex]
```
前置：设备已进入 DFU（NationstechDFU 驱动，VID:PID 19F5:4537）。
流程：ENUM → CONNECT → GET_INF → FLASH_ERASE → FLASH_DWNLD → APP_GO。
成功打印 `RESULT: OK` 且退出码 0。协议组帧与 CRC 全部由 DLL 内部完成。

关键：`NS_Dl_File` 结构 `+0x08` 是**数据指针**（不是 CRC）；`FLASH_DWNLD` 的
`a2..a5` 传 `NULL` 即可。

## 重建
```
cd host/vendor_dfu && cmd //c build.bat     # 需 VS x86 环境（vcvars32）
```
DLL 从厂商工具目录 `NSING MCU Download Tool V1.3.7\` 拷贝。
