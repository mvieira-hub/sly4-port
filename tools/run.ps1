# Runs one of the Python tools inside Docker.
#   .\tools\run.ps1 psarc.py list
#   .\tools\run.ps1 texr2dds.py /work/psarc/GUI --out /work/textures
# Mounts: PS3_GAME -> /game (read-only), tools -> /tools (read-only), work -> /work.
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Script,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ScriptArgs
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$image = 'sly4-tools:1'

# Cached by Docker; only rebuilds when the Dockerfile changes.
docker build -q -t $image "$root\tools" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "docker build falhou" }
New-Item -ItemType Directory -Force "$root\work" | Out-Null
# Progress goes to stderr; in PowerShell 5.1 a redirected stderr line must not stop the script.
$ErrorActionPreference = 'Continue'

docker run --rm `
    -v "${root}\PS3_GAME:/game:ro" `
    -v "${root}\tools:/tools:ro" `
    -v "${root}\work:/work" `
    $image "/tools/$Script" @ScriptArgs
exit $LASTEXITCODE
