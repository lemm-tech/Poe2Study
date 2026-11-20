param(
    [uint32]$GamePid = 0,
    [string]$OutputDirectory = '',
    [ValidateRange(1, 120)][int]$Samples = 1,
    [ValidateRange(100, 60000)][int]$IntervalMs = 1000,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
$plugRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDirectory = Join-Path $plugRoot '.build\tests\boss_probe'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found' }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools not found' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
$buildLog = Join-Path $buildDirectory 'build.log'
& $msbuild (Join-Path $PSScriptRoot 'BossProbe.vcxproj') /t:Build /p:Configuration=Release /p:Platform=x64 /p:AutoInject=false /m:1 /nologo /v:minimal "/flp:logfile=$buildLog;encoding=UTF-8"
if ($LASTEXITCODE -ne 0) { throw "BossProbe build failed; see $buildLog" }
$runtimeDirectory = Join-Path $plugRoot 'bin\BossProbeTest'
$exe = Join-Path $runtimeDirectory 'BossProbe.exe'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'scan.cmd') -Destination $runtimeDirectory -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $runtimeDirectory -Force
& $exe --self-test
if ($LASTEXITCODE -ne 0) { throw 'BossProbe self-test failed' }
if ($BuildOnly) { Write-Output "Built: $exe"; return }
$probeArgs = @('--samples', $Samples.ToString(), '--interval', $IntervalMs.ToString())
if ($GamePid) { $probeArgs += @('--pid', $GamePid.ToString()) }
if ($OutputDirectory) { $probeArgs += @('--out', [IO.Path]::GetFullPath($OutputDirectory)) }
& $exe @probeArgs
if ($LASTEXITCODE -ne 0) { throw 'BossProbe snapshot incomplete; see failure report' }
