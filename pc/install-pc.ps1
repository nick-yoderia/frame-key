# Install the PC side for the current Windows user: the OpenXR API layer that
# lets OpenXR apps ask for transparency (ALPHA_BLEND) and turns it into the key
# colour, plus the small Frame Key window in the Start menu.
# No admin rights needed; nothing outside your user profile changes.
#   install-pc.ps1               install or update
#   install-pc.ps1 -Uninstall    remove it again
param([switch]$Uninstall)
$ErrorActionPreference = "Stop"
$dest = Join-Path $env:LOCALAPPDATA "frame-key"
$reg = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
$manifest = Join-Path $dest "alphakey.json"
$programs = [Environment]::GetFolderPath("Programs")
$shortcut = Join-Path $programs "Frame Key.lnk"
$files = "FrameKey.exe", "frame_alphakey.dll", "alphakey.json", "keybox.exe", "xralphatest.exe", "openvr_api.dll", "openxr_loader.dll"

# Earlier, private builds lived in %LOCALAPPDATA%\frame-pcvr-passthrough with a
# "Chroma Key" (or "Frame Passthrough") Start menu entry.
$oldDir = Join-Path $env:LOCALAPPDATA "frame-pcvr-passthrough"
$oldShortcuts = (Join-Path $programs "Chroma Key.lnk"), (Join-Path $programs "Frame Passthrough.lnk")

function Stop-Ours($dir) {
	# Only our own copies, found by path, so nothing else with the same name is touched.
	Get-Process FrameKey, ChromaKey, FramePassthrough, keybox -ErrorAction SilentlyContinue |
		Where-Object { $_.Path -and $_.Path.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) } |
		ForEach-Object { $_ | Stop-Process -Force; $_.WaitForExit(5000) | Out-Null }
}

function Remove-Layer($json) {
	if (Test-Path $reg) { Remove-ItemProperty -Path $reg -Name $json -ErrorAction SilentlyContinue }
}

# Remove a folder; a file a running game still has loaded (the OpenXR layer)
# can't be deleted, so say so instead of failing.
function Remove-Folder($dir) {
	if (-not (Test-Path $dir)) { return $true }
	Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
	if (Test-Path $dir) {
		Write-Host "Couldn't remove everything in $dir (a VR game may still be using it)."
		Write-Host "Close your VR games and run this again, or delete that folder later."
		return $false
	}
	return $true
}

function Remove-OldInstall {
	Stop-Ours $oldDir
	Remove-Layer (Join-Path $oldDir "alphakey.json")
	foreach ($s in $oldShortcuts) { if (Test-Path $s) { Remove-Item -Force $s } }
	Remove-Folder $oldDir | Out-Null
}

if ($Uninstall) {
	Stop-Ours $dest
	Remove-Layer $manifest
	if (Test-Path $shortcut) { Remove-Item -Force $shortcut }
	$ok = Remove-Folder $dest
	Remove-OldInstall
	if ($ok) { Write-Host "PC side removed." }
	return
}

$src = $PSScriptRoot
$missing = $files + "alphakey.ini" | Where-Object { -not (Test-Path (Join-Path $src $_)) }
if ($missing) {
	throw "Files are missing next to this script ($($missing -join ', ')). Extract the whole zip to a folder first, then run Install.cmd from there."
}

New-Item -ItemType Directory -Force $dest | Out-Null
# Carry settings over from an earlier private build.
foreach ($f in "alphakey.ini", "last-headset.txt") {
	$old = Join-Path $oldDir $f
	if ((Test-Path $old) -and -not (Test-Path (Join-Path $dest $f))) { Copy-Item $old $dest }
}
Remove-OldInstall
Stop-Ours $dest

# Files left from an update while a game had the layer loaded.
Get-ChildItem $dest -Filter "*.old*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
foreach ($f in $files) {
	$to = Join-Path $dest $f
	try {
		Copy-Item (Join-Path $src $f) $to -Force
	} catch {
		# A running OpenXR game keeps frame_alphakey.dll (or the loader) open. Windows
		# lets a loaded file be renamed, so move it aside; the game keeps using the
		# old one until it restarts, and the next install deletes it.
		$aside = "$to.old$(Get-Random)"
		Move-Item -Force $to $aside
		Copy-Item (Join-Path $src $f) $to -Force
	}
}
# Downloaded zips mark their files as "from the internet"; clear that so
# Windows doesn't warn every time the window opens.
Get-ChildItem $dest -File | Unblock-File -ErrorAction SilentlyContinue
# Keep your settings across updates.
if (-not (Test-Path (Join-Path $dest "alphakey.ini"))) { Copy-Item (Join-Path $src "alphakey.ini") $dest }

if (-not (Test-Path $reg)) { New-Item -Path $reg -Force | Out-Null }
New-ItemProperty -Path $reg -Name $manifest -Value 0 -PropertyType DWord -Force | Out-Null
$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut($shortcut)
$lnk.TargetPath = Join-Path $dest "FrameKey.exe"
$lnk.WorkingDirectory = $dest
$lnk.Description = "Passthrough keying for PC VR on the Steam Frame"
$lnk.Save()
Write-Host "PC side installed to $dest"
Write-Host "Start menu: Frame Key (which headset this PC follows, the PC layer switch, a test box)."
