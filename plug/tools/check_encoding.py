# -*- coding: utf-8 -*-
"""复查项目里每个 .cpp/.h 的实际编码。

为什么需要它：本工程混用 GBK(936) 与 UTF-8(with BOM) 两种源码编码，
MSVC 又没有开 /utf-8，编码写错会直接变成编译错误（典型症状：
UTF-8 无 BOM 的中文注释被按 936 误解码，把下一行代码一起吞掉，
报「xxx 未声明」）。

判定规则：
  - 有 BOM                                   -> UTF-8 BOM   ✅
  - 无 BOM 但能按 UTF-8 解码且含非 ASCII      -> UTF-8 noBOM  ⚠️ 要补 BOM
  - 无 BOM 且不是合法 UTF-8                   -> GBK          ✅
  - 无 BOM 且全 ASCII                         -> ASCII        ✅

用法：
    python tools\\check_encoding.py            # 只报告
    python tools\\check_encoding.py --fix-bom  # 顺手给 UTF-8 noBOM 的文件补 BOM
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {".vs", ".build", "bin", "x64", "Win32", "__pycache__", "SDK"}

UTF8_BOM = b"\xef\xbb\xbf"


def iter_sources():
    for pattern in ("**/*.cpp", "**/*.h", "**/*.hpp"):
        for path in glob.glob(os.path.join(ROOT, pattern), recursive=True):
            parts = set(os.path.relpath(path, ROOT).split(os.sep)[:-1])
            if parts & SKIP_DIRS:
                continue
            yield path


def classify(path):
    raw = open(path, "rb").read()
    if raw.startswith(UTF8_BOM):
        return "UTF-8 BOM", True
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return "GBK", True
    if all(ord(ch) < 128 for ch in text):
        return "ASCII", True
    return "UTF-8 noBOM", False


def main():
    fix = "--fix-bom" in sys.argv
    bad = 0
    for path in sorted(iter_sources()):
        enc, ok = classify(path)
        rel = os.path.relpath(path, ROOT)
        if ok:
            print("  %-16s %s" % (enc, rel))
            continue
        bad += 1
        if fix:
            raw = open(path, "rb").read()
            open(path, "wb").write(UTF8_BOM + raw)
            print("  %-16s %s   -> BOM added" % (enc, rel))
        else:
            print("  %-16s %s   !! fix: --fix-bom" % (enc, rel))

    print("")
    if bad and not fix:
        print("发现 %d 个 UTF-8 无 BOM 的中文源文件：不补 BOM 会被 MSVC 当 GBK 误解码。" % bad)
        return 1
    print("编码检查通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
