[CmdletBinding()]
param(
    [string]$CreoRoot = 'C:\Program Files\PTC\Creo 2.0',
    [string]$VisualCpp,
    [string]$VisualCppHeaders,
    [string]$WindowsSdk,
    [string]$ToolRoot
)

$ErrorActionPreference = 'Stop'
if (-not $ToolRoot) {
    $search = (Get-Item -LiteralPath $PSScriptRoot).Parent
    while ($search) {
        foreach ($relativePath in @('build-tools\portable', 'work\build-tools\portable')) {
            $candidate = Join-Path $search.FullName $relativePath
            if (Test-Path -LiteralPath $candidate) {
                $ToolRoot = $candidate
                break
            }
        }
        if ($ToolRoot) { break }
        $search = $search.Parent
    }
}
$toolRoot = $ToolRoot
if (-not $VisualCpp) {
    $VisualCpp = 'C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC'
    if ($toolRoot) {
        $portable = Join-Path $toolRoot 'vc_stdamd64\Program Files(64)\Microsoft Visual Studio 10.0\VC'
        if (Test-Path -LiteralPath $portable) { $VisualCpp = $portable }
    }
}
if (-not $VisualCppHeaders) {
    $VisualCppHeaders = Join-Path $VisualCpp 'include'
    if ($toolRoot -and -not (Test-Path -LiteralPath $VisualCppHeaders)) {
        $VisualCppHeaders = Join-Path $toolRoot 'vc_stdx86\Program Files\Microsoft Visual Studio 10.0\VC\include'
    }
}
if (-not $WindowsSdk) {
    $WindowsSdk = 'C:\Program Files\Microsoft SDKs\Windows\v7.1'
    if ($toolRoot) {
        $portable = Join-Path $toolRoot 'WinSDKBuild_amd64\Program Files\Microsoft SDKs\Windows\v7.1'
        if (Test-Path -LiteralPath $portable) { $WindowsSdk = $portable }
    }
}
$compilerBin = Join-Path $VisualCpp 'bin\amd64'
$compiler = Join-Path $compilerBin 'cl.exe'
$linker = Join-Path $compilerBin 'link.exe'
$sdkInclude = Join-Path $WindowsSdk 'Include'
$sdkLibrary = Join-Path $WindowsSdk 'Lib\x64'
$ptcToolkit = Join-Path $CreoRoot 'Common Files\M250\protoolkit'
$ptcInclude = Join-Path $ptcToolkit 'includes'
$ptcLibrary = Join-Path $ptcToolkit 'x86e_win64\obj\protk_dll.lib'
foreach ($required in @($compiler,$linker,$sdkInclude,$sdkLibrary,$VisualCppHeaders,$ptcInclude,$ptcLibrary)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing build dependency $required" }
}

$output = Join-Path $PSScriptRoot 'bin'
New-Item -Path $output -ItemType Directory -Force | Out-Null
$oldInclude = $env:INCLUDE
$oldLibrary = $env:LIB
$oldPath = $env:PATH
try {
    $env:INCLUDE = "$VisualCppHeaders;$sdkInclude"
    $env:LIB = "$(Join-Path $VisualCpp 'lib\amd64');$sdkLibrary"
    $env:PATH = "$compilerBin;$oldPath"
    $objects = foreach ($module in @('bridge','sketch','hole')) {
        $source = Join-Path $PSScriptRoot "$module.c"
        $object = Join-Path $output "$module.obj"
        & $compiler /nologo /c /MT /O2 /W4 /WX /wd4127 /GS /fp:precise /DPRO_MACHINE=36 /DPRO_OS=4 "/I$ptcInclude" "/Fo$object" $source | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Creo TOOLKIT compilation failed for $module ($LASTEXITCODE)." }
        $object
    }
    $systemLibs = @('libcmt.lib','kernel32.lib','user32.lib','wsock32.lib','advapi32.lib','mpr.lib',
        'winspool.lib','netapi32.lib','psapi.lib','gdi32.lib','shell32.lib','comdlg32.lib','ole32.lib','ws2_32.lib')
    $dll = Join-Path $output 'creo2_agent.dll'
    & $linker /nologo /dll /machine:amd64 /subsystem:console /debug:none "/out:$dll" @objects $ptcLibrary @systemLibs
    if ($LASTEXITCODE -ne 0) { throw "Creo TOOLKIT linking failed ($LASTEXITCODE)." }
    Get-Item -LiteralPath $dll | Select-Object FullName,Length
} finally {
    $env:INCLUDE = $oldInclude
    $env:LIB = $oldLibrary
    $env:PATH = $oldPath
}
