# Install (or with -Uninstall remove) the headset side on a Steam Frame, over SSH.
#   install-headset.ps1 [-Target steamos@<address>] [-Uninstall]
# Without -Target it looks for headsets that already run Frame Key on your
# network (updates), offers the one used last time, or asks for the address.
# The first time it asks for the Frame's password once and adds an SSH key
# made just for this (~\.ssh\frame-key_ed25519), so later runs don't ask.
param([switch]$Uninstall, [string]$Target)
$ErrorActionPreference = "Stop"

foreach ($t in "ssh", "scp", "ssh-keygen") {
	if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
		throw "No $t.exe. Add Windows' OpenSSH Client: Settings > System > Optional features > Add a feature > OpenSSH Client, then run this again."
	}
}
$headset = Join-Path (Split-Path $PSScriptRoot) "headset"
$need = if ($Uninstall) { "uninstall-headset.sh" } else { "install-headset.sh", "bin\frame-keyer-panel", "libVkLayer_frame_keyer.so" }
foreach ($f in $need) {
	if (-not (Test-Path (Join-Path $headset $f))) {
		throw "$f is missing from the headset folder. Extract the whole zip to a folder first, then run this from there."
	}
}

$state = Join-Path $env:LOCALAPPDATA "frame-key"
$last = Join-Path $state "last-headset.txt"
$oldLast = Join-Path $env:LOCALAPPDATA "frame-pcvr-passthrough\last-headset.txt"
New-Item -ItemType Directory -Force $state | Out-Null
if (-not (Test-Path $last) -and (Test-Path $oldLast)) { Copy-Item $oldLast $last }

# Headsets that already run the Frame Key panel answer a UDP broadcast
# (the same one the PC layer uses), so an update finds them by itself.
function Find-Headsets {
	$found = @{}
	$udp = New-Object System.Net.Sockets.UdpClient 0
	try {
		$udp.EnableBroadcast = $true
		$q = [Text.Encoding]::ASCII.GetBytes("FPPQ1")
		$targets = @([Net.IPAddress]::Broadcast)
		foreach ($ni in [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
			if ($ni.OperationalStatus -ne "Up" -or $ni.NetworkInterfaceType -eq "Loopback") { continue }
			foreach ($u in $ni.GetIPProperties().UnicastAddresses) {
				if ($u.Address.AddressFamily -ne "InterNetwork" -or $u.PrefixLength -le 0 -or $u.PrefixLength -ge 31) { continue }
				$b = $u.Address.GetAddressBytes()
				$ip = ([uint32]$b[0] -shl 24) -bor ([uint32]$b[1] -shl 16) -bor ([uint32]$b[2] -shl 8) -bor [uint32]$b[3]
				$mask = [uint32]([math]::Pow(2, 32 - $u.PrefixLength) - 1)
				$bc = $ip -bor $mask
				$targets += [Net.IPAddress]::new([byte[]](@(24, 16, 8, 0) | ForEach-Object { ($bc -shr $_) -band 255 }))
			}
		}
		foreach ($t in $targets) { try { [void]$udp.Send($q, $q.Length, [Net.IPEndPoint]::new($t, 47911)) } catch { } }
		$until = (Get-Date).AddMilliseconds(1500)
		while ((Get-Date) -lt $until) {
			if ($udp.Available -eq 0) { Start-Sleep -Milliseconds 50; continue }
			$from = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
			$msg = [Text.Encoding]::UTF8.GetString($udp.Receive([ref]$from))
			if (-not $msg.StartsWith("FPPA1`n")) { continue }
			$kv = @{}
			foreach ($line in $msg -split "`n") { $i = $line.IndexOf("="); if ($i -gt 0) { $kv[$line.Substring(0, $i)] = $line.Substring($i + 1) } }
			if ($kv.id -and -not $found.ContainsKey($kv.id)) {
				$found[$kv.id] = [pscustomobject]@{ Name = $kv.name; Address = "steamos@$($from.Address)" }
			}
		}
	} finally { $udp.Close() }
	return @($found.Values)
}

# Hosts named in ~\.ssh\config are used as they are; anything else gets the
# Frame's user name, steamos.
function Get-SshHosts {
	$cfg = Join-Path $env:USERPROFILE ".ssh\config"
	if (-not (Test-Path $cfg)) { return @() }
	Select-String -Path $cfg -Pattern '^\s*Host\s+(.+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value -split '\s+' } | Where-Object { $_ -notmatch '[*?!]' }
}
function Resolve-Target($t) {
	$t = $t.Trim()
	if ($t -match "@" -or (Get-SshHosts) -contains $t) { return $t }
	return "steamos@$t"
}

if (-not $Target) {
	$prev = if (Test-Path $last) { (Get-Content $last -First 1).Trim() } else { "" }
	Write-Host "Looking for headsets running Frame Key on your network..."
	$heads = Find-Headsets
	$choices = @()
	foreach ($h in $heads) { $choices += [pscustomobject]@{ Label = "$($h.Name) ($($h.Address))"; Target = $h.Address } }
	if ($prev -and -not ($choices | Where-Object { $_.Target -eq $prev })) {
		$choices += [pscustomobject]@{ Label = "$prev (used last time)"; Target = $prev }
	}
	if ($choices.Count -eq 0) {
		Write-Host ""
		Write-Host "Enter your Steam Frame's IP address. You can find it in the Frame's Wi-Fi"
		Write-Host "settings (the connected network's details) or in your router's device list."
		$Target = Read-Host "Steam Frame address (e.g. 192.168.1.50)"
		if (-not $Target) { throw "No address given." }
	} else {
		Write-Host ""
		for ($i = 0; $i -lt $choices.Count; $i++) { Write-Host "  [$($i + 1)] $($choices[$i].Label)" }
		$a = Read-Host "Install to which headset? Enter for [1], a number, or another address"
		if (-not $a) { $a = "1" }
		if ($a -match '^\d+$' -and [int]$a -ge 1 -and [int]$a -le $choices.Count) { $Target = $choices[[int]$a - 1].Target } else { $Target = $a }
	}
}
$Target = Resolve-Target $Target

# An SSH key made only for this, without a passphrase, so installs and updates
# run without questions. Your other SSH keys aren't used or changed.
$key = Join-Path $env:USERPROFILE ".ssh\frame-key_ed25519"
if (-not (Test-Path "$key.pub")) {
	New-Item -ItemType Directory -Force (Split-Path $key) | Out-Null
	# An empty passphrase: Windows PowerShell 5.1 drops a bare '' argument.
	$empty = if ($PSVersionTable.PSVersion -ge [version]"7.3") { '' } else { '""' }
	ssh-keygen -q -t ed25519 -N $empty -f $key -C "frame-key@$env:COMPUTERNAME" | Out-Null
	if (-not (Test-Path "$key.pub")) { throw "Couldn't create an SSH key at $key." }
}
$pub = (Get-Content "$key.pub" -First 1).Trim()
# accept-new: trust the Frame's host key the first time without a yes/no question
# (a changed key is still refused).
$opts = @("-i", $key, "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=10",
	"-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=3")
$batch = $opts + @("-o", "BatchMode=yes")

function Test-Port($hostName, $port) {
	$c = New-Object System.Net.Sockets.TcpClient
	try { return $c.ConnectAsync($hostName, $port).Wait(5000) -and $c.Connected } catch { return $false } finally { $c.Close() }
}
function Explain-NoLogin {
	$cfg = @{}
	ssh -G @opts $Target 2>$null | ForEach-Object { $p = $_ -split ' ', 2; if ($p.Count -eq 2 -and -not $cfg.ContainsKey($p[0])) { $cfg[$p[0]] = $p[1] } }
	$h = $cfg["hostname"]; $port = if ($cfg["port"]) { [int]$cfg["port"] } else { 22 }
	if (-not (Test-Port $h $port)) {
		throw "Can't reach $h on port $port. Check the address, that the Frame is on and on the same network, and that SSH is turned on on the Frame (see README: 'Before you install')."
	}
	throw "Couldn't log in to $Target. Check the Frame's password (see README: 'Before you install'). If it says the host key changed (e.g. after a reset), run: ssh-keygen -R $h"
}

Write-Host "Connecting to $Target ..."
ssh @batch $Target "true" 2>$null
if ($LASTEXITCODE -ne 0) {
	# First time: log in with the password once and add this PC's key.
	Write-Host "First connection: enter the Frame's password (for the steamos user) when asked."
	ssh @opts $Target "umask 077; mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys && (grep -qxF '$pub' ~/.ssh/authorized_keys || echo '$pub' >> ~/.ssh/authorized_keys)"
	if ($LASTEXITCODE -ne 0) { Explain-NoLogin }
	ssh @batch $Target "true" 2>$null
	if ($LASTEXITCODE -ne 0) { throw "The key was added but the Frame still asks for a password; check ~/.ssh permissions on the Frame." }
}
Set-Content -Path $last -Value $Target

$remote = ".cache/frame-key-install"
ssh @batch $Target "rm -rf ~/$remote && mkdir -p ~/$remote"
if ($LASTEXITCODE) { throw "Couldn't prepare a folder on the headset." }
scp @batch -q -r "$headset\*" "${Target}:$remote/"
if ($LASTEXITCODE) { throw "Copying to the headset failed." }
if (-not $Uninstall) { Write-Host "Installing. SteamVR on the Frame restarts at the end, which closes any running VR app." }
$script = if ($Uninstall) { "uninstall-headset.sh" } else { "install-headset.sh" }
ssh @batch $Target "sh ~/$remote/$script; rc=`$?; rm -rf ~/$remote; exit `$rc"
if ($LASTEXITCODE) { throw "The headset script failed (see above)." }
