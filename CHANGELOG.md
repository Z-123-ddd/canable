# 更新日志 (CHANGELOG) — CyberBeast CANable 2 上位机

本仓库为 **上位机**（Qt6 Widgets + SerialPort）。固件见另一分支 `firmware-v20.0.1`
（本分支不含固件代码）。

| 构件 | 版本 | 来源 |
|---|---|---|
| 上位机 | **v10.0.1** | `host/package.sh` 的 `APPVER`；安装包见 `host/installer/canable2_host.nsi` |

---

## 上位机 v10.0.1

### 新增 / 变更 — CAN 参数区分「仲裁域 / 数据域」波特率
CAN FD 的**仲裁段**与**数据段**是两个独立速率，本次在「CAN 参数」行提供**两个独立下拉**：

- **仲裁域波特率**（默认 1 Mbit/s，USB 命令 1）
- **数据域波特率**（默认 5 Mbit/s，可选 2M/5M，USB 命令 **8**）
- 连接时两者都下发；仲裁域下拉去掉 5M（5M 仅属数据域）。
- 持久化：`can/bitrateIndex`（仲裁域）+ `can/dataBitrateIndex`（数据域）。

### 版本
- 1.1.2 → **10.0.1**（`host/package.sh`、`host/installer/canable2_host.nsi` 的 `VIProductVersion 10.0.1.0`）。

### 涉及文件
`host/src/Protocol.h`、`host/src/MainWindow.{h,cpp}`、`host/src/main.cpp`（uitest 断言）、
`host/package.sh`、`host/installer/canable2_host.nsi`、`host/README.md`。

---

## 历史

- **v1.1.2**：`AA AD` 告警帧解析对齐（CRC 位于 `DA AA` 之后，仅覆盖正文）。
- **v1.0.2**：新增其他 CAN 工具记录导入（Vector ASC / PEAK TRC / candump / 通用 CSV）。
- **v1.1.1**：DFU 改由厂商 32 位 `NsMcuCmdDll.dll` 完成；更名「CyberBeast CANable 2」；
  新增可选品牌串口驱动 `CyberBeast USB2CAN`。
