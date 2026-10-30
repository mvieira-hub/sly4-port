# Runs the game in RPCS3 (window, no GUI) and presses keys at given seconds, to capture
# reference frames for the comparison with sly4 (docs/GPU.md, 5e). F12 is RPCS3's
# screenshot key (screenshots\BCES01284 in the RPCS3 folder); the others reach the game's
# controller when player 1 is the keyboard (config\input_configs\global\Default.yml:
# Return = Start, X = Cross, C = Circle, arrows = D-pad).
#
#   .\tools\rpcs3_capture.ps1 -Keys "X@32,F12@38,F12@46,RETURN@58,F12@62" -Seconds 85
#
# Each key is held for 150 ms: RPCS3 reads the keyboard once a millisecond, and SendKeys'
# instant press and release were never seen.
param(
    [Parameter(Mandatory = $true)][string]$Keys,
    [int]$Seconds = 90,
    # RPCS3 folder: -Rpcs3, else RPCS3_HOME, else the first rpcs3-* folder in Documents
    [string]$Rpcs3 = $(if ($env:RPCS3_HOME) { $env:RPCS3_HOME } else {
        (Get-ChildItem (Join-Path $HOME 'Documents') -Directory -Filter 'rpcs3-*' -ErrorAction SilentlyContinue |
            Select-Object -First 1).FullName })
)
if (-not $Rpcs3 -or -not (Test-Path "$Rpcs3\rpcs3.exe")) { throw "rpcs3.exe not found: pass -Rpcs3 or set RPCS3_HOME" }
$game = Join-Path (Split-Path $PSScriptRoot) "PS3_GAME\USRDIR\EBOOT.BIN"
Add-Type -Namespace Sly4 -Name Keys -MemberDefinition @'
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
'@
$vk = @{ F12 = 0x7B; RETURN = 0x0D; X = 0x58; C = 0x43; Z = 0x5A; V = 0x56; SPACE = 0x20
         UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27 }
$shell = New-Object -ComObject WScript.Shell
$p = Start-Process -FilePath "$Rpcs3\rpcs3.exe" -ArgumentList @("--no-gui", "`"$game`"") -WorkingDirectory $Rpcs3 -PassThru
$t0 = Get-Date
$events = foreach ($item in $Keys.Split(',')) {
    $k, $s = $item.Split('@')
    [pscustomobject]@{ Key = $k.ToUpper(); At = [double]$s }
}
foreach ($e in ($events | Sort-Object At)) {
    while (((Get-Date) - $t0).TotalSeconds -lt $e.At) { Start-Sleep -Milliseconds 100 }
    if (-not $vk.ContainsKey($e.Key)) { Write-Warning "tecla desconhecida: $($e.Key)"; continue }
    [void]$shell.AppActivate($p.Id)
    Start-Sleep -Milliseconds 150
    $code = [byte]$vk[$e.Key]
    $scan = [byte][Sly4.Keys]::MapVirtualKey($code, 0)
    [Sly4.Keys]::keybd_event($code, $scan, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    [Sly4.Keys]::keybd_event($code, $scan, 2, [UIntPtr]::Zero)
    "{0,6:N1}s {1}" -f ((Get-Date) - $t0).TotalSeconds, $e.Key
}
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 200 }
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
