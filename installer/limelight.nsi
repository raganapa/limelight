; Limelight installer (per user, no administrator rights needed).
; Build from the repository root:  makensis -NOCD installer/limelight.nsi

Unicode true
Target amd64-unicode
!include "MUI2.nsh"

!define APPNAME "Limelight"
!define VERSION "1.0.3"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Limelight"
!define RUNKEY "Software\Microsoft\Windows\CurrentVersion\Run"

Name "${APPNAME}"
OutFile "dist\Limelight-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\Limelight"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "Limelight ${VERSION}"

VIProductVersion "1.0.3.0"
VIAddVersionKey "ProductName" "Limelight"
VIAddVersionKey "FileDescription" "Limelight setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT License"

!define MUI_ICON "res\limelight.ico"
!define MUI_UNICON "res\limelight.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install Limelight"
!define MUI_WELCOMEPAGE_TEXT "Smooth spotlight, magnifier, laser and drawing effects for the Logitech Spotlight 2 - without Logi Options+.$\r$\n$\r$\nIf Logi Options+ is installed, quit it (or uninstall it) so the two apps do not fight over the remote.$\r$\n$\r$\nClick Next to continue."
!define MUI_FINISHPAGE_RUN "$INSTDIR\Limelight.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Limelight now"
!define MUI_FINISHPAGE_TEXT "Limelight lives in the notification area (next to the clock).$\r$\n$\r$\nHold the highlight button lightly for the spotlight, press harder for the laser dot. Click the tray icon for settings."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Limelight" SecMain
  SectionIn RO
  ; Close a running copy so the file can be replaced.
  IfFileExists "$INSTDIR\Limelight.exe" 0 +2
    ExecWait '"$INSTDIR\Limelight.exe" --quit'
  SetOutPath "$INSTDIR"
  File "dist\Limelight.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortcut "$SMPROGRAMS\Limelight.lnk" "$INSTDIR\Limelight.exe"
  CreateShortcut "$SMPROGRAMS\Limelight diagnostics.lnk" "$INSTDIR\Limelight.exe" "--diagnostics" "$INSTDIR\Limelight.exe" 0

  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "Limelight"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "Limelight (open source)"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\Limelight.exe"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" 700
SectionEnd

Section "Start with Windows" SecAutostart
  WriteRegStr HKCU "${RUNKEY}" "Limelight" '"$INSTDIR\Limelight.exe" --background'
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\Limelight.lnk" "$INSTDIR\Limelight.exe"
SectionEnd

LangString DESC_Main ${LANG_ENGLISH} "The Limelight app (about 0.6 MB)."
LangString DESC_Auto ${LANG_ENGLISH} "Start Limelight quietly in the tray when you sign in."
LangString DESC_Desk ${LANG_ENGLISH} "Put a Limelight shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_Main)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutostart} $(DESC_Auto)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_Desk)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ExecWait '"$INSTDIR\Limelight.exe" --quit'
  Delete "$INSTDIR\Limelight.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Limelight.lnk"
  Delete "$SMPROGRAMS\Limelight diagnostics.lnk"
  Delete "$DESKTOP\Limelight.lnk"
  DeleteRegValue HKCU "${RUNKEY}" "Limelight"
  DeleteRegKey HKCU "${UNINSTKEY}"
  ; Settings in %APPDATA%\Limelight are kept so a reinstall remembers them.
SectionEnd
