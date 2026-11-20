# Read-only production scanner check. No DLL injection or game actions.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root '.build\tests\encounter_probe'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools not found' }
$sources = @('tests\encounter_probe.cpp', 'Plug\Surroundings.cpp', 'Plug\WorldArea.cpp', 'Plug\MyOLAFunction.cpp',
    'Common\OlaDriver.cpp', 'Common\GameMemory.cpp', 'Common\Entity.cpp', 'Common\Component.cpp',
    'Common\PointerChain.cpp', 'Common\AppConfig.cpp', 'Common\Logger.cpp')
$arguments = ($sources | ForEach-Object { '"' + (Join-Path $root $_) + '"' }) -join ' '
$batch = @"
@echo off
chcp 65001 >nul
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b %errorlevel%
cl /nologo /EHsc /MT /O2 /utf-8 /std:c++17 /DNOMINMAX $arguments /Fo:"$out\\" /Fe:"$out\encounter_probe.exe" /link user32.lib
"@
$batchPath = Join-Path $out 'build.cmd'
Set-Content -LiteralPath $batchPath -Value $batch -Encoding utf8NoBOM
$savedPath = $env:PATH
try {
    $env:PATH = (($env:PATH -split ';' | ForEach-Object { $_.Trim('"') }) -join ';')
    & cmd.exe /d /c $batchPath *> (Join-Path $out 'build.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $out 'build.log'); throw 'Probe build failed' }
    & (Join-Path $out 'encounter_probe.exe') | Tee-Object -FilePath (Join-Path $out 'run.log')
    if ($LASTEXITCODE -ne 0) { throw 'Production encounter scan failed' }
} finally { $env:PATH = $savedPath }
