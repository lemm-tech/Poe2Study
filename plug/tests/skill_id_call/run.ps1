param(
    [uint32]$GamePid = 0,
    [string]$SkillId = '',
    [switch]$Cast,
    [switch]$DllPreflight,
    [int]$X = -1,
    [int]$Y = -1,
    [int]$HoldMs = 800,
    [int]$Flags = 0x400,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
if ($Cast -and $DllPreflight) { throw 'Choose Cast or DllPreflight, not both' }
if ($PSBoundParameters.ContainsKey('X') -ne $PSBoundParameters.ContainsKey('Y')) {
    throw 'Supply both X and Y'
}
if ($PSBoundParameters.ContainsKey('X') -and ($X -lt 0 -or $Y -lt 0)) {
    throw 'Coordinates must be non-negative'
}
$probeOutput = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\.build\tests\skill_id_call'))
New-Item -ItemType Directory -Path $probeOutput -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools not found' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild (Join-Path $PSScriptRoot 'SkillIdCallProbe.vcxproj') /t:Build /p:Configuration=Release /p:Platform=x64 /p:AutoInject=false /m:1 /nologo /v:minimal "/flp:logfile=$probeOutput\build.log;encoding=UTF-8"
if ($LASTEXITCODE -ne 0) { throw 'SkillIdCallProbe build failed' }
if ($BuildOnly) { Write-Output "Built: $probeOutput\SkillIdCallProbe.dll"; return }
$probeArgs = @('-X','utf8',(Join-Path $PSScriptRoot 'probe.py'),'--hold-ms',$HoldMs.ToString(),'--flags',$Flags.ToString())
if ($GamePid) { $probeArgs += @('--pid',$GamePid.ToString()) }
if ($SkillId) { $probeArgs += @('--id',$SkillId) }
if ($Cast) { $probeArgs += '--cast' }
if ($DllPreflight) { $probeArgs += '--dll-preflight' }
if ($PSBoundParameters.ContainsKey('X')) { $probeArgs += @('--x',$X.ToString(),'--y',$Y.ToString()) }
& python @probeArgs
if ($LASTEXITCODE -ne 0) { throw 'Probe failed or no actual-action evidence; see JSON and console' }
