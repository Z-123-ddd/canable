#!/usr/bin/env bash
# ============================================================================
#  打包上位机为 Windows 安装程序 (NSIS)
#    1) Release 构建 (CMake + Ninja + MinGW)
#    2) windeployqt 收集 Qt 运行库 + 拷贝 libusb-1.0.dll
#    3) makensis 生成安装程序 -> host/dist/CyberBeast-CANable2-Setup-<ver>.exe
#  用法:  source /d/Qt/qt-tools-env.sh;  ./package.sh
# ============================================================================
set -euo pipefail

APPVER="${APPVER:-1.1.2}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

QT_PREFIX="${QT_PREFIX:-/d/Qt/6.8.3/mingw_64}"
LIBUSB_DLL="${LIBUSB_DLL:-/d/libusb/bin/libusb-1.0.dll}"
MAKENSIS="${MAKENSIS:-/d/NSIS/makensis}"

BUILD="build"
DEPLOY="$BUILD/deploy"
DIST="dist"
OUT="$DIST/CyberBeast-CANable2-Setup-$APPVER.exe"
DEPLOY_ABS="$HERE/$DEPLOY"
OUT_ABS="$HERE/$OUT"

echo "==> [1/3] Release 构建"
cmake -S . -B "$BUILD" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$QT_PREFIX"
cmake --build "$BUILD"

echo "==> [2/3] 收集运行库到 $DEPLOY"
rm -rf "$DEPLOY"; mkdir -p "$DEPLOY"
cp "$BUILD/CyberBeast-CANable2.exe" "$DEPLOY/"
cp "$LIBUSB_DLL" "$DEPLOY/"
# 厂商 DFU 驱动（NationstechDFU，WHQL 签名）随包分发，供安装时可选安装
cp -r installer/drivers "$DEPLOY/"
# 厂商 32 位 DFU 运行时（helper + NsMcuCmdDll.dll 等）——被上位机核心烧录逻辑调用
mkdir -p "$DEPLOY/vendor_dfu"
cp -f vendor_dfu/nsi_dfu_flash.exe vendor_dfu/NsMcuCmdDll.dll vendor_dfu/USB2UARTSPIIICDLL.dll "$DEPLOY/vendor_dfu/"
windeployqt --release --no-translations --compiler-runtime \
            --no-system-d3d-compiler --no-opengl-sw \
            "$DEPLOY/CyberBeast-CANable2.exe"

echo "==> [3/3] 生成安装程序 $OUT"
mkdir -p "$DIST"
# MSYS/git-bash 会把 /D... 误当路径改写，禁用参数转换
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' \
"$MAKENSIS" -V3 \
  "/DDEPLOYDIR=$(cygpath -w "$DEPLOY_ABS")" \
  "/DOUTFILE=$(cygpath -w "$OUT_ABS")" \
  "/DAPPVER=$APPVER" \
  "$(cygpath -w "$HERE/installer/canable2_host.nsi")"

echo "==> 完成: $OUT"
ls -lh "$OUT"
