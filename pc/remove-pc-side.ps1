# Remove the PC side that Frame Key releases before 0.7 installed: the OpenXR
# API layer (frame_alphakey), the Frame Key window and its Start menu entry.
# Frame Key now only keys apps running on the Frame itself; SteamOS 0.5.9 keys
# PC streams on its own, so nothing of Frame Key belongs in PC VR games any more.
# Install.cmd runs this before installing the headset side. No admin rights needed.
#   remove-pc-side.ps1          remove the old PC side, keep the installer's own
#                               state (last headset address)
#   remove-pc-side.ps1 -All     remove that too (Uninstall.cmd)
param([switch]$All)
$ErrorActionPreference = "Stop"
$reg = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
$programs = [Environment]::GetFolderPath("Programs")
$dest = Join-Path $env:LOCALAPPDATA "frame-key"
# Earlier, private builds lived in %LOCALAPPDATA%\frame-pcvr-passthrough.
$oldDir = Join-Path $env:LOCALAPPDATA "frame-pcvr-passthrough"
$keep = "last-headset.txt"

# The installer's remembered headset address moves along.
$oldLast = Join-Path $oldDir $keep
if ((Test-Path $oldLast) -and -not (Test-Path (Join-Path $dest $keep))) {
	New-Item -ItemType Directory -Force $dest | Out-Null
	Copy-Item $oldLast $dest
}

$found = $false
foreach ($dir in $dest, $oldDir) {
	# Only our own copies, found by path, so nothing else with the same name is touched.
	Get-Process FrameKey, ChromaKey, FramePassthrough, keybox -ErrorAction SilentlyContinue |
		Where-Object { $_.Path -and $_.Path.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase) } |
		ForEach-Object { $_ | Stop-Process -Force; $_.WaitForExit(5000) | Out-Null }
	$json = Join-Path $dir "alphakey.json"
	if ((Test-Path $reg) -and ((Get-Item $reg).Property -contains $json)) {
		Remove-ItemProperty -Path $reg -Name $json
		$found = $true
	}
}
foreach ($s in "Frame Key.lnk", "Chroma Key.lnk", "Frame Passthrough.lnk") {
	$lnk = Join-Path $programs $s
	if (Test-Path $lnk) { Remove-Item -Force $lnk; $found = $true }
}

$left = $false
foreach ($dir in $dest, $oldDir) {
	if (-not (Test-Path $dir)) { continue }
	$items = Get-ChildItem $dir -Force | Where-Object { $All -or $dir -ne $dest -or $_.Name -ne $keep }
	if ($items) { $found = $true }
	# A file a running game still has loaded (the OpenXR layer) can't be deleted.
	# The layer is already off for every game started from now on.
	if ($items) { $items | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }
	if (-not (Get-ChildItem $dir -Force)) { Remove-Item -Force $dir }
	elseif (Get-ChildItem $dir -Force | Where-Object { $All -or $dir -ne $dest -or $_.Name -ne $keep }) { $left = $true }
}
if ($left) {
	Write-Host "Some files of the old PC side are still in use (a VR game is running)."
	Write-Host "It is switched off already; close your VR games and run this again to delete the files."
} elseif ($found) {
	Write-Host "Old PC side removed (Frame Key no longer installs anything into PC VR games)."
} elseif ($All) {
	Write-Host "Nothing of Frame Key left on this PC."
}
