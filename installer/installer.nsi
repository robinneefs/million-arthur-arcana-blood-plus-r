; Arcana Blood +R installer (NSIS 3). Built by tools/make-release.sh:
;   makensis -DVERSION=0.2.0-beta -DVERSION_RC=0.2.0.120 -DICON=<.ico> -DSRC=<folder with the release files> -DOUT=<setup.exe> installer/installer.nsi
;
; Build 120: the mod lives in its own folder (default %LOCALAPPDATA%\Arcana Blood +R) with the
; launcher. The launcher puts one small dinput8.dll into the game's folder when the player
; presses Play; that DLL loads the mod only when the launcher started the game.
; Installs for the current user only (no admin rights), so the launcher can update the files.
; rollback.ini (Settings, My setup) is never overwritten; one from build 119 (in the game's
; folder, ArcanaPlusR\) is taken over, and that folder removed.
; Uninstall removes our folder, and dinput8.dll from the game's folder (putting back a
; dinput8.dll.before-plusr that another mod had there).
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !error "VERSION not set"
!endif
!ifndef VERSION_RC
  !error "VERSION_RC not set (x.y.z.build)"
!endif
!define APPNAME "Arcana Blood +R"
!define GAMEEXE "Million Arthur.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ArcanaBloodPlusR"
!define REGKEY "Software\ArcanaBloodPlusR"

Name "${APPNAME} ${VERSION}"
OutFile "${OUT}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\${APPNAME}"
InstallDirRegKey HKCU "${REGKEY}" "InstallDir"
SetCompressor /SOLID lzma
BrandingText "${APPNAME} ${VERSION}"
VIProductVersion "${VERSION_RC}"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} ${VERSION} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "GGenie (fan-made, open source)"
VIAddVersionKey "LegalCopyright" "MIT licence. github.com/robinneefs/million-arthur-arcana-blood-plus-r"

!define MUI_ABORTWARNING
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_WELCOMEPAGE_TITLE "${APPNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This installs ${APPNAME}, a fan-made rollback netcode mod for the Steam version of Million Arthur: Arcana Blood.$\r$\n$\r$\nStart it with the ${APPNAME} shortcut: the launcher keeps the mod up to date and starts the game with it. It puts one small file (dinput8.dll) into the game's folder, which does nothing when you start the game from Steam. No game file is changed.$\r$\n$\r$\nThis is a BETA: problems are expected."
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

Var Game

Section "${APPNAME} (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  ; the running launcher can't be overwritten
  FindWindow $0 "ArcanaBloodPlusRLauncher"
  ${If} $0 <> 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "Please close the ${APPNAME} launcher first, then press OK."
  ${EndIf}
  ClearErrors
  File "${SRC}\arcana_launcher.exe"
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONSTOP "Could not write arcana_launcher.exe (is the launcher or the game still open?). Close them and run the installer again."
    Abort
  ${EndIf}
  File "${SRC}\arcana_rollback.dll"
  File "${SRC}\dinput8.dll"
  File "${SRC}\rollback_logo.png"
  File "${SRC}\README.txt"
  File "${SRC}\LICENSE.txt"
  SetOutPath "$INSTDIR\fonts"
  File "${SRC}\fonts\*.*"
  SetOutPath "$INSTDIR"
  Delete "$INSTDIR\arcana_launcher.exe.old"

  ; build 119 had the mod in the game's folder (ArcanaPlusR\): take its settings, remove it.
  ; Its dinput8.dll stays: the launcher swaps it for the new one on Play.
  ReadRegStr $Game HKCU "${REGKEY}" "GameDir"
  ${If} $Game != ""
  ${AndIf} ${FileExists} "$Game\ArcanaPlusR\*.*"
    ${IfNot} ${FileExists} "$INSTDIR\rollback.ini"
    ${AndIf} ${FileExists} "$Game\ArcanaPlusR\rollback.ini"
      CopyFiles /SILENT "$Game\ArcanaPlusR\rollback.ini" "$INSTDIR\rollback.ini"
      DetailPrint "Settings and My setup taken over from $Game\ArcanaPlusR"
    ${EndIf}
    RMDir /r "$Game\ArcanaPlusR"
    DetailPrint "Removed the build 119 ArcanaPlusR folder from the game's folder"
  ${EndIf}
  ; build 119 shortcuts (they started the game through Steam)
  Delete "$DESKTOP\${APPNAME}.url"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.url"

  SetOverwrite off
  File "${SRC}\rollback.ini"          ; keep the player's settings on reinstall / upgrade
  SetOverwrite on

  WriteRegStr HKCU "${REGKEY}" "InstallDir" "$INSTDIR"
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
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\arcana_launcher.exe"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\arcana_launcher.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  ; our dinput8.dll in the game's folder (the launcher noted where it put it)
  ReadRegStr $0 HKCU "${REGKEY}" "GameDir"
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\${GAMEEXE}"
    Delete "$0\dinput8.dll"
    ${If} ${FileExists} "$0\dinput8.dll.before-plusr"
      Rename "$0\dinput8.dll.before-plusr" "$0\dinput8.dll"
    ${EndIf}
    RMDir /r "$0\ArcanaPlusR"
  ${EndIf}
  ; the mod's files, its logs and settings
  Delete "$INSTDIR\arcana_launcher.exe"
  Delete "$INSTDIR\arcana_launcher.exe.old"
  Delete "$INSTDIR\arcana_rollback.dll"
  Delete "$INSTDIR\dinput8.dll"
  Delete "$INSTDIR\rollback_logo.png"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\rollback.ini"
  Delete "$INSTDIR\ui_theme.ini"
  Delete "$INSTDIR\rollback_*.txt"
  Delete "$INSTDIR\rollback_*.bin"
  Delete "$INSTDIR\launcher_log.txt"
  Delete "$INSTDIR\loader_log.txt"
  RMDir /r "$INSTDIR\fonts"
  RMDir /r "$INSTDIR\update-backup"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "${REGKEY}"
SectionEnd
