call "build\ci\ci_includes.generated.cmd"

mkdir package
cd package

set PackageVersion=%PluginVersion%

if "%1"=="33" goto modules

copy ..\LICENSE          ..\release\data\obs-plugins\%PluginName%\LICENCE-%PluginName%.txt
set IsccDefines=
goto pack

:modules
REM OBS 33 and later: C:\ProgramData\obs-studio\plugins\<plugin>\<plugin>.dll and <plugin>\data
copy ..\LICENSE          ..\release\%PluginName%\data\LICENCE-%PluginName%.txt
set IsccDefines=/DOBS_MODULES

:pack
REM Package ZIP archive
7z a "%PluginName%-%PackageVersion%-obs%1-Windows.zip" "..\release\*"

REM Build installer
iscc %IsccDefines% /O. /F"%PluginName%-%PackageVersion%-obs%1-Windows-Installer" ..\build\installer-Windows.generated.iss

certutil.exe -hashfile "%PluginName%-%PackageVersion%-obs%1-Windows.zip" SHA1
certutil.exe -hashfile "%PluginName%-%PackageVersion%-obs%1-Windows-Installer.exe" SHA1
