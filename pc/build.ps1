# Build the PC tools with MSVC (Visual Studio 2022 or its Build Tools, with the
# C++ workload and a Windows 10/11 SDK; no vcvars needed).
#   .\build.ps1 -OpenVR C:\path\to\openvr -OpenXR C:\path\to\openxr
#     OpenVR: headers\, lib\win64\, bin\win64\ (from ValveSoftware/openvr)
#     OpenXR: include\, x64\{lib,bin}\ (openxr_loader_windows-<ver>.zip from
#             KhronosGroup/OpenXR-SDK-Source releases, unzipped)
param([Parameter(Mandatory)] [string]$OpenVR, [Parameter(Mandatory)] [string]$OpenXR)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vs) { throw "No Visual Studio (or Build Tools) with the C++ workload found." }
$vc = Join-Path $vs "VC\Tools\MSVC"
$msvc = Join-Path $vc (Get-ChildItem $vc | Sort-Object Name | Select-Object -Last 1).Name
$kit = "C:\Program Files (x86)\Windows Kits\10"
$kv = (Get-ChildItem "$kit\Include" | Sort-Object Name | Select-Object -Last 1).Name
$env:INCLUDE = "$msvc\include;$kit\Include\$kv\ucrt;$kit\Include\$kv\um;$kit\Include\$kv\shared"
$env:LIB = "$msvc\lib\x64;$kit\Lib\$kv\ucrt\x64;$kit\Lib\$kv\um\x64"
$cl = "$msvc\bin\Hostx64\x64\cl.exe"
$xrlib = "$OpenXR\x64"
if (-not (Test-Path "$xrlib\lib\openxr_loader.lib")) { $xrlib = "$OpenXR\native\x64\release" } # older nupkg layout
if (-not (Test-Path "$xrlib\lib\openxr_loader.lib")) { throw "No x64\lib\openxr_loader.lib under $OpenXR." }
New-Item -ItemType Directory -Force build | Out-Null

function Build($argv) { & $cl /nologo /O2 /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS @argv; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
Build @("/I$OpenVR\headers", "keybox.cpp", "/Fobuild\", "/Febuild\keybox.exe", "/link", "$OpenVR\lib\win64\openvr_api.lib")
Build @("/I$OpenXR\include", "xralphatest.cpp", "/Fobuild\", "/Febuild\xralphatest.exe", "/link",
	"$xrlib\lib\openxr_loader.lib", "d3d11.lib", "dxgi.lib")
Build @("/LD", "/I$OpenXR\include", "alphakey\alphakey.cpp", "/Fobuild\", "/Febuild\frame_alphakey.dll", "/link",
	"d3d11.lib", "d3dcompiler.lib", "ws2_32.lib", "iphlpapi.lib")
# The settings app: C# 5 / .NET Framework 4.x, compiled with the csc that ships with Windows.
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
& $csc -nologo -target:winexe -optimize -win32icon:app\icon.ico -r:System.Windows.Forms.dll -r:System.Drawing.dll `
	-out:build\FrameKey.exe app\FrameKey.cs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Copy-Item "$OpenVR\bin\win64\openvr_api.dll", "$xrlib\bin\openxr_loader.dll", "alphakey\alphakey.json" build\ -Force
if (-not (Test-Path build\alphakey.ini)) { Copy-Item alphakey\alphakey.ini build\ }
