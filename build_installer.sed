[Version]
Class=IEXPRESS
SEDVersion=3

[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeQuota=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=%DisplayLicense%
FinishMessage=%FinishMessage%
TargetName=%TargetName%
FriendlyName=%FriendlyName%
AppLaunched=%AppLaunched%
PostInstallCmd=%PostInstallCmd%
AdminQuietInstCmd=%AdminQuietInstCmd%
UserQuietInstCmd=%UserQuietInstCmd%
SourceFiles=SourceFiles

[Strings]
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=PhoneLinkLock_Setup.exe
FriendlyName=PhoneLinkLock Installer
AppLaunched=cmd.exe /c "mkdir ""%LocalAppData%\PhoneLinkLock"" & copy /Y PhoneLinkLock.exe ""%LocalAppData%\PhoneLinkLock\"" & start """" ""%LocalAppData%\PhoneLinkLock\PhoneLinkLock.exe"""
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=

[SourceFiles]
SourceFiles0=build\Release\

[SourceFiles0]
PhoneLinkLock.exe=
