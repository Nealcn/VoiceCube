; VoiceStick NSIS Installer
!define PRODUCT_NAME "VoiceStick"
!define PRODUCT_VERSION "1.0.0"
!define PRODUCT_PUBLISHER "VoiceStick"

Name "${PRODUCT_NAME}"
OutFile "dist\${PRODUCT_NAME}_Setup.exe"
InstallDir "$PROGRAMFILES\${PRODUCT_NAME}"
RequestExecutionLevel admin

Section "Install"
  SetOutPath "$INSTDIR"
  File "dist\${PRODUCT_NAME}\${PRODUCT_NAME}.exe"
  CreateShortCut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\${PRODUCT_NAME}.exe"
  CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
  CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" "$INSTDIR\${PRODUCT_NAME}.exe"
  CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk" "$INSTDIR\uninst.exe"
  WriteUninstaller "$INSTDIR\uninst.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" "UninstallString" "$INSTDIR\uninst.exe"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\${PRODUCT_NAME}.exe"
  Delete "$INSTDIR\uninst.exe"
  RMDir "$INSTDIR"
  Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
  RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
SectionEnd
