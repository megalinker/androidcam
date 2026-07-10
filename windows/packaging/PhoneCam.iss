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
Source: "{#Payload}\PhoneCam.exe"; DestDir: "{app}"
Source: "{#Payload}\PhoneCam.bat"; DestDir: "{app}"
Source: "{#Payload}\README.txt";   DestDir: "{app}"; Flags: isreadme
Source: "{#Payload}\PhoneCam.apk";  DestDir: "{app}"
Source: "{#Payload}\bin\*";         DestDir: "{app}\bin"; Flags: recursesubdirs
Source: "{#Payload}\redist\*";      DestDir: "{tmp}\redist"; Flags: deleteafterinstall

[Icons]
Name: "{group}\PhoneCam";                     Filename: "{app}\PhoneCam.exe"; WorkingDir: "{app}"
Name: "{group}\PhoneCam (command line)";      Filename: "{app}\PhoneCam.bat"; WorkingDir: "{app}"; IconFilename: "{app}\bin\receiver.exe"
Name: "{group}\Install Android app (APK)";    Filename: "{app}\PhoneCam.apk"
Name: "{group}\README";                       Filename: "{app}\README.txt"
Name: "{group}\Uninstall PhoneCam";           Filename: "{uninstallexe}"
Name: "{autodesktop}\PhoneCam";               Filename: "{app}\PhoneCam.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Run]
; Visual C++ runtime (needed by receiver.exe and by softcam.dll inside conferencing apps).
Filename: "{tmp}\redist\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ runtime (x64)..."; Flags: waituntilterminated
Filename: "{tmp}\redist\vc_redist.x86.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ runtime (x86)..."; Flags: waituntilterminated
; Register the "PhoneCam Camera" DirectShow virtual camera (64- and 32-bit).
Filename: "{sys}\regsvr32.exe";      Parameters: "/s ""{app}\bin\softcam\x64\softcam.dll""";   StatusMsg: "Adding PhoneCam Camera (64-bit)..."; Flags: waituntilterminated
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s ""{app}\bin\softcam\Win32\softcam.dll"""; StatusMsg: "Adding PhoneCam Camera (32-bit)..."; Flags: waituntilterminated
; Windows Firewall: allow the phone to reach the PC (the Wi-Fi QR pairing listens for an inbound
; connection from the phone; without this, Windows silently blocks it on first run — especially on
; "Public" networks — and the phone stays on "waiting for PC to connect").
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""PhoneCam"" dir=in action=allow program=""{app}\PhoneCam.exe"" enable=yes profile=any"; StatusMsg: "Allowing PhoneCam through Windows Firewall..."; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""PhoneCam Receiver"" dir=in action=allow program=""{app}\bin\receiver.exe"" enable=yes profile=any"; StatusMsg: "Allowing PhoneCam through Windows Firewall..."; Flags: runhidden waituntilterminated
; Offer to launch.
Filename: "{app}\PhoneCam.exe"; Description: "Start PhoneCam now"; Flags: postinstall nowait skipifsilent unchecked

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var rc: Integer;
begin
  // Force-close a running PhoneCam so its .exe/.dll aren't locked. Without this, installing over a
  // running instance silently keeps the OLD exe (the file can't be overwritten), so the "update"
  // does nothing — which is exactly what a tester hit (no version / no diagnostics = stale build).
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im PhoneCam.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im receiver.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Result := '';
end;

[UninstallRun]
Filename: "{sys}\regsvr32.exe";      Parameters: "/s /u ""{app}\bin\softcam\x64\softcam.dll""";   RunOnceId: "unreg64"; Flags: waituntilterminated
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /u ""{app}\bin\softcam\Win32\softcam.dll"""; RunOnceId: "unreg86"; Flags: waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""PhoneCam"""; RunOnceId: "fwdel1"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""PhoneCam Receiver"""; RunOnceId: "fwdel2"; Flags: runhidden waituntilterminated
