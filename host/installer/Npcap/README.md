# Npcap（EtherCAT 抓包驱动）

上位机的 **EtherCAT 主站**（内嵌 SOEM）在 Windows 上通过 **WinPcap API** 收发原始
以太网帧，运行前需安装 **Npcap**（安装时勾选 *WinPcap API-compatible Mode*）。

- 未安装时：上位机仍可正常启动（wpcap.dll 为运行时动态加载）；
  但「模式 → EtherCAT」里「刷新」会提示「未检测到 Npcap」，网卡列表为空。
- 安装 Npcap 后即可扫描网卡、打开、扫描从站与周期收发。

## 随包静默安装（可选）

将 Npcap 官方安装包重命名为 `npcap-installer.exe` 放到本目录：

    host/installer/npcap/npcap-installer.exe

则 `host/package.sh` 会把它打进安装包，NSIS 安装时以 `/S`（静默）安装。
未放置该文件时，安装包不含 Npcap，用户需自行安装。

> Npcap 版权归 Nmap Project 所有；如需商业再分发请遵循其许可。
> 下载：https://npcap.com/#download
