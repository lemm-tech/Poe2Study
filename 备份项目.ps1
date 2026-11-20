[CmdletBinding()]
param(
    [string]$ProjectRoot = $PSScriptRoot,
    [string]$Task = '',
    [string]$Milestone = '',
    [string]$MarkMilestone = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $ProjectRoot).Path).TrimEnd([char[]]'\/')
if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot 'plug\Poe2Study.sln') -PathType Leaf)) {
    throw "不是项目根目录：$ProjectRoot（缺少 plug\Poe2Study.sln）"
}
$archiveRoot = [IO.Path]::GetFullPath((Join-Path $ProjectRoot '版本管理'))
if (Test-Path -LiteralPath $archiveRoot) {
    if ((Get-Item -LiteralPath $archiveRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw '版本管理目录不能是符号链接或目录联接。'
    }
} else { New-Item -ItemType Directory -Path $archiveRoot | Out-Null }
$zone = [TimeZoneInfo]::FindSystemTimeZoneById('China Standard Time')
$excluded = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in @('版本管理', '.git', '.vs', '.build', 'bin', 'obj', 'x64', 'Win32',
        'Debug', 'Release', '__pycache__', '.pytest_cache', 'node_modules', '.venv', 'venv',
        'build', 'dist', 'CMakeFiles', 'profiles', 'logs', 'output')) { [void]$excluded.Add($name) }
$extensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.ixx',
    '.asm', '.s', '.rc', '.rc2', '.def', '.idl', '.manifest', '.sln', '.slnx', '.vcxproj',
    '.filters', '.props', '.targets', '.cmake', '.py', '.ps1', '.psm1', '.psd1', '.bat',
    '.cmd', '.sh', '.cs', '.csproj', '.js', '.ts', '.jsx', '.tsx', '.html', '.css', '.scss',
    '.ini', '.json', '.jsonc', '.yaml', '.yml', '.toml', '.xml', '.config', '.env',
    '.md', '.rst', '.txt', '.csv', '.sql', '.svg', '.png', '.ico', '.jpg', '.jpeg', '.webp')
$specialNames = @('.gitignore', '.gitattributes', '.editorconfig', '.clang-format', '.clang-tidy', '.env')

function Assert-ArchiveChild([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path).TrimEnd([char[]]'\/')
    if (-not [string]::Equals([IO.Path]::GetDirectoryName($absolute), $archiveRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝操作版本管理目录以外的路径：$absolute"
    }
    if ((Test-Path -LiteralPath $absolute) -and
        ((Get-Item -LiteralPath $absolute -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "拒绝操作链接目录：$absolute"
    }
    return $absolute
}

function Get-SourceHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Get-SourceManifest {
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($ProjectRoot)
    $files = while ($pending.Count) {
        foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($entry.PSIsContainer) {
                if (-not $excluded.Contains($entry.Name)) { $pending.Push($entry.FullName) }
            } elseif ($entry.Extension -in $extensions -or $entry.Name -in $specialNames) { $entry }
        }
    }
    foreach ($file in @($files | Sort-Object FullName)) {
        [PSCustomObject]@{
            Path = $file.FullName.Substring($ProjectRoot.Length + 1).Replace('\', '/')
            SHA256 = Get-SourceHash $file.FullName
            Bytes = $file.Length
        }
    }
}

function Get-Fingerprint($Manifest) {
    [string[]]$lines = @(foreach ($entry in $Manifest) {
        "{0}`t{1}`t{2}" -f $entry.Path.Replace('\', '/'), $entry.SHA256.ToUpperInvariant(), $entry.Bytes
    })
    # Identical content must deduplicate across shell versions and locale settings.
    [Array]::Sort($lines, [StringComparer]::Ordinal)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n")))).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Get-Snapshots {
    Get-ChildItem -LiteralPath $archiveRoot -Directory -Force |
        Where-Object { $_.Name -match '^\d{14}$' -and
            -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName '源码校验.json') -PathType Leaf) } |
        Sort-Object Name -Descending
}

function Set-Milestone([string]$Snapshot, [string]$Label) {
    $Snapshot = Assert-ArchiveChild $Snapshot
    if ([string]::IsNullOrWhiteSpace($Label)) { throw '里程碑名称不能为空。' }
    [ordered]@{ name = $Label; markedAt = [TimeZoneInfo]::ConvertTimeFromUtc([DateTime]::UtcNow, $zone).ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Snapshot '里程碑.json') -Encoding UTF8
    Write-Output "里程碑已保留：$Snapshot（$Label）"
}

function Invoke-Retention {
    foreach ($snapshot in @(Get-Snapshots | Select-Object -Skip 5)) {
        if (Test-Path -LiteralPath (Join-Path $snapshot.FullName '里程碑.json')) { continue }
        $target = Assert-ArchiveChild $snapshot.FullName
        # Check descendants without following junctions before any recursive deletion.
        $pending = [Collections.Generic.Stack[string]]::new()
        $pending.Push($target)
        $hasLink = $false
        while ($pending.Count -and -not $hasLink) {
            foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
                if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { $hasLink = $true; break }
                if ($entry.PSIsContainer) { $pending.Push($entry.FullName) }
            }
        }
        if ($hasLink) { Write-Warning "包含链接，保留以便人工检查：$target"; continue }
        Remove-Item -LiteralPath $target -Recurse -Force
        Write-Output "已清理超额快照：$($snapshot.Name)"
    }
}

# One writer prevents simultaneous runs from publishing or pruning each other's snapshots.
$lock = [IO.File]::Open((Join-Path $archiveRoot '.backup.lock'), [IO.FileMode]::OpenOrCreate,
    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
$staging = $null
try {
    if ($MarkMilestone) {
        if ($MarkMilestone -notmatch '^\d{14}$') { throw '快照名称必须是 14 位北京时间时间戳。' }
        $existing = @(Get-Snapshots | Where-Object { $_.Name -eq $MarkMilestone })
        if ($existing.Count -ne 1) { throw "未找到完整快照：$MarkMilestone" }
        Set-Milestone $existing[0].FullName $Milestone
        Invoke-Retention
        return
    }

    $manifest = @(Get-SourceManifest)
    if (-not $manifest.Count) { throw '未找到可备份的源码、配置或文档。' }
    $fingerprint = Get-Fingerprint $manifest
    $latest = @(Get-Snapshots | Select-Object -First 1)
    if ($latest.Count) {
        $previous = Get-Content -LiteralPath (Join-Path $latest[0].FullName '源码校验.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($fingerprint -eq (Get-Fingerprint $previous)) {
            if ($Milestone) { Set-Milestone $latest[0].FullName $Milestone }
            Write-Output "内容未变，跳过备份：$($latest[0].FullName)"
            Invoke-Retention
            return
        }
    }

    $staging = Assert-ArchiveChild (Join-Path $archiveRoot ('.pending-' + [Guid]::NewGuid().ToString('N')))
    New-Item -ItemType Directory -Path $staging | Out-Null
    foreach ($entry in $manifest) {
        $source = Join-Path $ProjectRoot $entry.Path
        $target = Join-Path $staging $entry.Path
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target
        if ((Get-SourceHash $target) -ne $entry.SHA256 -or
            (Get-Item -LiteralPath $target).Length -ne $entry.Bytes) { throw "备份校验失败：$($entry.Path)" }
    }
    if ((Get-Fingerprint @(Get-SourceManifest)) -ne $fingerprint) { throw '备份期间源文件发生变化；未发布快照，请重试。' }
    ConvertTo-Json -InputObject $manifest -Depth 4 |
        Set-Content -LiteralPath (Join-Path $staging '源码校验.json') -Encoding UTF8
    $now = [TimeZoneInfo]::ConvertTimeFromUtc([DateTime]::UtcNow, $zone)
    [ordered]@{
        schemaVersion = 2; kind = 'source'; createdAtBeijing = $now.ToString('o'); task = $Task
        sourceFingerprint = $fingerprint; fileCount = $manifest.Count
        bytes = ($manifest | Measure-Object -Property Bytes -Sum).Sum
        excludedDirectories = @($excluded | Sort-Object)
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $staging '备份信息.json') -Encoding UTF8
    do {
        $stamp = [TimeZoneInfo]::ConvertTimeFromUtc([DateTime]::UtcNow, $zone).ToString('yyyyMMddHHmmss')
        $destination = Assert-ArchiveChild (Join-Path $archiveRoot $stamp)
        if (Test-Path -LiteralPath $destination) { Start-Sleep -Milliseconds 250 }
    } while (Test-Path -LiteralPath $destination)
    # Both absolute paths were checked as immediate children of archiveRoot.
    Move-Item -LiteralPath $staging -Destination $destination
    $staging = $null
    if ($Milestone) { Set-Milestone $destination $Milestone }
    Write-Output "备份完成：$destination"
    Write-Output "已校验 $($manifest.Count) 个源码、配置、文档及源资源文件；排除运行包、编译缓存与日志。"
    Invoke-Retention
} finally {
    # Failed staging directories stay unpublished for inspection; never prune on failure.
    if ($staging) { Write-Warning "备份未完成，暂存目录保留：$staging" }
    $lock.Dispose()
}
