#!/usr/bin/env bash
# ============================================================================
#  打包上位机为 Windows 安装程序 (NSIS)
#    1) Release 构建 (CMake + Ninja + MinGW)
#    2) windeployqt 收集 Qt 运行库 + 拷贝 libusb-1.0.dll
#    3) makensis 生成安装程序 -> host/dist/CyberBeast-ProtocolTool-Setup-<ver>.exe
#  用法:  source /d/Qt/qt-tools-env.sh;  ./package.sh
# ============================================================================
set -euo pipefail

APPVER="${APPVER:-30.0.6}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

QT_PREFIX="${QT_PREFIX:-/d/Qt/6.8.3/mingw_64}"
LIBUSB_DLL="${LIBUSB_DLL:-/d/libusb/bin/libusb-1.0.dll}"
MAKENSIS="${MAKENSIS:-/d/NSIS/makensis}"

BUILD="build"
DEPLOY="$BUILD/deploy"
DIST="dist"
OUT="$DIST/CyberBeast-ProtocolTool-Setup-$APPVER.exe"
DEPLOY_ABS="$HERE/$DEPLOY"
OUT_ABS="$HERE/$OUT"

echo "==> [1/4] Release 构建"
cmake -S . -B "$BUILD" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$QT_PREFIX"
cmake --build "$BUILD"

echo "==> [2/4] 自检（协议 + GUI 烧机）"
"$BUILD/CyberBeast-ProtocolTool.exe" --selftest
if [[ "${SKIP_GUITEST:-0}" != "1" ]]; then
  "$BUILD/CyberBeast-ProtocolTool.exe" --guitest
fi

echo "==> [3/4] 收集运行库到 $DEPLOY"
rm -rf "$DEPLOY"; mkdir -p "$DEPLOY"
cp "$BUILD/CyberBeast-ProtocolTool.exe" "$DEPLOY/"
cp "$LIBUSB_DLL" "$DEPLOY/"
# 厂商 DFU 驱动（NationstechDFU，WHQL 签名）随包分发，供安装时可选安装
cp -r installer/drivers "$DEPLOY/"
# 厂商 32 位 DFU 运行时（helper + NsMcuCmdDll.dll 等）——被上位机核心烧录逻辑调用
mkdir -p "$DEPLOY/vendor_dfu"
cp -f vendor_dfu/nsi_dfu_flash.exe vendor_dfu/NsMcuCmdDll.dll vendor_dfu/USB2UARTSPIIICDLL.dll "$DEPLOY/vendor_dfu/"
# Npcap 抓包驱动（EtherCAT 用）：随包内置官方安装程序，安装时调用其图形界面
NPCAP_EXE=""
if [ -d installer/Npcap ]; then
  for f in installer/Npcap/*.exe; do
    [ -e "$f" ] || continue
    mkdir -p "$DEPLOY/Npcap"
    cp -f "$f" "$DEPLOY/Npcap/"
    NPCAP_EXE="$(basename "$f")"
    break
  done
fi
echo "    内置 Npcap 安装程序: ${NPCAP_EXE:-（无）}"
windeployqt --release --no-translations --compiler-runtime \
            --no-system-d3d-compiler --no-opengl-sw \
            "$DEPLOY/CyberBeast-ProtocolTool.exe"

echo "==> [4/4] 生成安装程序 $OUT"
mkdir -p "$DIST"
# MSYS/git-bash 会把 /D... 误当路径改写，禁用参数转换
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' \
"$MAKENSIS" -V3 \
  "/DDEPLOYDIR=$(cygpath -w "$DEPLOY_ABS")" \
  "/DOUTFILE=$(cygpath -w "$OUT_ABS")" \
  "/DAPPVER=$APPVER" \
  "/DNPCAP_EXE=$NPCAP_EXE" \
  "$(cygpath -w "$HERE/installer/canable2_host.nsi")"

# 同步便携目录（gitignore）并重打 zip（git 跟踪）
PORTABLE_DIR="$DIST/CyberBeast-ProtocolTool-$APPVER-portable"
PORTABLE_ZIP="$DIST/CyberBeast-ProtocolTool-$APPVER-portable.zip"
rm -rf "$PORTABLE_DIR"
mkdir -p "$PORTABLE_DIR"
cp -a "$DEPLOY/." "$PORTABLE_DIR/"
rm -f "$PORTABLE_ZIP"
# zip 在 MSYS 下可用；失败则提示手动打包
if command -v zip >/dev/null 2>&1; then
  (cd "$DIST" && zip -r -q "$(basename "$PORTABLE_ZIP")" "$(basename "$PORTABLE_DIR")")
elif command -v powershell.exe >/dev/null 2>&1; then
  powershell.exe -NoProfile -Command \
    "Compress-Archive -Path '$(cygpath -w "$PORTABLE_DIR")' -DestinationPath '$(cygpath -w "$PORTABLE_ZIP")' -Force"
fi

echo "==> 完成: $OUT"
ls -lh "$OUT" || true
ls -lh "$PORTABLE_ZIP" 2>/dev/null || true
