; ============================================================================
;  CANable2-N32 上位机 安装程序脚本 (NSIS 3.x, Unicode)
;  用法（由 host/package.sh 调用）：
;    makensis /DDEPLOYDIR=..\build\deploy /DOUTFILE=..\dist\xxx.exe \
;             /DAPPVER=30.0.4 installer\canable2_host.nsi
;
;  若检测到已安装版本，自定义页可选：修改 / 修复 / 覆盖
; ============================================================================

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "WinMessages.nsh"

; ---- 可由命令行覆盖的变量 ----
!ifndef DEPLOYDIR
  !define DEPLOYDIR "..\build\deploy"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\dist\CyberBeast-ProtocolTool-Setup.exe"
!endif
!ifndef APPVER
  !define APPVER "30.0.4"
!endif
; 随包内置的 Npcap 安装程序文件名（由 package.sh 用 /DNPCAP_EXE 传入）
!ifndef NPCAP_EXE
  !define NPCAP_EXE "npcap-1.89.exe"
!endif
; 测试用可覆盖项：安装目录 / 请求权限级别
!ifndef INSTALLDIR
  !define INSTALLDIR "D:\Programs\CyberBeast-ProtocolTool"
!endif
!ifndef REQLEVEL
  !define REQLEVEL admin
!endif

!define APPNAME    "CyberBeast 协议调试工具"
!define APPEXE     "CyberBeast-ProtocolTool.exe"
!define PUBLISHER  "CyberBeast"
!define REGKEY     "Software\Microsoft\Windows\CurrentVersion\Uninstall\CyberBeast-ProtocolTool"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "${INSTALLDIR}"
InstallDirRegKey HKLM "${REGKEY}" "InstallLocation"
RequestExecutionLevel ${REQLEVEL}
SetCompressor /SOLID lzma

VIProductVersion "30.0.4.0"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${APPNAME} 安装程序"
VIAddVersionKey "FileVersion"     "${APPVER}"
VIAddVersionKey "ProductVersion"  "${APPVER}"
VIAddVersionKey "LegalCopyright"  "MIT License"

; 0=全新 1=修改(组件) 2=修复 3=覆盖
Var ReinstallMode
Var ExistingVer
Var ExistingDir
Var RadioModify
Var RadioRepair
Var RadioOverwrite

; ---------------- 界面 ----------------
!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!insertmacro MUI_PAGE_WELCOME
Page custom ReinstallPageCreate ReinstallPageLeave
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

; ---------------- 已有版本检测 ----------------
Function .onInit
  StrCpy $ReinstallMode "0"
  StrCpy $ExistingVer ""
  StrCpy $ExistingDir ""

  ReadRegStr $ExistingVer HKLM "${REGKEY}" "DisplayVersion"
  ReadRegStr $ExistingDir HKLM "${REGKEY}" "InstallLocation"
  ${If} $ExistingVer == ""
    ReadRegStr $ExistingVer HKCU "${REGKEY}" "DisplayVersion"
    ReadRegStr $ExistingDir HKCU "${REGKEY}" "InstallLocation"
  ${EndIf}

  ${If} $ExistingDir != ""
    StrCpy $INSTDIR $ExistingDir
  ${EndIf}

  ; 安装前尽量结束正在运行的上位机，避免文件占用
  nsExec::ExecToLog 'taskkill /IM "${APPEXE}" /F'
  Pop $0
FunctionEnd

Function ReinstallPageCreate
  ${If} $ExistingVer == ""
    Abort   ; 无已装版本则跳过本页
  ${EndIf}

  !insertmacro MUI_HEADER_TEXT "检测到已安装版本" "请选择修改、修复或覆盖安装方式"
  nsDialogs::Create 1018
  Pop $0

  ${NSD_CreateLabel} 0 0 100% 36u "已安装 ${APPNAME}（版本 $ExistingVer），安装目录：$\r$\n$INSTDIR$\r$\n请选择本次安装方式："
  Pop $0

  ${NSD_CreateRadioButton} 10 50u 100% 14u "修改 — 更改安装组件（驱动 / 快捷方式等），保留现有程序文件后按勾选更新"
  Pop $RadioModify
  ${NSD_CreateRadioButton} 10 72u 100% 14u "修复 — 重新安装程序文件与已选组件（覆盖损坏文件，目录不变）"
  Pop $RadioRepair
  ${NSD_CreateRadioButton} 10 94u 100% 14u "覆盖 — 升级/覆盖为当前安装包版本 ${APPVER}"
  Pop $RadioOverwrite

  ${NSD_Check} $RadioOverwrite
  nsDialogs::Show
FunctionEnd

Function ReinstallPageLeave
  ${If} $ExistingVer == ""
    Return
  ${EndIf}
  ${NSD_GetState} $RadioModify $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $ReinstallMode "1"
    Return
  ${EndIf}
  ${NSD_GetState} $RadioRepair $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $ReinstallMode "2"
    Return
  ${EndIf}
  StrCpy $ReinstallMode "3"
FunctionEnd

; ---------------- 安装 ----------------
Section "主程序" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  DetailPrint "安装模式 ReinstallMode=$ReinstallMode（0新装/1修改/2修复/3覆盖）"

  ; 修改/修复/覆盖/新装均写入当前 deploy 文件（修改时也更新主程序，保证与勾选组件一致）
  File /r "${DEPLOYDIR}\*.*"

  ; 卸载程序与注册表登记（允许“修改/修复”入口）
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr   HKLM "${REGKEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr   HKLM "${REGKEY}" "DisplayVersion"  "${APPVER}"
  WriteRegStr   HKLM "${REGKEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${REGKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKLM "${REGKEY}" "DisplayIcon"     "$INSTDIR\${APPEXE}"
  WriteRegStr   HKLM "${REGKEY}" "ModifyPath"      '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 0
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 0
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${REGKEY}" "EstimatedSize" "$0"
SectionEnd

; DFU 驱动（NationstechDFU，WHQL 签名）—— 供 USB DFU 固件升级
Section "DFU 驱动 (NationstechDFU, USB 升级用)" SecDfuDrv
  DetailPrint "安装 DFU 驱动 (pnputil /add-driver)..."
  nsExec::ExecToLog 'pnputil /add-driver "$INSTDIR\drivers\NationstechDFU\NationstechDFU.inf" /install'
  Pop $0
  DetailPrint "pnputil 返回: $0"
SectionEnd

; 品牌串口驱动（CyberBeast USB2CAN）
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


; Npcap 抓包驱动（EtherCAT 主站运行所需；WinPcap-API 兼容）——随包内置
; 安装时调用 Npcap 官方安装程序的图形界面（非静默），由用户按向导完成。
; 若包内未附带安装程序（$INSTDIR\Npcap\${NPCAP_EXE} 不存在）则跳过。
Section "Npcap 抓包驱动 (EtherCAT 用, 可选)" SecNpcap
  IfFileExists "$INSTDIR\Npcap\${NPCAP_EXE}" 0 npcap_skip
    DetailPrint "启动 Npcap 安装程序（图形界面，请按向导完成）..."
    ; /winpcap_mode=yes 预勾选 “WinPcap API-compatible Mode”（仍显示 GUI）
    ExecWait '"$INSTDIR\Npcap\${NPCAP_EXE}" /winpcap_mode=yes' $0
    DetailPrint "Npcap 安装程序返回码: $0"
    ; 0=成功，3010=成功但需重启；其它视为未完成，仅提示不阻断安装
    ${If} $0 != 0
    ${AndIf} $0 != 3010
      MessageBox MB_OK|MB_ICONEXCLAMATION "Npcap 未安装或未完成（返回码 $0）。\r\nEtherCAT 模式需要 Npcap 才能收发网卡原始帧；可稍后手动运行：\r\n$INSTDIR\Npcap\${NPCAP_EXE}"
    ${EndIf}
  npcap_skip:
SectionEnd

Section "开始菜单快捷方式" SecStartMenu
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
  FileOpen $9 "$INSTDIR\install_dfu_driver.bat" w
  FileWrite $9 'pnputil /add-driver "%~dp0drivers\NationstechDFU\NationstechDFU.inf" /install$\r$\n'
  FileWrite $9 'pause$\r$\n'
  FileClose $9
  CreateShortCut "$SMPROGRAMS\${APPNAME}\安装 DFU 驱动.lnk" "$INSTDIR\install_dfu_driver.bat"
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
  nsExec::ExecToLog 'taskkill /IM "${APPEXE}" /F'
  Pop $0
  nsExec::ExecToLog 'pnputil /delete-driver NationstechDFU.inf /uninstall /force'
  nsExec::ExecToLog 'pnputil /delete-driver CyberBeast_USB2CAN.inf /uninstall /force'
  nsExec::ExecToLog 'certutil -delstore Root "USB\VID_16D0&PID_117E (libwdi autogenerated)"'
  nsExec::ExecToLog 'certutil -delstore TrustedPublisher "USB\VID_16D0&PID_117E (libwdi autogenerated)"'
  Delete "$INSTDIR\${APPEXE}"
  Delete "$INSTDIR\libusb-1.0.dll"
  Delete "$INSTDIR\install_dfu_driver.bat"
  Delete "$INSTDIR\install_cdc_driver.bat"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"

  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\安装 DFU 驱动.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\安装品牌串口驱动.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"

  DeleteRegKey HKLM "${REGKEY}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDfuDrv} "安装厂商签名(WHQL)的 NationstechDFU 驱动，用于 USB DFU 固件升级。需管理员；未勾选则仅随包分发驱动文件、不注册。"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCdcDrv} "安装品牌化 USB CDC 串口驱动，使设备在设备管理器/端口列表显示为“CyberBeast USB2CAN”。纯命名、非必需（不装也能出 COM 口，只是显示系统默认名）。需管理员；会把一张自签名证书装入本机受信任存储。"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecNpcap} "内置 Npcap 官方安装程序，安装时弹出其图形界面向导（非静默）。EtherCAT 主站收发网卡原始帧需要 Npcap（WinPcap 兼容模式）。需管理员；可取消勾选、稍后自行安装。"
!insertmacro MUI_FUNCTION_DESCRIPTION_END
