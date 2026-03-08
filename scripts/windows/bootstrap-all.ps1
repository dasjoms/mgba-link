[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
$relayScript = Join-Path $scriptDir 'bootstrap-relay.ps1'
$clientScript = Join-Path $scriptDir 'bootstrap-client-msys2.ps1'

foreach ($scriptPath in @($relayScript, $clientScript)) {
    if (-not (Test-Path -Path $scriptPath -PathType Leaf)) {
        Write-Error "Required script not found: $scriptPath"
        exit 1
    }
}

$commonArgs = @{
    ErrorAction = 'Stop'
    SkipBuild = $SkipBuild
    WhatIf = [bool]$WhatIfPreference
}

if ($PSCmdlet.ShouldProcess('bootstrap-relay.ps1', 'Run relay bootstrap')) {
    & $relayScript @commonArgs
    if (-not $?) {
        Write-Error 'Relay bootstrap failed. See output above for next steps.'
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        Write-Error 'Relay bootstrap failed. See output above for next steps.'
        exit 1
    }
}

if ($PSCmdlet.ShouldProcess('bootstrap-client-msys2.ps1', 'Run client bootstrap')) {
    & $clientScript @commonArgs
    if (-not $?) {
        Write-Error 'Client bootstrap failed. See output above for next steps.'
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        Write-Error 'Client bootstrap failed. See output above for next steps.'
        exit 1
    }
}

Write-Host 'Bootstrap complete. Outputs are under out/relay and out/mgba.' -ForegroundColor Green
