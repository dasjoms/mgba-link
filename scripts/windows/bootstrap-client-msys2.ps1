[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Fail-Step {
    param([string]$Message)
    Write-Error $Message
    exit 1
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )

    if ($PSCmdlet.ShouldProcess($Description, 'Run')) {
        $global:LASTEXITCODE = 0
        & $Action
        $nativeExitCode = $global:LASTEXITCODE
        if (-not $?) {
            Fail-Step "Failed: $Description"
        }
        if ($null -ne $nativeExitCode -and $nativeExitCode -ne 0) {
            Fail-Step "Failed: $Description"
        }
    }
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outDir = Join-Path $repoRoot 'out/mgba'
$buildDir = Join-Path $outDir 'build'
$msysRoot = 'C:\msys64'
$msysShell = Join-Path $msysRoot 'usr\bin\bash.exe'
$mingwPrefix = 'mingw-w64-x86_64'

$requiredPackages = @(
    'base-devel',
    'git',
    "$mingwPrefix-cmake",
    "$mingwPrefix-ffmpeg",
    "$mingwPrefix-gcc",
    "$mingwPrefix-gdb",
    "$mingwPrefix-libelf",
    "$mingwPrefix-libepoxy",
    "$mingwPrefix-libzip",
    "$mingwPrefix-lua",
    "$mingwPrefix-pkgconf",
    "$mingwPrefix-qt5",
    "$mingwPrefix-SDL2",
    "$mingwPrefix-ntldd-git"
)

if (-not (Test-Path -Path $msysShell -PathType Leaf)) {
    Write-Host "MSYS2 shell was not found at '$msysShell'." -ForegroundColor Yellow
    Write-Host 'Next steps:' -ForegroundColor Yellow
    Write-Host '  1. Install MSYS2 from https://www.msys2.org/' -ForegroundColor Yellow
    Write-Host '  2. Confirm C:\msys64 exists and launch MSYS2 once to finish initialization.' -ForegroundColor Yellow
    Write-Host '  3. Re-run this script.' -ForegroundColor Yellow
    Fail-Step 'Missing required toolchain: MSYS2'
}

Invoke-Step -Description "Create mGBA output directory '$outDir'" -Action {
    New-Item -Path $outDir -ItemType Directory -Force | Out-Null
}

$pkgList = $requiredPackages -join ' '
$pkgInstallScript = "pacman -Sy --needed --noconfirm $pkgList"

Invoke-Step -Description 'Install required MSYS2 packages for mGBA Qt build' -Action {
    & $msysShell -lc $pkgInstallScript
}

if ($SkipBuild) {
    Write-Host "SkipBuild enabled. mGBA Qt build skipped. Build directory would be: $buildDir" -ForegroundColor Cyan
    exit 0
}

$repoRootPosix = (& $msysShell -lc "cygpath -u '$repoRoot'").Trim()
if (-not $repoRootPosix) {
    Fail-Step 'Unable to convert repository path to MSYS2 format (cygpath failed).'
}

$buildScriptTemplate = @'
set -euo pipefail
export PATH="/mingw64/bin:$PATH"
mkdir -p "__REPO_ROOT__/out/mgba/build"
cd "__REPO_ROOT__/out/mgba/build"
cmake "__REPO_ROOT__" -G "MSYS Makefiles"
make -j"$(nproc)"
'@

$buildScript = $buildScriptTemplate.Replace('__REPO_ROOT__', $repoRootPosix)

Invoke-Step -Description "Configure and build mGBA Qt in '$buildDir'" -Action {
    & $msysShell -lc $buildScript
}

$qtBinary = Join-Path $buildDir 'mgba-qt.exe'
if (-not (Test-Path -Path $qtBinary -PathType Leaf)) {
    Write-Host "Build completed but '$qtBinary' was not found." -ForegroundColor Yellow
    Write-Host 'Check CMake output for disabled Qt frontend or build errors.' -ForegroundColor Yellow
    Fail-Step 'mGBA Qt build artifact missing'
}

Write-Host "Client bootstrap complete. Qt binary: $qtBinary" -ForegroundColor Green
