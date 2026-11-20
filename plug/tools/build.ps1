# Build only. Loading Monitor.dll is an explicit action in NavGUI.
param(
    [ValidateSet('Rebuild', 'Build', 'Clean')]
    [string]$Target = 'Build',
    [ValidateSet('', 'Plug', 'Monitor', 'NavGUI')]
    [string]$Project = '',
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$OutputDirectory = '',
    [switch]$SkipChecks
)
$ErrorActionPreference = 'Stop'
$solutionDir = Split-Path -Parent $PSScriptRoot
$outputDir = if ($OutputDirectory) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    Join-Path $solutionDir "bin\$Configuration"
}
if (-not $SkipChecks -and $Target -ne 'Clean') {
    $python = Get-Command python -ErrorAction Stop
    & $python.Source -X utf8 (Join-Path $PSScriptRoot 'check_arch.py') --strict
    if ($LASTEXITCODE -ne 0) { throw 'Architecture check failed' }
    & $python.Source -X utf8 (Join-Path $PSScriptRoot 'check_encoding.py')
    if ($LASTEXITCODE -ne 0) { throw 'Encoding check failed' }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools not found' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
$parts = if ($Project) { @($Project) } else { @('Plug', 'Monitor', 'NavGUI') }
foreach ($part in $parts) {
    $intermediate = Join-Path $solutionDir ".build\x64\$Configuration\$part"
    $buildArgs = @(
        (Join-Path $solutionDir "$part\$part.vcxproj"),
        "/p:Configuration=$Configuration", '/p:Platform=x64', '/p:AutoInject=false',
        "/p:OutDir=$outputDir\", "/p:IntDir=$intermediate\",
        "/t:$Target", '/m:1', '/nologo', '/v:minimal'
    )
    & $msbuild @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $part (exit $LASTEXITCODE)" }
}
if ($Target -eq 'Clean') { return }
$files = [ordered]@{}
foreach ($name in @('NavGUI.exe', 'Plug.exe', 'Monitor.dll', 'config.ini', 'recovery.ini')) {
    $path = Join-Path $outputDir $name
    if (Test-Path -LiteralPath $path) {
        $files[$name] = [ordered]@{
            bytes = (Get-Item -LiteralPath $path).Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}
[ordered]@{
    createdAt = [DateTimeOffset]::Now.ToString('o')
    configuration = "$Configuration x64"
    autoInject = $false
    builtProjects = $parts
    files = $files
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputDir 'build_info.json') -Encoding UTF8
Write-Host "Output: $outputDir"
