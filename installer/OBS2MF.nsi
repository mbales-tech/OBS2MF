; OBS2MF - self-contained installer (NSIS 3.x)
; Installs the tray broker + the Media Foundation virtual-camera media source,
; registers the 64-bit COM DLL in HKLM for the Frame Server, and adds a Start-menu shortcut.

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!define PRODUCT       "OBS2MF"
!define VERSION       "0.1.0.0"
!define VERSION_TEXT  "0.1.0"
!define PUBLISHER     "OBS2MF"
!define DLL           "Vcam.MediaSource.dll"
!define EXE           "Vcam.Broker.exe"
!define SRC           "${__FILEDIR__}\..\x64\Release"

Name "${PRODUCT} ${VERSION_TEXT}"
OutFile "${__FILEDIR__}\OBS2MF-Setup-${VERSION_TEXT}.exe"
InstallDir "$PROGRAMFILES64\${PRODUCT}"
InstallDirRegKey HKLM "Software\${PRODUCT}" "InstallDir"
RequestExecutionLevel admin
ShowInstDetails show
ShowUnInstDetails show

VIProductVersion "${VERSION}"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Portions derived from smourier/VCamSample (MIT)."
VIAddVersionKey "FileDescription" "${PRODUCT} Installer"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_LICENSE "${__FILEDIR__}\..\NOTICE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${PRODUCT} now"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT} requires 64-bit Windows 11."
    Abort
  ${EndIf}
FunctionEnd

Section "Install"
  SetOutPath "$INSTDIR"

  ; stop a running broker so the exe/dll can be replaced
  nsExec::Exec 'taskkill /f /im "${EXE}"'
  ; unregister any previous copy of the DLL (ignore errors)
  ${DisableX64FSRedirection}
  nsExec::Exec '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\${DLL}"'
  ${EnableX64FSRedirection}
  Sleep 500

  File "${SRC}\${EXE}"
  File "${SRC}\${DLL}"
  File "${__FILEDIR__}\..\NOTICE.txt"

  ; register the 64-bit media source COM DLL in HKLM (Frame Server loads it)
  ${DisableX64FSRedirection}
  nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\${DLL}"'
  Pop $0
  ${EnableX64FSRedirection}
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "regsvr32 returned $0. The virtual camera may not work until the DLL is registered."
  ${EndIf}

  CreateDirectory "$SMPROGRAMS\${PRODUCT}"
  CreateShortcut  "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk" "$INSTDIR\${EXE}"
  CreateShortcut  "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "Software\${PRODUCT}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}" "DisplayName" "${PRODUCT} ${VERSION_TEXT}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}" "DisplayVersion" "${VERSION_TEXT}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}" "NoRepair" 1

  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /f /im "${EXE}"'
  ${DisableX64FSRedirection}
  nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\${DLL}"'
  ${EnableX64FSRedirection}
  Sleep 500

  ; DLL may still be loaded in the Frame Server; delete on reboot if needed
  Delete /REBOOTOK "$INSTDIR\${DLL}"
  Delete "$INSTDIR\${EXE}"
  Delete "$INSTDIR\NOTICE.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT}"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"
  DeleteRegKey HKLM "Software\${PRODUCT}"
SectionEnd
