<#
.SYNOPSIS
  Package the FRIEND kit: one zip that takes a friend from nothing to
  "click JOIN GAME" with a single double-click. For friends who are bad at
  computers: no Tailscale account, no login, no addresses, no mod menu.

.DESCRIPTION
  Kit contents (dist\friend-kit\, zipped to dist\KenshiCoop-friend-kit.zip):
    SETUP.cmd                - double-click this. Self-elevates (one UAC "Yes"),
                               then runs SETUP.ps1.
    SETUP.ps1                - the real work:
                                 1. finds Kenshi (Steam default; asks if not there)
                                 2. warns loudly if RE_Kenshi is missing (link)
                                 3. copies the KenshiCoop mod folder into mods\
                                 4. ENABLES the mod in data\mods.cfg
                                 5. installs Tailscale if missing (winget, then
                                    MSI download fallback)
                                 6. joins YOUR tailnet with the embedded pre-auth
                                    key (tailscale up --auth-key, unattended - no
                                    account, no login screen)
    KenshiCoop\              - the drop-in mod folder (Release build)
    README.txt               - three-line friend instructions

  THE AUTH KEY IS A SECRET. Generate one at
  https://login.tailscale.com/admin/settings/keys ("Generate auth key":
  Reusable ON so one kit serves both friends, expiry as short as you can live
  with). Anyone with the zip can join your tailnet until the key expires or
  you revoke it - send it over a private channel and revoke the key once your
  friends are in (their machines STAY joined after revocation).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_friend_kit.ps1 -AuthKey tskey-auth-XXXXX
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_friend_kit.ps1 -AuthKey tskey-auth-XXXXX -SkipBuild
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AuthKey,
    [switch]$SkipBuild,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if ($AuthKey -notmatch '^tskey-') {
    Write-Warning "auth key does not start with 'tskey-' - double-check it (continuing)"
}

if (-not $SkipBuild) {
    Write-Host "=== build plugin (Release / shipped) ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" Release"
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

function Resolve-First([string[]]$candidates, [string]$what) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    throw "$what not found (looked in: $($candidates -join '; '))"
}
$dll  = Resolve-First @(
    (Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"),
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.dll")
) "KenshiCoop.dll"
$modf = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.mod"),
    (Join-Path $HostDir "mods\KenshiCoop\KenshiCoop.mod")
) "KenshiCoop.mod"
$json = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"),
    (Join-Path $HostDir "mods\KenshiCoop\RE_Kenshi.json")
) "RE_Kenshi.json"

$kit = Join-Path $repoRoot "dist\friend-kit"
if (Test-Path $kit) { Remove-Item $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $kit "KenshiCoop") | Out-Null
Copy-Item $dll  (Join-Path $kit "KenshiCoop\KenshiCoop.dll")
Copy-Item $modf (Join-Path $kit "KenshiCoop\KenshiCoop.mod")
Copy-Item $json (Join-Path $kit "KenshiCoop\RE_Kenshi.json")

# ---- SETUP.cmd: double-click entry; self-elevates and runs SETUP.ps1 ----------
@'
@echo off
REM KenshiCoop friend setup - double-click me. One UAC prompt, then automatic.
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights ^(click Yes^)...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0SETUP.ps1"
pause
'@ | Set-Content -Path (Join-Path $kit "SETUP.cmd") -Encoding ASCII

# ---- SETUP.ps1: the friend-machine bootstrap ----------------------------------
$setup = @'
# KenshiCoop friend setup (run by SETUP.cmd; elevated).
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Write-Host ""
Write-Host "=== KenshiCoop setup ===" -ForegroundColor Cyan

# 1. Find Kenshi.
$kenshi = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
if (-not (Test-Path (Join-Path $kenshi "kenshi_x64.exe"))) {
    foreach ($drive in @("D:", "E:", "F:")) {
        $try = "$drive\SteamLibrary\steamapps\common\Kenshi"
        if (Test-Path (Join-Path $try "kenshi_x64.exe")) { $kenshi = $try; break }
        $try = "$drive\Steam\steamapps\common\Kenshi"
        if (Test-Path (Join-Path $try "kenshi_x64.exe")) { $kenshi = $try; break }
    }
}
while (-not (Test-Path (Join-Path $kenshi "kenshi_x64.exe"))) {
    Write-Host "Could not find Kenshi automatically." -ForegroundColor Yellow
    $kenshi = Read-Host "Paste your Kenshi folder path (the one with kenshi_x64.exe)"
}
Write-Host "Kenshi: $kenshi"

# 2. RE_Kenshi is the plugin loader - the mod does nothing without it.
if (-not (Test-Path (Join-Path $kenshi "RE_Kenshi.dll"))) {
    Write-Host ""
    Write-Host "!! RE_Kenshi is NOT installed - KenshiCoop needs it." -ForegroundColor Red
    Write-Host "   Get it here (install, then run me again):" -ForegroundColor Red
    Write-Host "   https://www.nexusmods.com/kenshi/mods/847" -ForegroundColor Red
    Write-Host ""
}

# 3. Install the mod.
$dst = Join-Path $kenshi "mods\KenshiCoop"
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item (Join-Path $here "KenshiCoop\*") $dst -Force
Write-Host "Mod installed -> $dst"

# 4. Enable it in Kenshi's mod list (idempotent; preserves the existing list).
$cfg = Join-Path $kenshi "data\mods.cfg"
$entry = "KenshiCoop.mod"
$lines = @()
if (Test-Path $cfg) { $lines = @(Get-Content $cfg | Where-Object { $_ -ne "" }) }
if ($lines -notcontains $entry) {
    $lines += $entry
    Set-Content -Path $cfg -Value $lines -Encoding ASCII
    Write-Host "Mod enabled in data\mods.cfg"
} else {
    Write-Host "Mod already enabled in data\mods.cfg"
}

# 5. Tailscale: install if missing.
$ts = "C:\Program Files\Tailscale\tailscale.exe"
if (-not (Test-Path $ts)) {
    Write-Host "Installing Tailscale ..."
    $ok = $false
    try {
        winget install --id Tailscale.Tailscale --silent --accept-package-agreements --accept-source-agreements --disable-interactivity
        if (Test-Path $ts) { $ok = $true }
    } catch {}
    if (-not $ok) {
        Write-Host "  (winget unavailable - downloading the installer)"
        $msi = Join-Path $env:TEMP "tailscale-setup.msi"
        Invoke-WebRequest -Uri "https://pkgs.tailscale.com/stable/tailscale-setup-latest-amd64.msi" -OutFile $msi
        Start-Process msiexec.exe -ArgumentList "/i", "`"$msi`"", "/qn" -Wait
    }
    if (-not (Test-Path $ts)) { throw "Tailscale install failed - install it manually from tailscale.com, then run me again." }
    Write-Host "Tailscale installed."
} else {
    Write-Host "Tailscale already installed."
}

# 6. Join the host's network with the baked-in key (no account needed). If this
#    machine is ALREADY on a tailscale network, leave it alone.
$status = & $ts status 2>&1 | Out-String
if ($status -match "Logged out|NeedsLogin") {
    Write-Host "Joining the game network ..."
    & $ts up --auth-key=__AUTHKEY__ --unattended
    if ($LASTEXITCODE -ne 0) { throw "tailscale up failed - the key may have expired; ask for a fresh kit." }
    Write-Host "Joined."
} else {
    Write-Host "Already on a tailscale network - leaving it as is." -ForegroundColor Yellow
    Write-Host "(If joining in-game finds no host, ask to be invited to the host's network.)"
}

Write-Host ""
Write-Host "=== DONE ===" -ForegroundColor Green
Write-Host "1. Launch Kenshi."
Write-Host "2. Click JOIN GAME in the KenshiCoop window (top right of the menu)."
Write-Host "That's it - it finds the host and connects by itself."
'@
$setup = $setup.Replace('__AUTHKEY__', $AuthKey)
Set-Content -Path (Join-Path $kit "SETUP.ps1") -Value $setup -Encoding ASCII

# ---- README.txt ----------------------------------------------------------------
@'
KenshiCoop - friend setup
=========================

1. Install RE_Kenshi first if you don't have it (one-time):
   https://www.nexusmods.com/kenshi/mods/847

2. Double-click SETUP.cmd. Click "Yes" on the prompt. Wait for DONE.

3. Launch Kenshi. Click JOIN GAME in the KenshiCoop window (top right
   of the main menu). It finds the host and connects by itself.

To play again later: just do step 3.
'@ | Set-Content -Path (Join-Path $kit "README.txt") -Encoding ASCII

$zip = Join-Path $repoRoot "dist\KenshiCoop-friend-kit.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $kit "*") -DestinationPath $zip
$dllHash = (Get-FileHash -Algorithm SHA256 $dll).Hash.Substring(0, 16)
Write-Host ""
Write-Host "friend kit: $zip"
Write-Host "  dll sha256[0..16]: $dllHash  (protocol-matched to your build)"
Write-Host ""
Write-Host "REMEMBER: the zip embeds your tailscale auth key - private channel"
Write-Host "only, and revoke the key at login.tailscale.com/admin/settings/keys"
Write-Host "once your friends are in (their machines stay joined)."
