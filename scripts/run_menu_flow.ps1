<#
.SYNOPSIS
  ONE-CLICK menu-flow rehearsal (protocol 49): three INTERACTIVE instances -
  no scenario runner, no self-exit timer - driven purely through the title-
  screen launcher's entry point via KENSHICOOP_UI_AUTO (synthetic mouse input
  never reaches Kenshi, so the hook presses HOST GAME / JOIN GAME for us).

  The flow under test is exactly what three friends do:
    host:  click HOST GAME (goes ONLINE over UDP), load a save
           (KENSHICOOP_SAVE auto-loads squad3 here to stand in for the human)
    join1: click JOIN GAME - scan finds the host, auto-claims squad slot 1,
           connects, receives/loads the world. Nothing else.
    join2: click JOIN GAME - the advert now says 2 players are in, so it
           auto-claims squad slot 2 and connects. Nothing else.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_menu_flow.ps1
#>
[CmdletBinding()]
param(
    [string]$Save = "squad3",
    [string]$HostDir  = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir  = "$env:USERPROFILE\Kenshi-Join",
    [string]$Join2Dir = "$env:USERPROFILE\Kenshi-Join2",
    [int]$StartTimeoutSec = 120,
    [int]$SettleSec = 45,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "out\menu_flow\$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$hostLog  = Join-Path $OutDir "host.log"
$join1Log = Join-Path $OutDir "join1.log"
$join2Log = Join-Path $OutDir "join2.log"

foreach ($d in @($HostDir, $JoinDir, $Join2Dir)) {
    if (-not (Test-Path (Join-Path $d "mods\KenshiCoop\KenshiCoop.dll"))) { throw "KenshiCoop not deployed in $d" }
}
$stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "deploy_saves.ps1") -Save $Save
if ($LASTEXITCODE -ne 0) { throw "deploy_saves.ps1 failed for '$Save'" }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "set_video_mode.ps1") `
    -Width 1280 -Height 1024 -HostDir $HostDir -JoinDir $JoinDir
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "set_video_mode.ps1") `
    -Width 1280 -Height 1024 -HostDir $Join2Dir -JoinDir $Join2Dir

function Wait-ForLogLine {
    param([string]$File, [string]$Pattern, [int]$TimeoutSec)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $File) {
            $hit = Select-String -Path $File -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $hit) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# INTERACTIVE env: no scenario, no self-exit - only the UI_AUTO press, the log
# path, and (host only) the auto-load save standing in for the human's Load.
function Set-FlowEnv {
    param([string]$UiAuto, [string]$Log, [string]$AutoSave)
    $env:KENSHICOOP_MODE           = ""
    $env:KENSHICOOP_TRANSPORT      = ""
    $env:KENSHICOOP_SCENARIO       = ""
    $env:KENSHICOOP_SETUP          = ""
    $env:KENSHICOOP_TEST_SECONDS   = ""
    $env:KENSHICOOP_SAVE           = $AutoSave
    $env:KENSHICOOP_LOG            = $Log
    $env:KENSHICOOP_UI_AUTO        = $UiAuto
    $env:KENSHICOOP_OWN_RANK_CLAIM = ""
    $env:KENSHICOOP_OWN_SQUAD      = ""
    $env:KENSHICOOP_OWN_RANK       = ""
    $env:KENSHICOOP_DISC           = "1"
    $env:KENSHICOOP_DISC_AUTOSCAN  = "0"
}

function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

Write-Host "== one-click menu flow rehearsal: save=$Save =="
Write-Host "Launching HOST (UI_AUTO=host, auto-load $Save) ..."
Set-FlowEnv -UiAuto "host" -Log $hostLog -AutoSave $Save
$hostPid = Start-PastLauncher -Exe (Join-Path $HostDir "kenshi_x64.exe") -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }

Write-Host "Waiting for the one-click HOST press + hosting + responder ..."
if (-not (Wait-ForLogLine -File $hostLog -Pattern "one-click HOST" -TimeoutSec $StartTimeoutSec)) { Write-Warning "host never pressed HOST GAME" }
[void](Wait-ForLogLine -File $hostLog -Pattern "\[disc\] responder up" -TimeoutSec 30)
[void](Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec)

Write-Host "Launching JOIN 1 (UI_AUTO=join, nothing else) ..."
Set-FlowEnv -UiAuto "join" -Log $join1Log -AutoSave ""
$join1Pid = Start-PastLauncher -Exe (Join-Path $JoinDir "kenshi_x64.exe") -WorkDir $JoinDir
if ($join1Pid -eq 0) { Write-Warning "Join 1 failed to get past the launcher." }

Write-Host "Waiting for join1 auto-join + admit + gameplay ..."
[void](Wait-ForLogLine -File $join1Log -Pattern "\[disc\] auto-join" -TimeoutSec $StartTimeoutSec)
[void](Wait-ForLogLine -File $hostLog  -Pattern "peer connected id=1" -TimeoutSec 60)
[void](Wait-ForLogLine -File $join1Log -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec)

Write-Host "Launching JOIN 2 (UI_AUTO=join, nothing else) ..."
Set-FlowEnv -UiAuto "join" -Log $join2Log -AutoSave ""
$join2Pid = Start-PastLauncher -Exe (Join-Path $Join2Dir "kenshi_x64.exe") -WorkDir $Join2Dir
if ($join2Pid -eq 0) { Write-Warning "Join 2 failed to get past the launcher." }

Write-Host "Waiting for join2 auto-join (slot 2) + admit ..."
[void](Wait-ForLogLine -File $join2Log -Pattern "\[disc\] auto-join" -TimeoutSec $StartTimeoutSec)
[void](Wait-ForLogLine -File $hostLog  -Pattern "peer connected id=2" -TimeoutSec 60)
[void](Wait-ForLogLine -File $join2Log -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec)

Write-Host "Letting the session settle ${SettleSec}s ..."
Start-Sleep -Seconds $SettleSec

Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

# ---- Judge -------------------------------------------------------------------
$fails = New-Object System.Collections.Generic.List[string]
$notes = New-Object System.Collections.Generic.List[string]
function Gate {
    param([string]$Name, [string]$File, [string]$Pattern, [bool]$MustExist = $true)
    $hit = $null
    if (Test-Path $File) {
        $hit = Select-String -Path $File -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    $found = ($null -ne $hit)
    if ($found -eq $MustExist) {
        Write-Host ("  ok   {0}" -f $Name)
        if ($found) { $notes.Add("$Name :: $($hit.Line.Trim())") }
    } else {
        Write-Host ("  FAIL {0}" -f $Name)
        $fails.Add($Name)
    }
}

Write-Host "`n== one-click flow gates =="
Gate "host pressed HOST GAME"          $hostLog  "menu: one-click HOST \(udp\)"
Gate "host went online"                $hostLog  "starting as HOST"
Gate "host responder up"               $hostLog  "\[disc\] responder up"
Gate "host loaded the save"            $hostLog  "gameplay started"
Gate "join1 pressed JOIN GAME"         $join1Log "UI_AUTO pressing JOIN GAME"
Gate "join1 auto-joined slot 1"        $join1Log "\[disc\] auto-join .*slot=1"
Gate "join1 reached gameplay"          $join1Log "gameplay started"
Gate "host admitted join1 rank 1"      $hostLog  "peer connected id=1 rank=1"
Gate "join2 auto-joined slot 2"        $join2Log "\[disc\] auto-join .*slot=2"
Gate "join2 reached gameplay"          $join2Log "gameplay started"
Gate "host admitted join2 rank 2"      $hostLog  "peer connected id=2 rank=2"
Gate "no slot rejection"               $hostLog  "slot .*taken|slot .*occupied" $false
foreach ($pair in @(@("host", $hostLog), @("join1", $join1Log), @("join2", $join2Log))) {
    Gate "no protocol mismatch ($($pair[0]))" $pair[1] "protocol mismatch" $false
    Gate "no CHECK FAIL ($($pair[0]))"        $pair[1] "CHECK FAIL"        $false
}

Write-Host "`n== supporting evidence =="
foreach ($n in $notes) { Write-Host "  $n" }
Write-Host "`nLogs: $OutDir"
if ($fails.Count -eq 0) { Write-Host "MENU FLOW RESULT: PASS"; exit 0 }
else { Write-Host "MENU FLOW RESULT: FAIL ($($fails.Count) gate(s): $($fails -join ', '))"; exit 1 }
