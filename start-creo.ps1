[CmdletBinding()]
param(
    [string]$CreoRoot = 'C:\Program Files\PTC\Creo 2.0',
    [string]$Session = (Join-Path $PSScriptRoot 'session')
)

$ErrorActionPreference = 'Stop'
$sessionPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Session)
$readyPath = Join-Path $sessionPath 'ready.json'
if (Test-Path -LiteralPath $readyPath) {
    $ready = Get-Content -LiteralPath $readyPath -Encoding UTF8 -Raw | ConvertFrom-Json -ErrorAction SilentlyContinue
    if ($ready.protocol -eq 2 -and $ready.pid -gt 0 -and
        (Get-Process -Id $ready.pid -ErrorAction SilentlyContinue | Where-Object ProcessName -eq xtop)) {
        Write-Output "Creo 2.0 MCP is already running in $sessionPath."
        return
    }
}

if (Get-Process -Name parametric,xtop -ErrorAction SilentlyContinue) {
    throw 'Exit the open Creo session before starting the separate Creo 2.0 MCP session.'
}

$binaryDirectory = Join-Path $CreoRoot 'Parametric\bin'
$creo = Join-Path $binaryDirectory 'parametric.exe'
$profile = Join-Path $binaryDirectory 'parametric.psf'
$template = Join-Path $CreoRoot 'Common Files\M250\templates\mmns_part_solid.prt'
$dll = Join-Path $PSScriptRoot 'bin\creo2_agent.dll'
foreach ($file in @($creo, $profile, $template, $dll)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file is missing: $file" }
}

$textPath = Join-Path $sessionPath 'text'
$modelsPath = Join-Path $sessionPath 'models'
$queuePath = Join-Path $sessionPath 'queue'
New-Item -ItemType Directory -Path @($textPath, $modelsPath, $queuePath) -Force | Out-Null
$registryPath = Join-Path $modelsPath 'protk.dat'
@(
    'NAME Creo2_MCP'
    "EXEC_FILE $dll"
    "TEXT_DIR $textPath"
    'STARTUP dll'
    'REVISION 18'
    'FAIL_TOL TRUE'
    'END'
) | Set-Content -LiteralPath $registryPath -Encoding ASCII

$previousSession = $env:CREO2_BRIDGE_SESSION
$previousTemplate = $env:CREO2_BRIDGE_TEMPLATE
try {
    $env:CREO2_BRIDGE_SESSION = $sessionPath
    $env:CREO2_BRIDGE_TEMPLATE = $template
    $started = Start-Process -FilePath $creo -ArgumentList ('"' + $profile + '"') -WorkingDirectory $modelsPath -PassThru
    Write-Output "Started Creo 2.0 (launcher PID $($started.Id)) in $sessionPath."
} finally {
    $env:CREO2_BRIDGE_SESSION = $previousSession
    $env:CREO2_BRIDGE_TEMPLATE = $previousTemplate
}
