#!/usr/bin/env python3
"""Prepare src/stubs.asm for llvm-ml.

llvm-ml (LLVM's MASM) does not implement EXTERNDEF and does not apply the
C '_' prefix to symbols, while clang-compiled C++ objects reference
'_name'.  Rewrite EXTERNDEF->EXTERN and prefix every external/global and
every PROC name with '_'.
"""
import re
import sys

src = open(sys.argv[1]).read().replace("EXTERNDEF", "EXTERN")
names = set(re.findall(r"^EXTERN\s+(\w+)\s*:", src, re.M))
names |= set(re.findall(r"^(\w+)\s+PROC\b", src, re.M))
for n in sorted(names, key=len, reverse=True):
    src = re.sub(r"(?<![\w_])" + re.escape(n) + r"(?![\w_])", "_" + n, src)
open(sys.argv[2], "w").write(src)
