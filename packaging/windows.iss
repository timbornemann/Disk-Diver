[Setup]
AppId={{82C34ED8-7761-48F7-A966-BBB28B649989}
AppName=Disk-Diver
AppPublisher=Disk-Diver contributors
AppVersion={#AppVersion}
DefaultDirName={autopf}\Disk-Diver
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
OutputBaseFilename=Disk-Diver-{#AppVersion}-windows-x64
Compression=lzma2
SolidCompression=yes
SetupIconFile=..\assets\disk-diver.ico
UninstallDisplayIcon={app}\disk-diver.exe
WizardStyle=modern

[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{commonprograms}\Disk-Diver"; Filename: "{app}\disk-diver.exe"; IconFilename: "{app}\disk-diver.exe"
