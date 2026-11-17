; Arcana Blood +R installer (NSIS 3). Built by tools/make-release.sh:
;   makensis -DVERSION=0.1.0-beta -DVERSION_RC=0.1.0.119 -DICON=<.ico> -DSRC=<folder with the release files> -DOUT=<setup.exe> installer/installer.nsi
;
; Build 119: the mod goes into the game's folder. The game loads dinput8.dll from its own
; folder first; ours hands DirectInput on to Windows and loads ArcanaPlusR\arcana_rollback.dll.
; No game file is changed. The game starts from Steam as usual (hold Shift = without the mod).
; Uninstall removes dinput8.dll and the ArcanaPlusR folder again.
; rollback.ini (Settings, My setup) is never overwritten; one from the old launcher install
; (%LOCALAPPDATA%\Arcana Blood +R, builds up to 118) is taken over, and that install removed.
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"

!ifndef VERSION
  !error "VERSION not set"
!endif
!ifndef VERSION_RC
  !error "VERSION_RC not set (x.y.z.build)"
!endif
!define APPNAME "Arcana Blood +R"
!define APPID "989550"
!define GAMEEXE "Million Arthur.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ArcanaBloodPlusR"
!define REGKEY "Software\ArcanaBloodPlusR"

Name "${APPNAME} ${VERSION}"
OutFile "${OUT}"
RequestExecutionLevel user
InstallDir ""
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
!define MUI_WELCOMEPAGE_TEXT "This installs ${APPNAME}, a fan-made rollback netcode mod for the Steam version of Million Arthur: Arcana Blood.$\r$\n$\r$\nIt adds a small dinput8.dll and an ArcanaPlusR folder to the game's folder. No game file is changed. Afterwards just start the game from Steam; hold Shift while it starts to play without the mod.$\r$\n$\r$\nThis is a BETA: problems are expected."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Pick the folder of Million Arthur: Arcana Blood (the one with ${GAMEEXE} in it). It was looked up through Steam; change it only if it is wrong."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Game folder"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckGameDir
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION StartGame
!define MUI_FINISHPAGE_RUN_TEXT "Start the game now (through Steam)"
!define MUI_COMPONENTSPAGE_NODESC

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var Lib
Var Found
Var Old

; $Lib = a Steam library folder: is the game in it?
Function TryLib
  ${If} $Found == ""
  ${AndIf} ${FileExists} "$Lib\steamapps\common\Million Arthur Arcana Blood\${GAMEEXE}"
    StrCpy $Found "$Lib\steamapps\common\Million Arthur Arcana Blood"
  ${EndIf}
FunctionEnd

; the folder used last time, else Steam's folder, then every "path" in steamapps\libraryfolders.vdf
Function FindGame
  StrCpy $Found ""
  ReadRegStr $0 HKCU "${REGKEY}" "GameDir"
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\${GAMEEXE}"
    StrCpy $Found $0
    Return
  ${EndIf}
  ReadRegStr $1 HKCU "Software\Valve\Steam" "SteamPath"
  ${If} $1 == ""
    ReadRegStr $1 HKLM "SOFTWARE\WOW6432Node\Valve\Steam" "InstallPath"
  ${EndIf}
  ${If} $1 == ""
    Return
  ${EndIf}
  ${WordReplace} $1 "/" "\" "+" $1
  StrCpy $Lib $1
  Call TryLib
  ClearErrors
  FileOpen $2 "$1\steamapps\libraryfolders.vdf" r
  IfErrors vdfDone
  vdfLoop:
    ClearErrors
    FileRead $2 $3
    IfErrors vdfClose
    ${WordFind} $3 '"path"' "E+1}" $4      ; what comes after "path" on this line
    IfErrors vdfLoop
    ${WordFind} $4 '"' "E+2" $5            ; the text inside the next pair of quotes
    IfErrors vdfLoop
    ${WordReplace} $5 "\\" "\" "+" $5
    StrCpy $Lib $5
    Call TryLib
    Goto vdfLoop
  vdfClose:
  FileClose $2
  vdfDone:
FunctionEnd

Function .onInit
  Call FindGame
  StrCpy $INSTDIR $Found
FunctionEnd

Function CheckGameDir
  ${IfNot} ${FileExists} "$INSTDIR\${GAMEEXE}"
    MessageBox MB_OK|MB_ICONEXCLAMATION "There is no ${GAMEEXE} in that folder.$\r$\n$\r$\nIn Steam: right-click Million Arthur: Arcana Blood > Manage > Browse local files shows the right folder."
    Abort
  ${EndIf}
FunctionEnd

Function StartGame
  ExecShell "open" "steam://rungameid/${APPID}"
FunctionEnd

Section "${APPNAME} (required)" SecMain
  SectionIn RO
  ; a dinput8.dll from another mod (not ours) is kept aside, and put back on uninstall
  ${If} ${FileExists} "$INSTDIR\dinput8.dll"
  ${AndIfNot} ${FileExists} "$INSTDIR\ArcanaPlusR\arcana_rollback.dll"
  ${AndIfNot} ${FileExists} "$INSTDIR\dinput8.dll.before-plusr"
    Rename "$INSTDIR\dinput8.dll" "$INSTDIR\dinput8.dll.before-plusr"
    DetailPrint "Another dinput8.dll was there: kept as dinput8.dll.before-plusr"
  ${EndIf}
  SetOutPath "$INSTDIR"
  ClearErrors
  File "${SRC}\dinput8.dll"
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONSTOP "Could not write into the game folder (is the game still running?).$\r$\nClose the game and run the installer again. If it still fails, right-click the installer > Run as administrator."
    Abort
  ${EndIf}
  SetOutPath "$INSTDIR\ArcanaPlusR"
  File "${SRC}\ArcanaPlusR\arcana_rollback.dll"
  File "${SRC}\ArcanaPlusR\rollback_logo.png"
  File "${SRC}\ArcanaPlusR\README.txt"
  File "${SRC}\ArcanaPlusR\LICENSE.txt"
  File "/oname=arcana_plusr.ico" "${ICON}"
  ; Settings and My setup from the old launcher install (builds up to 118)
  ReadRegStr $Old HKCU "${REGKEY}" "InstallDir"
  ${If} $Old != ""
  ${AndIf} ${FileExists} "$Old\rollback.ini"
  ${AndIfNot} ${FileExists} "$INSTDIR\ArcanaPlusR\rollback.ini"
    CopyFiles /SILENT "$Old\rollback.ini" "$INSTDIR\ArcanaPlusR\rollback.ini"
    DetailPrint "Settings and My setup taken over from $Old"
  ${EndIf}
  SetOverwrite off
  File "${SRC}\ArcanaPlusR\rollback.ini"          ; keep the player's settings on reinstall / upgrade
  SetOverwrite on
  SetOutPath "$INSTDIR\ArcanaPlusR\fonts"
  File "${SRC}\ArcanaPlusR\fonts\*.*"
  SetOutPath "$INSTDIR\ArcanaPlusR"

  ; the old launcher install is not needed any more
  ${If} $Old != ""
  ${AndIf} ${FileExists} "$Old\arcana_launcher.exe"
    Delete "$Old\arcana_launcher.exe"
    Delete "$Old\arcana_launcher.exe.old"
    Delete "$Old\arcana_rollback.dll"
    Delete "$Old\rollback_logo.png"
    Delete "$Old\README.txt"
    Delete "$Old\LICENSE.txt"
    Delete "$Old\rollback.ini"
    Delete "$Old\rollback_*.txt"
    Delete "$Old\rollback_*.bin"
    Delete "$Old\launcher_log.txt"
    Delete "$Old\Uninstall.exe"
    RMDir /r "$Old\fonts"
    RMDir /r "$Old\update-backup"
    RMDir "$Old"
    DetailPrint "Removed the old launcher install in $Old"
  ${EndIf}
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  DeleteRegValue HKCU "${REGKEY}" "InstallDir"

  WriteRegStr HKCU "${REGKEY}" "GameDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\ArcanaPlusR\Uninstall.exe"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "GGenie"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\ArcanaPlusR\arcana_plusr.ico"
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR\ArcanaPlusR"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\ArcanaPlusR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "URLInfoAbout" "https://github.com/robinneefs/million-arthur-arcana-blood-plus-r"
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1

  ; shortcuts start the game through Steam (the mod comes with it)
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  WriteINIStr "$SMPROGRAMS\${APPNAME}\${APPNAME}.url" "InternetShortcut" "URL" "steam://rungameid/${APPID}"
  WriteINIStr "$SMPROGRAMS\${APPNAME}\${APPNAME}.url" "InternetShortcut" "IconFile" "$INSTDIR\ArcanaPlusR\arcana_plusr.ico"
  WriteINIStr "$SMPROGRAMS\${APPNAME}\${APPNAME}.url" "InternetShortcut" "IconIndex" "0"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\ArcanaPlusR\Uninstall.exe"
SectionEnd

Section "Desktop shortcut" SecDesktop
  WriteINIStr "$DESKTOP\${APPNAME}.url" "InternetShortcut" "URL" "steam://rungameid/${APPID}"
  WriteINIStr "$DESKTOP\${APPNAME}.url" "InternetShortcut" "IconFile" "$INSTDIR\ArcanaPlusR\arcana_plusr.ico"
  WriteINIStr "$DESKTOP\${APPNAME}.url" "InternetShortcut" "IconIndex" "0"
SectionEnd

Section "Uninstall"
  ; $INSTDIR is ...\Million Arthur Arcana Blood\ArcanaPlusR here
  Delete "$DESKTOP\${APPNAME}.url"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  GetFullPathName $0 "$INSTDIR\.."
  ${If} ${FileExists} "$0\${GAMEEXE}"
    Delete "$0\dinput8.dll"
    ${If} ${FileExists} "$0\dinput8.dll.before-plusr"
      Rename "$0\dinput8.dll.before-plusr" "$0\dinput8.dll"
    ${EndIf}
  ${EndIf}
  ; the mod's files, its logs and settings
  Delete "$INSTDIR\arcana_rollback.dll"
  Delete "$INSTDIR\rollback_logo.png"
  Delete "$INSTDIR\arcana_plusr.ico"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\rollback.ini"
  Delete "$INSTDIR\ui_theme.ini"
  Delete "$INSTDIR\rollback_*.txt"
  Delete "$INSTDIR\rollback_*.bin"
  Delete "$INSTDIR\loader_log.txt"
  RMDir /r "$INSTDIR\fonts"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "${REGKEY}"
SectionEnd
