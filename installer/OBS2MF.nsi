; OBS2MF - self-contained installer (NSIS 3.x)
; Installs the tray broker + the Media Foundation virtual-camera media source,
; registers the 64-bit COM DLL in HKLM for the Frame Server, and adds a Start-menu shortcut.

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!define PRODUCT       "OBS2MF"
!define VERSION       "0.9.2.0"
!define VERSION_TEXT  "0.9.2"
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
; Launch the tray app de-elevated: the installer runs as admin, and launching the exe
; directly would inherit that elevated token. Going through explorer.exe hands it to the
; non-elevated shell so the tray app runs as a normal user process.
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${PRODUCT} now"
!define MUI_FINISHPAGE_RUN_FUNCTION "LaunchDeElevated"
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

Function LaunchDeElevated
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${EXE}"'
FunctionEnd

Section "Install"
  SetOutPath "$INSTDIR"

  ; The media source DLL is loaded by the Windows Camera Frame Server (svchost) whenever a
  ; consumer has the camera open, which locks the file on upgrade. Stop the broker and the
  ; Frame Server services so the DLL is released; they are demand-start and resume on next use.
  nsExec::Exec 'taskkill /f /im "${EXE}"'
  nsExec::Exec 'net stop "FrameServerMonitor"'
  nsExec::Exec 'net stop "FrameServer"'
  Sleep 1500

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

  ; Create shortcuts in the ALL-USERS (common) Start Menu / Desktop. The installer runs
  ; elevated, so the per-user context would point at the elevating admin's profile and the
  ; shortcuts wouldn't appear for the logged-in user. Common locations are visible to everyone.
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\${PRODUCT}"
  CreateShortcut  "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk" "$INSTDIR\${EXE}"
  CreateShortcut  "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut  "$DESKTOP\${PRODUCT}.lnk" "$INSTDIR\${EXE}"

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
  ; stop the broker and Frame Server so the DLL is released and can be removed now
  nsExec::Exec 'taskkill /f /im "${EXE}"'
  nsExec::Exec 'net stop "FrameServerMonitor"'
  nsExec::Exec 'net stop "FrameServer"'
  Sleep 1500

  ; DllUnregisterServer removes HKLM\Software\Classes\CLSID\{our CLSID}; run while DLL present
  ${DisableX64FSRedirection}
  nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\${DLL}"'
  ${EnableX64FSRedirection}

  ; files (DLL scheduled for reboot only if something re-locked it)
  Delete /REBOOTOK "$INSTDIR\${DLL}"
  Delete "$INSTDIR\${EXE}"
  Delete "$INSTDIR\NOTICE.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  ; Start-menu + desktop shortcuts (all-users, matching install)
  SetShellVarContext all
  Delete "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT}"
  Delete "$DESKTOP\${PRODUCT}.lnk"

  ; per-user logs written by the broker
  SetShellVarContext current
  RMDir /r "$LOCALAPPDATA\OBS2MF"

  ; all registry we created
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"
  DeleteRegKey HKLM "Software\${PRODUCT}"
SectionEnd
