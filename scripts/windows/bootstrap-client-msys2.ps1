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
    "$mingwPrefix-angleproject",
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

    $buildRuntimeDlls = Get-ChildItem -Path $buildDir -Filter '*.dll' -File -ErrorAction SilentlyContinue
    foreach ($buildRuntimeDll in $buildRuntimeDlls) {
        Copy-Item -Path $buildRuntimeDll.FullName -Destination (Join-Path $runtimeDir $buildRuntimeDll.Name) -Force
    }

    $runtimeLibmgba = Join-Path $runtimeDir 'libmgba.dll'
    if (-not (Test-Path -Path $runtimeLibmgba -PathType Leaf)) {
        Fail-Step "Critical missing runtime DLL: expected '$runtimeLibmgba'. Ensure libmgba.dll is produced in '$buildDir'."
    }

    $runtimeDirPosix = (& $msysShell -lc 'cygpath -u "$1"' -- $runtimeDir).Trim()
    if (-not $runtimeDirPosix) {
        Fail-Step 'Unable to convert runtime directory path to MSYS2 format (cygpath failed).'
    }

    $bundleScriptPath = [System.IO.Path]::ChangeExtension([System.IO.Path]::GetTempFileName(), '.sh')
    $bundleScriptLines = @(
        'set -euo pipefail'
        'export PATH="/mingw64/bin:$PATH"'
        'cd "$1"'
        'exe="$2"'
        ''
        'deploy_tool=""'
        'if command -v windeployqt-qt5 >/dev/null 2>&1; then'
        '  deploy_tool="windeployqt-qt5"'
        'elif command -v windeployqt >/dev/null 2>&1; then'
        '  deploy_tool="windeployqt"'
        'fi'
        ''
        'if [[ -n "$deploy_tool" ]]; then'
        '  "$deploy_tool" --release --no-translations --no-compiler-runtime "./$exe"'
        'else'
        '  echo "warning: windeployqt not found; falling back to ntldd for direct DLL dependencies" >&2'
        'fi'
        ''
        'ntldd -R "./$exe" > ./ntldd-mgba-qt.txt'
        'cp -f ./ntldd-mgba-qt.txt ./ntldd-runtime-scan.txt'
        'unresolved_count=0'
        'noncritical_api_set_count=0'
        'noncritical_missing_dlls=()'
        'critical_missing_dlls=()'
        'while IFS= read -r unresolved; do'
        '  [[ -z "$unresolved" ]] && continue'
        '  dep_name="$(awk "{print \$1}" <<< "$unresolved" | tr -d "\r")"'
        '  [[ -z "$dep_name" ]] && continue'
        '  dep_lower="${dep_name,,}"'
        '  if [[ "$dep_lower" == ext-ms-* || "$dep_lower" == api-ms-* || "$dep_lower" == api-ms-win-* ]]; then'
        '    noncritical_missing_dlls+=("$dep_name")'
        '    noncritical_api_set_count=$((noncritical_api_set_count + 1))'
        '    continue'
        '  fi'
        '  # Some Windows components appear as delay-load entries in ntldd but are optional on many machines.'
        '  if [[ "$dep_lower" == "pdmutilities.dll" || "$dep_lower" == "hvsifiletrust.dll" ]]; then'
        '    noncritical_missing_dlls+=("$dep_name")'
        '    continue'
        '  fi'
        '  echo "error: critical missing runtime DLL from ntldd: $dep_name" >&2'
        '  critical_missing_dlls+=("$dep_name")'
        '  unresolved_count=$((unresolved_count + 1))'
        'done < <(grep "not found" ./ntldd-mgba-qt.txt || true)'
        'printf "%s\n" "${noncritical_missing_dlls[@]:-}" | sed "/^$/d" | sort -u > ./ntldd-noncritical-missing.txt || true'
        'printf "%s\n" "${critical_missing_dlls[@]:-}" | sed "/^$/d" | sort -u > ./ntldd-critical-missing.txt || true'
        ''
        'if (( unresolved_count > 0 )); then'
        '  echo "critical missing runtime DLLs detected ($unresolved_count). Review ./ntldd-critical-missing.txt and ./ntldd-mgba-qt.txt." >&2'
        '  exit 1'
        'fi'
        ''
        'if (( noncritical_api_set_count > 0 )); then'
        '  echo "non-critical unresolved API-set entries detected ($noncritical_api_set_count); see ./ntldd-noncritical-missing.txt." >&2'
        'fi'
        ''
        "awk '"
        '  /=> \/mingw64\// { print $3 }'
        '  /^\/mingw64\// { print $1 }'
        "' ./ntldd-runtime-scan.txt | sort -u | while read -r dep; do"
        '  if [[ -f "$dep" ]]; then'
        '    cp -f "$dep" .'
        '  fi'
        'done'
        ''
        'echo "Scanning bundled runtime DLLs for unresolved dependencies..." >&2'
        'while IFS= read -r runtimeDll; do'
        '  [[ -z "$runtimeDll" ]] && continue'
        '  echo >> ./ntldd-runtime-scan.txt'
        '  echo "### SCAN: $runtimeDll" >> ./ntldd-runtime-scan.txt'
        '  ntldd -R "$runtimeDll" >> ./ntldd-runtime-scan.txt || true'
        'done < <(find . -type f -name "*.dll" | sort)'
        ''
        'scan_unresolved_count=0'
        'scan_critical_missing_dlls=()'
        'while IFS= read -r unresolved; do'
        '  [[ -z "$unresolved" ]] && continue'
        '  dep_name="$(awk "{print \\$1}" <<< "$unresolved" | tr -d "\r")"'
        '  [[ -z "$dep_name" ]] && continue'
        '  dep_lower="${dep_name,,}"'
        '  if [[ "$dep_lower" == ext-ms-* || "$dep_lower" == api-ms-* || "$dep_lower" == api-ms-win-* || "$dep_lower" == "pdmutilities.dll" || "$dep_lower" == "hvsifiletrust.dll" ]]; then'
        '    continue'
        '  fi'
        '  scan_critical_missing_dlls+=("$dep_name")'
        '  scan_unresolved_count=$((scan_unresolved_count + 1))'
        'done < <(grep "not found" ./ntldd-runtime-scan.txt || true)'
        'printf "%s\n" "${scan_critical_missing_dlls[@]:-}" | sed "/^$/d" | sort -u > ./ntldd-runtime-critical-missing.txt || true'
        'if (( scan_unresolved_count > 0 )); then'
        '  echo "error: runtime scan found additional critical unresolved DLLs ($scan_unresolved_count). Review ./ntldd-runtime-critical-missing.txt and ./ntldd-runtime-scan.txt." >&2'
        '  exit 1'
        'fi'
    )
    $bundleScript = ($bundleScriptLines -join "`n")

    try {
        [System.IO.File]::WriteAllText($bundleScriptPath, $bundleScript, (New-Object System.Text.UTF8Encoding($false)))

        $prefixBytes = [System.IO.File]::ReadAllBytes($bundleScriptPath) | Select-Object -First 3
        if ($prefixBytes.Count -ge 3 -and $prefixBytes[0] -eq 0xEF -and $prefixBytes[1] -eq 0xBB -and $prefixBytes[2] -eq 0xBF) {
            Fail-Step "Temporary bundle script '$bundleScriptPath' was written with a UTF-8 BOM (EF BB BF), but MSYS2 bash requires UTF-8 without BOM."
        }

        $bundleScriptPathPosix = (& $msysShell -lc 'cygpath -u "$1"' -- $bundleScriptPath).Trim()
        if (-not $bundleScriptPathPosix) {
            Fail-Step 'Unable to convert temporary bundle script path to MSYS2 format (cygpath failed).'
        }

        & $msysShell -lc 'bash "$1" "$2" "$3"' -- $bundleScriptPathPosix $runtimeDirPosix $resolvedQtBinaryName
    }
    finally {
        Remove-Item -Path $bundleScriptPath -Force -ErrorAction SilentlyContinue
    }
}

Invoke-Step -Description 'Run mgba-qt runtime smoke test with plugin diagnostics' -Action {
    $runtimeBinary = Join-Path $runtimeDir $resolvedQtBinaryName
    if (-not (Test-Path -Path $runtimeBinary -PathType Leaf)) {
        Fail-Step "Runtime smoke test could not find '$runtimeBinary'."
    }

    $logFile = Join-Path $runtimeDir 'startup-log.txt'
    $null = New-Item -Path $logFile -ItemType File -Force

    $env:QT_DEBUG_PLUGINS = '1'
    try {
        Add-Content -Path $logFile -Value ("[{0}] Smoke test starting" -f (Get-Date -Format o))
        Add-Content -Path $logFile -Value ("Runtime binary: {0}" -f $runtimeBinary)
        Add-Content -Path $logFile -Value ("Command: {0} --version" -f $runtimeBinary)

        $exitCode = $null
        try {
            & $runtimeBinary '--version' *>> $logFile
            $exitCode = $LASTEXITCODE
        }
        catch {
            $_ | Out-String | Add-Content -Path $logFile
            Fail-Step "mgba-qt smoke test could not launch '$runtimeBinary'."
        }

        if ($null -eq $exitCode) {
            $exitCode = 1
        }

        Add-Content -Path $logFile -Value ("Exit code: {0}" -f $exitCode)
        Add-Content -Path $logFile -Value ("[{0}] Smoke test finished" -f (Get-Date -Format o))

        if ($exitCode -ne 0) {
            $inspectHint = ''
            if (Test-Path -Path $logFile -PathType Leaf) {
                $inspectHint = " Inspect '$logFile'."
            }
            Fail-Step "mgba-qt smoke test failed with exit code $exitCode.$inspectHint Inspect '$runtimeDir/ntldd-mgba-qt.txt' and '$runtimeDir/ntldd-runtime-critical-missing.txt'."
        }
    }
    finally {
        $env:QT_DEBUG_PLUGINS = $null
    }

    if (Test-Path -Path $logFile -PathType Leaf) {
        $logSizeBytes = (Get-Item -Path $logFile).Length
        Write-Host "Smoke-test startup log: $logFile ($logSizeBytes bytes)" -ForegroundColor Cyan
    }
    else {
        Write-Host "Smoke-test startup log was not created at expected path: $logFile" -ForegroundColor Yellow
    }
}

Write-Host "Client bootstrap complete. Qt binary: $qtBinary" -ForegroundColor Green
Write-Host "Resolved runtime executable: $(Join-Path $runtimeDir $resolvedQtBinaryName)" -ForegroundColor Green
Write-Host "Runtime bundle: $runtimeDir" -ForegroundColor Green
Write-Host "Dependency report (exe): $(Join-Path $runtimeDir 'ntldd-mgba-qt.txt')" -ForegroundColor Green
Write-Host "Dependency report (runtime scan): $(Join-Path $runtimeDir 'ntldd-runtime-scan.txt')" -ForegroundColor Green
Write-Host "Critical missing DLL report: $(Join-Path $runtimeDir 'ntldd-runtime-critical-missing.txt')" -ForegroundColor Green
Write-Host "Startup diagnostics: $(Join-Path $runtimeDir 'startup-log.txt')" -ForegroundColor Green
