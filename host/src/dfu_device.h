// ============================================================================
//  dfu_device — N32 出厂 DFU bootloader 检测与烧录（libusb）
//
//  流程（DfuSe 风格，.bin 烧到目标地址）：
//    打开设备 → 声明接口 → 设定地址(0x21) → 擦除(0x41) → 分块下载 → 离开(重启)
//
//  设备匹配：
//    · 若 kDfuVid/kDfuPid 非 0 → 按 VID/PID 匹配
//    · 否则按 USB DFU 类描述符匹配（bInterfaceClass=0xFE, SubClass=0x01,
//      Protocol=0x02 = DFU mode）
//
//  注意：N32 出厂 bootloader 的具体 VID/PID 需实测后填入 kDfuVid/kDfuPid；
//        未填(0)时按 DFU 类自动匹配。协议按常见 DfuSe 约定实现。
// ============================================================================
#ifndef DFU_DEVICE_H
#define DFU_DEVICE_H

#include <QString>
#include <functional>
#include <cstdint>

namespace dfu {

// DFU 设备 VID/PID（0 = 按 USB DFU 类匹配）。实测后填入。
constexpr uint16_t kDfuVid = 0x0000;
constexpr uint16_t kDfuPid = 0x0000;

struct Info {
    bool found = false;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint8_t  bus = 0;
    uint8_t  address = 0;
    uint8_t  interface = 0;      // DFU 接口号（wIndex）
    QString  product;            // iProduct 字符串（可选）
};

// 枚举 USB，返回匹配到的 DFU 设备信息（未找到则 found=false）
Info findDevice();

// 枚举所有 USB 设备，返回可读清单（用于读出 DFU 设备的 VID/PID）
QString scanDevices();

// 把 .bin 烧录到 addr（默认 0x08000000）。
// progress(percent, stage) 会周期性回调（可能在工作线程中）。
// 返回 true 成功；失败时 err 写原因。
bool flashBin(const QString &binPath, quint32 addr, QString *err,
              const std::function<void(int, const QString &)> &progress = nullptr);

} // namespace dfu

#endif // DFU_DEVICE_H
