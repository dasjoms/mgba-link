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
$relayDir = Join-Path $repoRoot 'server/relay'
$outDir = Join-Path $repoRoot 'out/relay'
$outExe = Join-Path $outDir 'relay.exe'

if (-not (Test-Path -Path $relayDir -PathType Container)) {
    Fail-Step "Relay source folder not found at '$relayDir'. Run this script from the repository checkout."
}

$goCmd = Get-Command go -ErrorAction SilentlyContinue
if (-not $goCmd) {
    Write-Host 'Go was not found in PATH.' -ForegroundColor Yellow
    Write-Host 'Next steps:' -ForegroundColor Yellow
    Write-Host '  1. Install Go (recommended): winget install --id GoLang.Go -e --source winget' -ForegroundColor Yellow
    Write-Host '  2. Re-open PowerShell so PATH is refreshed.' -ForegroundColor Yellow
    Write-Host '  3. Re-run this script.' -ForegroundColor Yellow
    Fail-Step 'Missing required tool: go'
}

Invoke-Step -Description 'Validate Go installation (go version)' -Action {
    & $goCmd.Source version
}

Invoke-Step -Description "Create relay output directory '$outDir'" -Action {
    New-Item -Path $outDir -ItemType Directory -Force | Out-Null
}

if ($SkipBuild) {
    Write-Host "SkipBuild enabled. Relay build skipped. Expected output binary path: $outExe" -ForegroundColor Cyan
    exit 0
}

Invoke-Step -Description "Build relay binary to '$outExe'" -Action {
    Push-Location $relayDir
    try {
        & $goCmd.Source build -o $outExe .
    }
    finally {
        Pop-Location
    }
}

if (-not (Test-Path -Path $outExe -PathType Leaf)) {
    Fail-Step "Relay build reported success but '$outExe' was not created."
}

Write-Host "Relay bootstrap complete. Binary: $outExe" -ForegroundColor Green
