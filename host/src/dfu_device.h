// ============================================================================
//  dfu_device — N32 出厂 DFU bootloader 检测与回退烧录（libusb, DfuSe 风格）
//
//  ⚠ 主路径不是这里。实际烧录由厂商 32 位 NsMcuCmdDll.dll 完成（见
//    src/VendorDfuFlasher.cpp + vendor_dfu/）。本文件仅保留 libusb 自实现作**回退**，
//    它要求 DFU 设备绑定 WinUSB/libusb 驱动，与厂商 NationstechDFU 驱动**互斥**，
//    正常装机场景下不会走到。
//
//  事实依据（见 docs/FIRMWARE_PROTOCOL.md §9 与官方资料）：
//    · N32 出厂 bootloader 在 USB-FS 上以 **DFU 模式** 枚举，
//      VID/PID = **0x19F5:0x4537**（设备名 "Nationstech DFU"）——不是 ST 的 0x0483:0xDF11。
//    · 若走 libusb 回退，走 DfuSe 扩展协议：
//      DNLOAD(0x01)+CLRSTATUS(0x04)+GETSTATUS(0x03)+ABORT(0x06)。
//      特殊命令（block0）：0x21+addr(LE4)=设定地址，0x41+addr(LE4)=擦除该扇区，
//      block0 空数据=离开并运行应用。数据块从 block=2 起，块大小 = gcd(扇区, 2048)。
//    · 内存布局（扇区表）从 DFU 接口的 alt-setting 字符串解析，形如
//        "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,03*128Kg"
//
//  Windows 注意：libusb 回退路径需 DFU 设备绑定 WinUSB/libusb 驱动（Zadig 安装），
//  但**不要**对 NationstechDFU 设备这么做——那会让厂商烧录 DLL 打不开它。
// ============================================================================
#ifndef DFU_DEVICE_H
#define DFU_DEVICE_H

#include <QMutex>
#include <QString>
#include <QList>
#include <functional>
#include <cstdint>

namespace dfu {

// N32 出厂 DFU bootloader 的 VID/PID（**本板实测**：设备名为 "Nationstech DFU"）。
// 注意：不是 ST 的 0x0483:0xDF11 —— N32 用它自己的 0x19F5:0x4537。
// 进程内所有 libusb 入口（UsbLink 的收发/关闭、DFU 探测）都必须持有的锁。
//
// libusb 的上下文/事件处理不是线程安全的：一边有异步 URB 在泵事件，另一边
// 又对同一份全局状态做 init/exit，会在 libusb-1.0.dll 内部触发访问违例
// （0xc0000005）。点「更新固件」时设备进 DFU、连接被关闭，紧接着每秒三次
// 的 DFU 探测正好与接收线程的重叠 —— 这就是闪退的根因。
// 定义在 dfu_device.cpp，UsbLink 与 DFU 探测共用同一把。
QMutex &libusbGuard();

constexpr uint16_t kDfuVid = 0x19F5;
constexpr uint16_t kDfuPid = 0x4537;

struct Info {
    bool found = false;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint8_t  bus = 0;
    uint8_t  address = 0;
    uint8_t  interface = 0;      // DFU 接口号（wIndex）
    uint8_t  altSetting = 0;     // Internal Flash 对应的 alt setting
    QString  product;            // iProduct 字符串（可选）
};

struct Sector {
    quint32 addr = 0;            // 扇区起始地址
    quint32 len = 0;             // 扇区长度（字节）
};

// 枚举 USB，返回匹配到的 DFU 设备信息（未找到则 found=false）
Info findDevice();

// 枚举所有 USB 设备，返回可读清单（用于读出 DFU 设备的 VID/PID）
QString scanDevices();

// Dump 目标 DFU 设备的接口/alt-setting/字符串（诊断用）
QString describeDevice();

// DFU 协议探针：dump 原始 GETSTATE/GETSTATUS 等响应
QString probeDfu();

// 把 .bin 烧录到 addr（默认 0x08000000）。
// progress(percent, stage) 会周期性回调（可能在工作线程中）。
// 返回 true 成功；失败时 err 写原因。
bool flashBin(const QString &binPath, quint32 addr, QString *err,
              const std::function<void(int, const QString &)> &progress = nullptr);

// ---------------------------------------------------------------------------
// 纯函数（无硬件依赖，供 --dfu-selftest 离线校验）
// ---------------------------------------------------------------------------
// DfuSe 特殊命令帧：设定地址 = [0x21, addr(LE4)]
QByteArray encodeSetAddress(quint32 addr);
// DfuSe 特殊命令帧：擦除扇区 = [0x41, addr(LE4)]
QByteArray encodeErase(quint32 addr);
// 解析 DFU alt-setting 内存布局串，得到 baseaddr 与扇区表。
// 形如 "@Internal Flash  /0x08000000/04*016Kg,01*064Kg,03*128Kg"；
// 解析失败返回空表。altName 命中 "Internal Flash" 才认为有效。
QList<Sector> parseLayout(const QString &altName, quint32 *baseaddr);
// 数据块大小 = gcd(扇区长度, 2048)
quint32 transferSize(quint32 sectorLen);
// 返回覆盖 [addr, addr+len) 的扇区（按地址升序）
QList<Sector> sectorsCovering(const QList<Sector> &sectors, quint32 addr, quint32 len);

} // namespace dfu

#endif // DFU_DEVICE_H
