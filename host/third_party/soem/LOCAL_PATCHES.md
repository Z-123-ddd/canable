# vendored SOEM 本地改动（相对上游 v2.0.0）

为适配本仓库的 **MinGW-w64 (msvcrt) + Qt6** 构建环境，`third_party/soem/`
相对上游 SOEM v2.0.0 只做了一处源码改动：

- `osal/win32/osal.c`：`osal_current_time()` 原用 `timespec_get(&ts, TIME_UTC)`
  （需 UCRT 头/导入库，会引入私有 `api-ms-win-crt-private-l1-1-0.dll` 依赖），
  改为纯 Win32 `GetSystemTimeAsFileTime()` 实现，避免 UCRT 依赖。
  （因此 CMake 不再定义 `_UCRT`、不再链接 `ucrt`。）

其余文件与上游一致（`include/soem/*`、`src/*.c`、`osal/win32/*`、
`oshw/win32/wpcap/*`、`LICENSE.md`）。

**新增文件（上游没有）**：`oshw/win32/wpcap_dynload.c` —— 不链接 wpcap 导入库，
改为运行时动态加载 `wpcap.dll`（只 shim SOEM 用到的 6 个 pcap 函数）；
并按名字或 `<System32>\Npcap\<name>` 全路径兜底加载，以兼容未启用
WinPcap 兼容模式的 Npcap 安装。
生成文件 `include/soem/ec_options.h` 由 `ec_options.h.in` 按上游默认值展开。

WIN32 网络层使用 **WinPcap API**（`wpcap.dll`/`Packet.dll`）；
Windows 上推荐安装 **Npcap（WinPcap-API 兼容模式）**。
