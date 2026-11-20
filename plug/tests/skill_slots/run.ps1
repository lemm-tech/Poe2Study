param(
    [uint32]$GamePid = 0,
    [string]$OutputPath = '',
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
$probeRoot = $PSScriptRoot
$plugRoot = Split-Path -Parent (Split-Path -Parent $probeRoot)
$probeOutput = Join-Path $plugRoot '.build\tests\skill_slots'
New-Item -ItemType Directory -Path $probeOutput -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found' }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools not found' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
$project = Join-Path $probeRoot 'SkillSlotsProbe.vcxproj'
$buildLog = Join-Path $probeOutput 'build.log'
& $msbuild $project /t:Build /p:Configuration=Release /p:Platform=x64 /p:AutoInject=false /m:1 /nologo /v:minimal "/flp:logfile=$buildLog;encoding=UTF-8"
if ($LASTEXITCODE -ne 0) { throw "SkillSlotsProbe build failed; see $buildLog" }
$exe = Join-Path $probeOutput 'SkillSlotsProbe.exe'
if ($BuildOnly) { Write-Output "Built: $exe"; return }
if (-not $OutputPath) {
    $zone = [TimeZoneInfo]::FindSystemTimeZoneById('China Standard Time')
    $stamp = [TimeZoneInfo]::ConvertTimeFromUtc([DateTime]::UtcNow, $zone).ToString('yyyyMMddHHmmssfff')
    $OutputPath = Join-Path $probeOutput "slots-$stamp.json"
}
$probeArgs = @('--out', [IO.Path]::GetFullPath($OutputPath))
if ($GamePid) { $probeArgs += @('--pid', $GamePid.ToString()) }
& $exe @probeArgs
if ($LASTEXITCODE -ne 0) { throw 'Skill-slot snapshot failed; see the FAIL line above' }
