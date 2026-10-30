# Runs Ghidra headless (analyzeHeadless) in Docker with JDK 21.
#   .\tools\run_ghidra.ps1 import              # import + analyse EBOOT.elf (Sly4ApplyMap as pre-script)
#   .\tools\run_ghidra.ps1 export <tokens...>  # Sly4Export on the analysed program
# The Ghidra install is mounted read-only; a copy of ppc_64_32.cspec with r2
# marked "unaffected" (the PS3 TOC register) is mounted over the original.
# The project lives in work\ghidra_proj and can also be opened in the Ghidra GUI.
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('import', 'export')][string]$Mode,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Tokens
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ghidra = if ($env:GHIDRA_HOME) { $env:GHIDRA_HOME } else { Join-Path $HOME 'Documents\ghidra_12.1.4_PUBLIC' }
if (-not (Test-Path "$ghidra\support\analyzeHeadless.bat")) { throw "Ghidra not found in ${ghidra}: set GHIDRA_HOME" }
$cspecRel = 'Ghidra\Processors\PowerPC\data\languages\ppc_64_32.cspec'
$patched = "$root\work\ghidra\ppc_64_32.cspec"

New-Item -ItemType Directory -Force "$root\work\ghidra", "$root\work\ghidra_proj" | Out-Null
if (-not (Test-Path $patched)) {
    $text = Get-Content -Raw "$ghidra\$cspecRel"
    if ($text -notmatch '<unaffected>') { throw "bloco <unaffected> nao encontrado em $cspecRel" }
    $text = $text -replace '<unaffected>', "<unaffected>`n      <register name=`"r2`"/>"
    [IO.File]::WriteAllText($patched, $text)
}

$cspecMount = '/ghidra/' + ($cspecRel -replace '\\', '/')
$common = @(
    'run', '--rm',
    '-e', 'GHIDRA_HEADLESS_MAXMEM=8G',
    '-v', "${ghidra}:/ghidra:ro",
    '-v', "${patched}:${cspecMount}:ro",
    '-v', "${root}\PS3_GAME:/game:ro",
    '-v', "${root}\tools\ghidra:/scripts:ro",
    '-v', "${root}\work:/work",
    'eclipse-temurin:21-jdk',
    '/ghidra/support/analyzeHeadless', '/work/ghidra_proj', 'Sly4',
    '-scriptPath', '/scripts'
)
# Java writes normal output to stderr; in PowerShell 5.1 that must not stop the script.
$ErrorActionPreference = 'Continue'
if ($Mode -eq 'import') {
    docker @common -import /game/USRDIR/EBOOT.elf -overwrite `
        -processor 'PowerPC:BE:64:A2ALT-32addr' -cspec default `
        -preScript Sly4ApplyMap.java /work/eboot `
        -log /work/ghidra/import.log
} else {
    docker @common -process EBOOT.elf -noanalysis `
        -postScript Sly4Export.java /work/ghidra/export @Tokens `
        -log /work/ghidra/export.log
}
exit $LASTEXITCODE
