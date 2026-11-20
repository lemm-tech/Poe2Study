# -*- coding: utf-8 -*-
"""架构守卫：防止重构成果被慢慢改回去。

检查项（每条都对应重构时收掉的一类重复）：

  1. 除 Common/OlaDriver.cpp 之外，任何源文件都不准 include SDK/OLAPlugServer.h
  2. 任何源文件都不准出现硬写的地址范围魔数 0xFFFFFFF / 0x7ffffffffff
     （必须走 IsPlausibleAddress / GameMemory::Follow）
  3. 任何源文件都不准出现 kIntTypeU8 / kIntTypeU32 / kIntTypeU64
     （必须走 GameMemory::Read<T>）
  4. 任何源文件都不准出现 ReadIntAddr / ReadStringAddr / ReadDataAddrToBin
     （必须走 OlaDriver / GameMemory）
  5. 资源名不准硬编码：Lmy / map_cost.bmp / monitor_log.txt / Path of Exile 2
     （必须走 AppConfig）
  6. 偏移不准零散硬写：Plug/ 与 Common/ 下（GameOffsets.h 除外）不准出现
     m_ptr + 0x 形式的裸偏移

用法：
    python tools\\check_arch.py          # 只报告
    python tools\\check_arch.py --strict # 有违规时返回非 0（给 build.ps1 用）
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SKIP_DIRS = {".vs", ".build", "bin", "x64", "Win32", "__pycache__", "SDK"}

# 允许持有 SDK 与类型码的"唯一出口"
OLA_ALLOWED = {os.path.join("Common", "OlaDriver.cpp")}

# 允许写资源名默认值的唯一出口
# （这些是"配置项的默认值"，资源名本来就该只在这几处出现）
APP_CONFIG_ALLOWED = {
    os.path.join("Common", "AppConfig.h"),
    os.path.join("Common", "AppConfig.cpp"),
    os.path.join("Common", "Logger.h"),
    os.path.join("Monitor", "MonitorConfig.h"),
    os.path.join("Monitor", "Log.h"),          # 日志文件名的最后兜底
}

# 允许写裸偏移的文件（偏移唯一定义处）
OFFSET_ALLOWED = {
    os.path.join("Common", "GameOffsets.h"),
    os.path.join("Common", "GameMemory.cpp"),
    os.path.join("Plug", "Inventory.cpp"),      # 树节点布局在本文件已集中
    os.path.join("Plug", "Surroundings.cpp"),
}

# 检查范围：原来只查 Plug / Common，Monitor 是"法外之地"，
# 导致硬编码 "PathOfExile.exe"、裸指针解引用等问题长期无人拦截。
CHECKED_TOP_DIRS = ("Plug", "Common", "Monitor")

# 允许进程内裸指针解引用的唯一出口
# （这些文件里的 SafeMem / SafeReadU64 都已内置 IsPlausibleAddress + SEH 兜底）
RAW_DEREF_ALLOWED = {
    os.path.join("Monitor", "CostProbe.cpp"),
    os.path.join("Monitor", "ActionWalker.cpp"),   # 走路 call 的 rdi/vtable 校验
    os.path.join("Monitor", "NavDriver.cpp"),      # 玩家坐标 / 组件解析
    os.path.join("Monitor", "SkillPusher.cpp"),    # 技能 call 的 vtable / 入口校验
}

RULES = [
    # (编号, 说明, 正则, 允许的文件集合, 是否只查 Plug/Common)
    ("A1", "include 了 OLA SDK", re.compile(r'OLAPlugServer\.h'), OLA_ALLOWED, True),
    ("A2", "硬写地址范围魔数",
     re.compile(r'0x[fF]{7,}\b|0x7ffffffffff', re.I), OFFSET_ALLOWED, True),
    ("A3", "手写整数类型码",
     re.compile(r'kIntTypeU8|kIntTypeU32|kIntTypeU64'), OLA_ALLOWED, True),
    ("A4", "直接调用 OLA 读接口",
     re.compile(r'\bReadIntAddr\b|\bReadStringAddr\b|\bReadDataAddrToBin\b'), OLA_ALLOWED, True),
    ("A5", "硬编码资源名",
     re.compile(r'"Lmy"|"map_cost\.bmp"|"monitor_log\.txt"|"Path of Exile 2"'),
     APP_CONFIG_ALLOWED, True),
]

# 仅针对 Monitor 的规则（进程内直读、printf 等）
MONITOR_RULES = [
    ("A7", "进程内裸指针解引用（应走 SafeMem / GameMemory）",
     re.compile(r'\*\s*\(\s*(?:const\s+)?(?:UINT64|UINT32|uint64_t|uint32_t|int)\s*\*\s*\)'),
     RAW_DEREF_ALLOWED),

    ("A8", "DLL 里用 printf（SubSystem=Windows 无 stdout，日志会丢）",
     re.compile(r'\bprintf\s*\('),
     {os.path.join("Monitor", "Log.h")}),

    ("A9", "硬编码游戏模块名 / 管道名（应走 MonitorConfig / AppConfig）",
     re.compile(r'"PathOfExile\.exe"|L"Lmy"'),
     {os.path.join("Monitor", "MonitorConfig.h"),   # 默认值唯一出口
      os.path.join("Monitor", "dllmain.cpp")}),    # 已改为从 g_cfg 取，这里只留 MFC-free 的兜底
]


def iter_sources():
    for pattern in ("**/*.cpp", "**/*.h", "**/*.hpp"):
        for path in glob.glob(os.path.join(ROOT, pattern), recursive=True):
            rel = os.path.relpath(path, ROOT)
            parts = set(rel.split(os.sep)[:-1])
            if parts & SKIP_DIRS:
                continue
            if rel.split(os.sep)[0] not in CHECKED_TOP_DIRS:
                continue
            yield path, rel


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", errors="replace")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("gbk", errors="replace")


def main():
    strict = "--strict" in sys.argv
    violations = []

    for path, rel in sorted(iter_sources()):
        text = read_text(path)
        lines = text.splitlines()

        for rule_id, desc, pattern, allowed, _ in RULES:
            if rel in allowed:
                continue
            for idx, line in enumerate(lines, start=1):
                # 跳过纯注释行（架构守卫只看代码）
                stripped = line.strip()
                if stripped.startswith("//") or stripped.startswith("*"):
                    continue
                if pattern.search(line):
                    violations.append((rule_id, desc, rel, idx, stripped[:110]))

    # A6：Plug / Monitor 下不准出现裸偏移拼接
    offset_pat = re.compile(r'm_ptr\s*\+\s*0x')
    for path, rel in sorted(iter_sources()):
        if rel.startswith("Common"):
            continue
        text = read_text(path)
        for idx, line in enumerate(text.splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            if offset_pat.search(line):
                violations.append(("A6", "裸偏移出现在业务代码", rel, idx, stripped[:110]))

    # A7 / A8 / A9：Monitor 专属规则
    for path, rel in sorted(iter_sources()):
        if not rel.startswith("Monitor" + os.sep):
            continue
        text = read_text(path)
        lines = text.splitlines()

        for rule_id, desc, pattern, allowed in MONITOR_RULES:
            if rel in allowed:
                continue
            for idx, line in enumerate(lines, start=1):
                stripped = line.strip()
                if stripped.startswith("//") or stripped.startswith("*"):
                    continue
                # A8 的检查对象是 printf，注释里提到 printf 不算违规
                if rule_id == "A8" and stripped.startswith("//"):
                    continue
                if pattern.search(line):
                    violations.append((rule_id, desc, rel, idx, stripped[:110]))

    if not violations:
        print("架构守卫通过：没有发现违规。")
        return 0

    print("发现 %d 处架构违规：" % len(violations))
    print("")
    last_rel = None
    for rule_id, desc, rel, idx, snippet in violations:
        if rel != last_rel:
            print("  %s" % rel)
            last_rel = rel
        print("    [%s] %s  L%d: %s" % (rule_id, desc, idx, snippet))
    print("")
    print("说明：偏移必须集中到 Common\\GameOffsets.h；")
    print("      读内存必须走 OlaDriver / GameMemory；资源名必须走 AppConfig。")
    return 1 if strict else 0


if __name__ == "__main__":
    sys.exit(main())
