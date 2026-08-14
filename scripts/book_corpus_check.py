#!/usr/bin/env python3
"""Low-cost host-side sanity report for InkMOD regression books.

Usage:
  python scripts/book_corpus_check.py book1.epub book2.fb2 book3.fb2.zip

It never modifies input files. The report is intentionally structural: it
finds unusually large EPUB HTML members and counts FB2 sections/binaries so a
release corpus can keep known stress cases visible before hardware testing.
"""
from __future__ import annotations
import re
import sys
import zipfile
from pathlib import Path

HTML_EXT = ('.xhtml', '.html', '.htm')

def human(n: int) -> str:
    return f"{n/1024/1024:.2f} MiB" if n >= 1024*1024 else f"{n/1024:.1f} KiB"

def epub_report(path: Path) -> int:
    try:
        with zipfile.ZipFile(path) as z:
            html = [(i.filename, i.file_size) for i in z.infolist() if i.filename.lower().endswith(HTML_EXT)]
            html.sort(key=lambda x: x[1], reverse=True)
            largest = html[0] if html else ('-', 0)
            print(f"EPUB  {path.name}: {human(path.stat().st_size)}, html={len(html)}, largest={largest[0]} {human(largest[1])}")
            if largest[1] >= 300*1024:
                print("      NOTE: oversized HTML spine candidate; verify first-open latency on X3/X4")
        return 0
    except Exception as e:
        print(f"ERROR {path}: {e}")
        return 1

def fb2_bytes(path: Path) -> bytes:
    if path.name.lower().endswith('.zip'):
        with zipfile.ZipFile(path) as z:
            names=[n for n in z.namelist() if n.lower().endswith('.fb2')]
            if not names: raise ValueError('archive contains no .fb2')
            return z.read(names[0])
    return path.read_bytes()

def fb2_report(path: Path) -> int:
    try:
        data=fb2_bytes(path)
        sections=len(re.findall(br'<(?:[A-Za-z0-9_]+:)?section\b', data, re.I))
        binaries=len(re.findall(br'<(?:[A-Za-z0-9_]+:)?binary\b', data, re.I))
        paras=len(re.findall(br'<(?:[A-Za-z0-9_]+:)?p(?:\s|>)', data, re.I))
        print(f"FB2   {path.name}: source={human(len(data))}, sections={sections}, binaries={binaries}, paragraphs={paras}")
        if len(data) >= 20*1024*1024:
            print("      NOTE: large FB2 stress case; verify compact index and first-open recovery")
        return 0
    except Exception as e:
        print(f"ERROR {path}: {e}")
        return 1

def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 0
    rc=0
    for raw in sys.argv[1:]:
        p=Path(raw)
        low=p.name.lower()
        if low.endswith('.epub'):
            rc |= epub_report(p)
        elif low.endswith('.fb2') or low.endswith('.fb2.zip') or low.endswith('.zip'):
            rc |= fb2_report(p)
        else:
            print(f"SKIP  {p}: unsupported corpus type")
    return rc

if __name__ == '__main__':
    raise SystemExit(main())
