// ============================================================================
//  VendorDfuFlasher — 用厂商 32 位 NsMcuCmdDll.dll 做 DFU 烧录
//
//  为什么用 helper 子进程：厂商 NsMcuCmdDll.dll 是 32 位，而本上位机是 64 位，
//  无法同进程加载。故随包分发一个 32 位 CLI `vendor_dfu/nsi_dfu_flash.exe`
//  （它负责 ENUM/CONNECT/GET_INF/FLASH_ERASE/FLASH_DWNLD/APP_GO，DLL 内部完成
//   协议组帧与校验——无需上位机破解），本类用 QProcess 调用它。
//
//  前置：设备已在 DFU 模式（NationstechDFU 驱动），由 MainWindow「更新固件」
//  先经当前连接发 ENTER_DFU 完成。
// ============================================================================
#ifndef VENDORDFUFLASHER_H
#define VENDORDFUFLASHER_H

#include <QString>
#include <functional>

namespace vendorDfu {

// 运行时组件目录（helper exe + 两个 DLL）相对可执行文件的路径
constexpr const char *kRbDir = "vendor_dfu";

// 是否已随包提供 vendor_dfu 运行时（nsi_dfu_flash.exe 存在）
bool available();

// 用厂商 DLL 把 bin 烧到 addr（默认 0x08000000），成功后跳转运行。
// progress(percent, stage) 周期回调；失败时 err 写原因。
bool flash(const QString &binPath, quint32 addr, QString *err,
           const std::function<void(int, const QString &)> &progress = nullptr);

} // namespace vendorDfu

#endif // VENDORDFUFLASHER_H
