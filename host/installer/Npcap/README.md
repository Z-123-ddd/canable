# Npcap（EtherCAT 抓包驱动，随安装包内置）

上位机的 **EtherCAT 主站**（内嵌 SOEM）在 Windows 上通过 **WinPcap API** 收发原始
以太网帧，运行前需安装 **Npcap**（安装时勾选 *WinPcap API-compatible Mode*）。

本目录存放随安装包一起分发的 **Npcap 官方安装程序**：

    npcap-1.89.exe      （来自 https://npcap.com/dist/npcap-1.89.exe）
    sha256 = 8aed85e900d783d1308506e919587d3e540451947af8a82f2d04f819e44305cc

`host/package.sh` 会把本目录下的 `*.exe` 拷进 `build/deploy/Npcap/`，
再由 NSIS 安装脚本在安装过程中**调用它的图形界面**（非静默）：

- 安装到自定义页可勾选/取消该组件（默认勾选）。
- 调用参数为 `/winpcap_mode=yes`（**仅预勾选** WinPcap 兼容模式，仍显示 GUI 向导）。
- 用户按 Npcap 向导完成后，返回码 0（成功）或 3010（需重启）视为正常；
  其它返回码弹出提示但**不阻断**本程序安装。

## 未安装 Npcap 时的行为

上位机仍可正常启动（`wpcap.dll` 为运行时动态加载，见
`third_party/soem/oshw/win32/wpcap_dynload.c`），只是「模式 → EtherCAT」里
「刷新」会提示未检测到抓包驱动、网卡列表为空。

## 许可与更新

Npcap 版权归 Nmap Project 所有（非开源；免费版有使用范围限制）。
更新版本时：把新版安装包放入本目录并同步 `README`、`package.sh` 会自动取用
（本目录只保留一个 `*.exe` 时无需改脚本；NSIS 侧由 `package.sh` 以
`/DNPCAP_EXE=<文件名>` 传入，缺省名为 `npcap-1.89.exe`）。
下载：https://npcap.com/#download
