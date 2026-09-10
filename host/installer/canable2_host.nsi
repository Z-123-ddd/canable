; ============================================================================
;  CANable2-N32 上位机 安装程序脚本 (NSIS 3.x, Unicode)
;  用法（由 host/package.sh 调用）：
;    makensis /DDEPLOYDIR=..\build\deploy /DOUTFILE=..\dist\xxx.exe \
;             /DAPPVER=1.0.0 installer\canable2_host.nsi
; ============================================================================

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

; ---- 可由命令行覆盖的变量 ----
!ifndef DEPLOYDIR
  !define DEPLOYDIR "..\build\deploy"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\dist\CANable2-Host-Setup.exe"
!endif
!ifndef APPVER
  !define APPVER "1.0.0"
!endif
; 测试用可覆盖项：安装目录 / 请求权限级别
!ifndef INSTALLDIR
  !define INSTALLDIR "D:\Programs\CANable2-Host"
!endif
!ifndef REQLEVEL
  !define REQLEVEL admin
!endif

!define APPNAME    "CANable2-N32 上位机"
!define APPEXE     "canable2_host.exe"
!define PUBLISHER  "CyberBeast"
!define REGKEY     "Software\Microsoft\Windows\CurrentVersion\Uninstall\CANable2Host"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "${INSTALLDIR}"
InstallDirRegKey HKLM "${REGKEY}" "InstallLocation"
RequestExecutionLevel ${REQLEVEL}
SetCompressor /SOLID lzma

VIProductVersion "1.0.0.0"
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

Section "开始菜单快捷方式" SecStartMenu
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "桌面快捷方式" SecDesktop
  CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
SectionEnd

; ---------------- 卸载 ----------------
Section "Uninstall"
  Delete "$INSTDIR\${APPEXE}"
  Delete "$INSTDIR\libusb-1.0.dll"
  Delete "$INSTDIR\Uninstall.exe"
  ; 删除部署目录下的其余文件（及其子目录）
  RMDir /r "$INSTDIR"

  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"

  DeleteRegKey HKLM "${REGKEY}"
SectionEnd
