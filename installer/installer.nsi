; Arcana Blood +R installer (NSIS 3). Built by tools/make-release.sh:
;   makensis -DVERSION=0.1.0-beta -DSRC=<folder with the release files> -DOUT=<setup.exe> installer/installer.nsi
; Installs for the current user only (no admin rights), so the launcher's update check can
; replace the files later. rollback.ini (Settings, My setup) is never overwritten.
Unicode true
!include "MUI2.nsh"

!ifndef VERSION
  !error "VERSION not set"
!endif
!define APPNAME "Arcana Blood +R"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ArcanaBloodPlusR"

Name "${APPNAME} ${VERSION}"
OutFile "${OUT}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\${APPNAME}"
InstallDirRegKey HKCU "Software\ArcanaBloodPlusR" "InstallDir"
SetCompressor /SOLID lzma
BrandingText "${APPNAME} ${VERSION}"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "${APPNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This installs ${APPNAME}, a fan-made rollback netcode mod for the Steam version of Million Arthur: Arcana Blood.$\r$\n$\r$\nThe mod never changes the game's files. You need the Steam version of the game and Steam running.$\r$\n$\r$\nThis is a BETA: problems are expected."
!define MUI_FINISHPAGE_RUN "$INSTDIR\arcana_launcher.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APPNAME} now"
!define MUI_COMPONENTSPAGE_NODESC

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "${APPNAME} (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${SRC}\arcana_launcher.exe"
  File "${SRC}\arcana_rollback.dll"
  File "${SRC}\rollback_logo.png"
  File "${SRC}\README.txt"
  File "${SRC}\LICENSE.txt"
  SetOverwrite off
  File "${SRC}\rollback.ini"          ; keep the player's settings on reinstall / upgrade
  SetOverwrite on
  SetOutPath "$INSTDIR\fonts"
  File "${SRC}\fonts\*.*"
  SetOutPath "$INSTDIR"

  WriteRegStr HKCU "Software\ArcanaBloodPlusR" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "GGenie"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\arcana_launcher.exe"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "URLInfoAbout" "https://github.com/robinneefs/million-arthur-arcana-blood-plus-r"
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\arcana_launcher.exe" "" "$INSTDIR\arcana_launcher.exe"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\arcana_launcher.exe" "" "$INSTDIR\arcana_launcher.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  ; the mod's own files, its logs and the update backup; the folder goes if nothing else is left
  Delete "$INSTDIR\arcana_launcher.exe"
  Delete "$INSTDIR\arcana_launcher.exe.old"
  Delete "$INSTDIR\arcana_rollback.dll"
  Delete "$INSTDIR\rollback_logo.png"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\rollback.ini"
  Delete "$INSTDIR\rollback_*.txt"
  Delete "$INSTDIR\rollback_*.bin"
  Delete "$INSTDIR\launcher_log.txt"
  RMDir /r "$INSTDIR\fonts"
  RMDir /r "$INSTDIR\update-backup"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "Software\ArcanaBloodPlusR"
SectionEnd
