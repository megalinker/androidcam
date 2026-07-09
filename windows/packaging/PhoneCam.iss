; PhoneCam installer (Inno Setup 6).
; Build:  ISCC /DAppVersion=0.1.0 PhoneCam.iss
; Payload defaults to the assembled ..\..\dist\PhoneCam folder (see assemble.ps1).

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef Payload
  #define Payload "..\..\dist\PhoneCam"
#endif

[Setup]
AppId={{8F3B7C40-9A21-4E6D-B0F5-7C2E1A9D4B88}
AppName=PhoneCam
AppVersion={#AppVersion}
AppPublisher=PhoneCam
DefaultDirName={autopf}\PhoneCam
DefaultGroupName=PhoneCam
DisableProgramGroupPage=yes
UninstallDisplayName=PhoneCam (virtual webcam)
UninstallDisplayIcon={app}\bin\receiver.exe
OutputDir={#Payload}\..
OutputBaseFilename=PhoneCam-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Files]
Source: "{#Payload}\PhoneCam.bat"; DestDir: "{app}"
Source: "{#Payload}\README.txt";   DestDir: "{app}"; Flags: isreadme
Source: "{#Payload}\PhoneCam.apk";  DestDir: "{app}"
Source: "{#Payload}\bin\*";         DestDir: "{app}\bin"; Flags: recursesubdirs
Source: "{#Payload}\redist\*";      DestDir: "{tmp}\redist"; Flags: deleteafterinstall

[Icons]
Name: "{group}\Start PhoneCam webcam";        Filename: "{app}\PhoneCam.bat"; WorkingDir: "{app}"; IconFilename: "{app}\bin\receiver.exe"
Name: "{group}\Install Android app (APK)";    Filename: "{app}\PhoneCam.apk"
Name: "{group}\README";                       Filename: "{app}\README.txt"
Name: "{group}\Uninstall PhoneCam";           Filename: "{uninstallexe}"
Name: "{autodesktop}\PhoneCam webcam";        Filename: "{app}\PhoneCam.bat"; WorkingDir: "{app}"; IconFilename: "{app}\bin\receiver.exe"; Tasks: desktopicon

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Run]
; Visual C++ runtime (needed by receiver.exe and by softcam.dll inside conferencing apps).
Filename: "{tmp}\redist\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ runtime (x64)..."; Flags: waituntilterminated
Filename: "{tmp}\redist\vc_redist.x86.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ runtime (x86)..."; Flags: waituntilterminated
; Register the "PhoneCam Camera" DirectShow virtual camera (64- and 32-bit).
Filename: "{sys}\regsvr32.exe";      Parameters: "/s ""{app}\bin\softcam\x64\softcam.dll""";   StatusMsg: "Adding PhoneCam Camera (64-bit)..."; Flags: waituntilterminated
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s ""{app}\bin\softcam\Win32\softcam.dll"""; StatusMsg: "Adding PhoneCam Camera (32-bit)..."; Flags: waituntilterminated
; Offer to launch.
Filename: "{app}\PhoneCam.bat"; Description: "Start PhoneCam now"; Flags: postinstall nowait skipifsilent unchecked

[UninstallRun]
Filename: "{sys}\regsvr32.exe";      Parameters: "/s /u ""{app}\bin\softcam\x64\softcam.dll""";   RunOnceId: "unreg64"; Flags: waituntilterminated
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /u ""{app}\bin\softcam\Win32\softcam.dll"""; RunOnceId: "unreg86"; Flags: waituntilterminated
