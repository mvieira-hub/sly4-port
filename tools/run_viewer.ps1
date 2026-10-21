# Serves tools\viewer, work\models and work\models_all on http://localhost:8765 (Docker, read-only).
#   .\tools\run_viewer.ps1          # starts in the background (container "sly4-viewer")
#   .\tools\run_viewer.ps1 -Stop
# Open: http://localhost:8765/viewer/?model=models/<pasta>/<arquivo>.gltf
param([switch]$Stop, [int]$Port = 8765)
$root = Split-Path -Parent $PSScriptRoot
$ErrorActionPreference = 'Continue'
docker rm -f sly4-viewer 2>$null | Out-Null
if ($Stop) { exit 0 }
docker run -d --name sly4-viewer -p "127.0.0.1:${Port}:8000" `
    -v "${root}\tools\viewer:/srv/viewer:ro" `
    -v "${root}\work\models:/srv/models:ro" `
    -v "${root}\work\models_all:/srv/models_all:ro" `
    --entrypoint python sly4-tools:1 -m http.server 8000 --directory /srv | Out-Null
"http://localhost:$Port/viewer/"
