; ============================================================================
;  CANable2-N32 上位机 安装程序脚本 (NSIS 3.x, Unicode)
;  用法（由 host/package.sh 调用）：
;    makensis /DDEPLOYDIR=..\build\deploy /DOUTFILE=..\dist\xxx.exe \
;             /DAPPVER=10.0.2 installer\canable2_host.nsi
; ============================================================================

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

; ---- 可由命令行覆盖的变量 ----
!ifndef DEPLOYDIR
  !define DEPLOYDIR "..\build\deploy"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\dist\CyberBeast-CANable2-Setup.exe"
!endif
!ifndef APPVER
  !define APPVER "10.0.2"
!endif
; 测试用可覆盖项：安装目录 / 请求权限级别
!ifndef INSTALLDIR
  !define INSTALLDIR "D:\Programs\CyberBeast-CANable2"
!endif
!ifndef REQLEVEL
  !define REQLEVEL admin
!endif

!define APPNAME    "CyberBeast CANable 2"
!define APPEXE     "CyberBeast-CANable2.exe"
!define PUBLISHER  "CyberBeast"
!define REGKEY     "Software\Microsoft\Windows\CurrentVersion\Uninstall\CyberBeast-CANable2"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "${INSTALLDIR}"
InstallDirRegKey HKLM "${REGKEY}" "InstallLocation"
RequestExecutionLevel ${REQLEVEL}
SetCompressor /SOLID lzma

VIProductVersion "10.0.2.0"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${APPNAME} 安装程序"
VIAddVersionKey "FileVersion"     "${APPVER}"
VIAddVersionKey "ProductVersion"  "${APPVER}"
VIAddVersionKey "LegalCopyright"  "MIT License"

; ---------------- 界面 ----------------
!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

; ---------------- 安装 ----------------
Section "主程序" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  ; 递归安装 deploy 目录下的全部文件（exe + Qt 运行库 + libusb dll + 插件）
  File /r "${DEPLOYDIR}\*.*"

  ; 卸载程序与注册表登记
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr   HKLM "${REGKEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr   HKLM "${REGKEY}" "DisplayVersion"  "${APPVER}"
  WriteRegStr   HKLM "${REGKEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${REGKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKLM "${REGKEY}" "DisplayIcon"     "$INSTDIR\${APPEXE}"
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${REGKEY}" "EstimatedSize" "$0"
SectionEnd

; DFU 驱动（NationstechDFU，WHQL 签名）—— 供 USB DFU 固件升级
; 驱动文件随主程序区装入 $INSTDIR\drivers\NationstechDFU，此处用 pnputil 注册到系统
Section "DFU 驱动 (NationstechDFU, USB 升级用)" SecDfuDrv
  DetailPrint "安装 DFU 驱动 (pnputil /add-driver)..."
  nsExec::ExecToLog 'pnputil /add-driver "$INSTDIR\drivers\NationstechDFU\NationstechDFU.inf" /install'
  Pop $0
  DetailPrint "pnputil 返回: $0"
SectionEnd

; 品牌串口驱动（CyberBeast USB2CAN）—— 让设备在设备管理器/端口列表里显示品牌名
; 该 INF 用自签名证书签名，故先把证书加入「受信任根 + 受信任发布者」，再 pnputil 注册
; 驱动文件随主程序区装入 $INSTDIR\drivers\CyberBeast_USB2CAN
Section "品牌串口驱动 (CyberBeast USB2CAN, 可选)" SecCdcDrv
  DetailPrint "安装品牌串口驱动证书 (certutil)..."
  nsExec::ExecToLog 'certutil -addstore -f Root "$INSTDIR\drivers\CyberBeast_USB2CAN\CyberBeast_USB2CAN.cer"'
  Pop $0
  DetailPrint "certutil Root 返回: $0"
  nsExec::ExecToLog 'certutil -addstore -f TrustedPublisher "$INSTDIR\drivers\CyberBeast_USB2CAN\CyberBeast_USB2CAN.cer"'
  Pop $0
  DetailPrint "certutil TrustedPublisher 返回: $0"
  DetailPrint "注册串口驱动 (pnputil /add-driver)..."
  nsExec::ExecToLog 'pnputil /add-driver "$INSTDIR\drivers\CyberBeast_USB2CAN\CyberBeast_USB2CAN.inf" /install'
  Pop $0
  DetailPrint "pnputil 返回: $0"
SectionEnd

Section "开始菜单快捷方式" SecStartMenu
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
  ; 手动(重新)安装 DFU 驱动的小工具（需管理员）
  FileOpen $9 "$INSTDIR\install_dfu_driver.bat" w
  FileWrite $9 'pnputil /add-driver "%~dp0drivers\NationstechDFU\NationstechDFU.inf" /install$\r$\n'
  FileWrite $9 'pause$\r$\n'
  FileClose $9
  CreateShortCut "$SMPROGRAMS\${APPNAME}\安装 DFU 驱动.lnk" "$INSTDIR\install_dfu_driver.bat"
  ; 手动(重新)安装品牌串口驱动的小工具（需管理员）
  FileOpen $9 "$INSTDIR\install_cdc_driver.bat" w
  FileWrite $9 'certutil -addstore -f Root "%~dp0drivers\CyberBeast_USB2CAN\CyberBeast_USB2CAN.cer"$\r$\n'
  FileWrite $9 'certutil -addstore -f TrustedPublisher "%~dp0drivers\CyberBeast_USB2CAN\CyberBeast_USB2CAN.cer"$\r$\n'
  FileWrite $9 'pnputil /add-driver "%~dp0drivers\CyberBeast_USB2CAN\CyberBeast_USB2CAN.inf" /install$\r$\n'
  FileWrite $9 'pause$\r$\n'
  FileClose $9
  CreateShortCut "$SMPROGRAMS\${APPNAME}\安装品牌串口驱动.lnk" "$INSTDIR\install_cdc_driver.bat"
SectionEnd

Section "桌面快捷方式" SecDesktop
  CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
SectionEnd

; ---------------- 卸载 ----------------
Section "Uninstall"
  ; 移除 DFU 驱动（尽力而为；若设备在用可能失败）
  nsExec::ExecToLog 'pnputil /delete-driver NationstechDFU.inf /uninstall /force'
  ; 移除品牌串口驱动与其自签名证书（尽力而为）
  nsExec::ExecToLog 'pnputil /delete-driver CyberBeast_USB2CAN.inf /uninstall /force'
  nsExec::ExecToLog 'certutil -delstore Root "USB\VID_16D0&PID_117E (libwdi autogenerated)"'
  nsExec::ExecToLog 'certutil -delstore TrustedPublisher "USB\VID_16D0&PID_117E (libwdi autogenerated)"'
  Delete "$INSTDIR\${APPEXE}"
  Delete "$INSTDIR\libusb-1.0.dll"
  Delete "$INSTDIR\install_dfu_driver.bat"
  Delete "$INSTDIR\install_cdc_driver.bat"
  Delete "$INSTDIR\Uninstall.exe"
  ; 删除部署目录下的其余文件（及其子目录）
  RMDir /r "$INSTDIR"

  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\安装 DFU 驱动.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\安装品牌串口驱动.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"

  DeleteRegKey HKLM "${REGKEY}"
SectionEnd

; 组件页里各区的说明文字
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDfuDrv} "安装厂商签名(WHQL)的 NationstechDFU 驱动，用于 USB DFU 固件升级。需管理员；未勾选则仅随包分发驱动文件、不注册。"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCdcDrv} "安装品牌化 USB CDC 串口驱动，使设备在设备管理器/端口列表显示为“CyberBeast USB2CAN”。纯命名、非必需（不装也能出 COM 口，只是显示系统默认名）。需管理员；会把一张自签名证书装入本机受信任存储。"
!insertmacro MUI_FUNCTION_DESCRIPTION_END
