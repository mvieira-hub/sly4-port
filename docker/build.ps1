# Builds the native port (sly4.exe) inside the MinGW-w64 toolchain container.
#   .\docker\build.ps1                 # Release -> build\mingw-release\sly4.exe
#   .\docker\build.ps1 -Config Debug   # Debug   -> build\mingw-debug\sly4.exe
#   .\docker\build.ps1 -Clean          # wipes the build folder first
# The toolchain image (sly4-mingw:1) is built on first use and cached by Docker.
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$image = 'sly4-mingw:1'
$dir = "build/mingw-$($Config.ToLower())"

docker build -q -t $image -f "$root\docker\toolchain\Dockerfile" $root | Out-Null
if ($LASTEXITCODE -ne 0) { throw "docker build da toolchain falhou" }
if ($Clean -and (Test-Path "$root\$dir")) { Remove-Item -Recurse -Force "$root\$dir" }

# Compiler output goes to stderr; in PowerShell 5.1 that must not stop the script.
$ErrorActionPreference = 'Continue'
docker run --rm -v "${root}:/src" -w /src $image sh -c (
    "cmake -S . -B $dir -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/cmake/mingw-w64-x86_64.cmake " +
    "-DCMAKE_BUILD_TYPE=$Config && cmake --build $dir")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
"$root\$($dir -replace '/', '\')\sly4.exe"
