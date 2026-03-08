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
$runtimeDir = Join-Path $outDir 'runtime'
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
    "$mingwPrefix-ninja",
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
# Ensure generator/config settings don't get stuck from previous failed runs.
rm -f CMakeCache.txt
rm -rf CMakeFiles

# Build with full feature set, including Lua scripting support.
# Disable LTO for MSYS2 bootstrap builds to avoid known GCC 15 ICE failures
# (internal compiler errors) seen in Qt sources with -flto.
cmake "__REPO_ROOT__" -G "Ninja" -DUSE_LUA=5.4 -DBUILD_LTO=OFF
cmake --build . --parallel "$(nproc)"
'@

$buildScript = $buildScriptTemplate.Replace('__REPO_ROOT__', $repoRootPosix)

Invoke-Step -Description "Configure and build mGBA Qt in '$buildDir'" -Action {
    & $msysShell -lc $buildScript
}

$qtBinaryCandidates = @('mGBA.exe', 'mgba-qt.exe')
$resolvedQtBinaryName = $null
$qtBinary = $null
foreach ($candidate in $qtBinaryCandidates) {
    $candidatePath = Join-Path $buildDir $candidate
    if (Test-Path -Path $candidatePath -PathType Leaf) {
        $resolvedQtBinaryName = $candidate
        $qtBinary = $candidatePath
        break
    }
}

if (-not $qtBinary) {
    Write-Host "Build completed but no Qt executable was found in '$buildDir'." -ForegroundColor Yellow
    Write-Host "Checked for: $($qtBinaryCandidates -join ', ')" -ForegroundColor Yellow
    Write-Host 'Check CMake output for disabled Qt frontend or build errors.' -ForegroundColor Yellow
    Fail-Step 'mGBA Qt build artifact missing'
}

Invoke-Step -Description "Prepare runtime bundle in '$runtimeDir'" -Action {
    Remove-Item -Path $runtimeDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -Path $runtimeDir -ItemType Directory -Force | Out-Null

    $runtimeQtBinary = Join-Path $runtimeDir $resolvedQtBinaryName
    Copy-Item -Path $qtBinary -Destination $runtimeQtBinary -Force

    $runtimeDirPosix = (& $msysShell -lc "cygpath -u '$runtimeDir'").Trim()
    if (-not $runtimeDirPosix) {
        Fail-Step 'Unable to convert runtime directory path to MSYS2 format (cygpath failed).'
    }

    $bundleScriptTemplate = @'
set -euo pipefail
export PATH="/mingw64/bin:$PATH"
cd "__RUNTIME_DIR__"

deploy_tool=""
if command -v windeployqt-qt5 >/dev/null 2>&1; then
  deploy_tool="windeployqt-qt5"
elif command -v windeployqt >/dev/null 2>&1; then
  deploy_tool="windeployqt"
fi

if [[ -n "$deploy_tool" ]]; then
  "$deploy_tool" --release --no-translations --no-compiler-runtime ./__QT_BINARY_NAME__
else
  echo "warning: windeployqt not found; falling back to ntldd for direct DLL dependencies" >&2
fi

ntldd -R ./__QT_BINARY_NAME__ > ./ntldd-mgba-qt.txt

awk '
  /=> \/mingw64\// { print $3 }
  /^\/mingw64\// { print $1 }
' ./ntldd-mgba-qt.txt | sort -u | while read -r dep; do
  if [[ -f "$dep" ]]; then
    cp -f "$dep" .
  fi
done
'@

    $bundleScript = $bundleScriptTemplate.Replace('__RUNTIME_DIR__', $runtimeDirPosix).Replace('__QT_BINARY_NAME__', $resolvedQtBinaryName)
    & $msysShell -lc $bundleScript
}

Invoke-Step -Description 'Run mgba-qt runtime smoke test with plugin diagnostics' -Action {
    $runtimeBinary = Join-Path $runtimeDir $resolvedQtBinaryName
    if (-not (Test-Path -Path $runtimeBinary -PathType Leaf)) {
        Fail-Step "Runtime smoke test could not find '$runtimeBinary'."
    }

    $logFile = Join-Path $runtimeDir 'startup-log.txt'
    if (Test-Path -Path $logFile) {
        Remove-Item -Path $logFile -Force
    }

    $env:QT_DEBUG_PLUGINS = '1'
    & $runtimeBinary --version *>&1 | Tee-Object -FilePath $logFile
    $env:QT_DEBUG_PLUGINS = $null

    if ($LASTEXITCODE -ne 0) {
        Fail-Step "mgba-qt smoke test failed. Inspect '$logFile' and '$runtimeDir/ntldd-mgba-qt.txt'."
    }
}

Write-Host "Client bootstrap complete. Qt binary: $qtBinary" -ForegroundColor Green
Write-Host "Resolved runtime executable: $(Join-Path $runtimeDir $resolvedQtBinaryName)" -ForegroundColor Green
Write-Host "Runtime bundle: $runtimeDir" -ForegroundColor Green
Write-Host "Dependency report: $(Join-Path $runtimeDir 'ntldd-mgba-qt.txt')" -ForegroundColor Green
Write-Host "Startup diagnostics: $(Join-Path $runtimeDir 'startup-log.txt')" -ForegroundColor Green
