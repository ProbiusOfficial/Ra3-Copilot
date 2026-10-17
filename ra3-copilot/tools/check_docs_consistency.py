#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""RA3 Copilot 文档一致性检查（零依赖，仅标准库）。

用途
----
交付前跑一遍，抓三类"文档与代码不一致"：

  1. 端点差集：docs/ 里声称存在、但 src/httpapi.cpp 里根本没路由的端点（虚标），
     以及代码里有、docs/ 里从没写的端点（漏记）。
  2. 可疑状态词：docs/ 里出现「未实现 / 待实测 / 待验收 / 待验证 / 无实际作用」的行。
     只提示、不判定对错——有些「未实现」是真实状态（如 privateIdle）。
  3. 记忆页体积：.workbuddy/memory/MEMORY.md 超过 10240 字节时告警。

用法
----
  python check_docs_consistency.py          # 中文文本报告，问题存在时退出码 1
  python check_docs_consistency.py --json   # 机器可读 JSON，同样按问题定退出码
  python check_docs_consistency.py --strict # 仅「文档有代码无」或 MEMORY 超限 -> 1（默认）

判定口径（哪些写法才算端点）
--------------------------
代码侧：只认 `strcmp(path, "/xxx")` 里的字符串字面量。一个 if 里并列多个（`/read` 与
`/hex`）会全部收下；函数体内再次用来判类型的分支（`/view` vs `/cursor`）也收下——它们
同样是合法路由字面量。

文档侧：必须有"动词证据"，即同一行出现下列任一形式，否则整行不算端点声明：
  * 行内代码带动词：`GET /snapshot`、`POST /summon?...`
  * 表格列式动词：`| GET | `/ping` |`
  * 裸文本动词：`POST /order/select?...`（代码块里的草案也算，但会按文件名归类）
同一行里出现的其它斜杠路径（如 `GET /state` / `/diag` 的第二个）一并收下，因为该行已
经明确是端点清单。

路径归一化：去掉 `?query`、去掉尾部标点，只留路径本身（`/order`、`/snapshot`）。

防误判（以下几类绝不算端点）
  * 文件路径：`docs/xxx.md`、`../src/httpapi.cpp`、`C:/Users/...`
    —— 判据是 `/` 前一个字符为字母/数字/`_`/`.`/`:`/`-` 时直接丢弃。
  * URL：先把 `http://host:port` 前缀整体剥掉，剩下的 `/path` 才算，避免把
    `127.0.0.1:17832` 的端口号当路径边界，也避免整条 URL 被误读。
  * 带扩展名的路径（`/foo.md`）、超过 3 段的深路径、纯目录（`/x/`）。
  * 无动词的散文里出现的 `/`（"读/写"、中文斜杠）不匹配（要求 `/` 后紧跟 ASCII 字母）。

文档目录/记忆页位置
------------------
脚本在 ra3-copilot/tools/ 下，因此真实路径是 ../../docs 与 ../../.workbuddy/memory/MEMORY.md。
同时兼容把 tools 直接放在仓库根的老布局（../docs / ../.workbuddy/...），按存在性择一。

文件名含「调研 / 方案 / 可行性 / 规划 / 草案」的文档视为**未承诺的草案**，其中出现的
端点单独列出、不计入退出码——否则草案里的 P2 API 会永久把门禁刷红，工具就失去意义。

退出码
------
0 = 干净；1 = 存在「文档有代码无」端点，或 MEMORY.md 超限，或关键文件缺失。
"""

import argparse
import json
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------- 常量与路径

MAX_MEMORY_BYTES = 10240
STATUS_WORDS = ("未实现", "待实测", "待验收", "待验证", "无实际作用")
PROPOSAL_MARKERS = ("调研", "方案", "可行性", "规划", "草案")
# 代码里保留、但文档刻意不再列的兼容端点：不判为"文档漏记"
CODE_ONLY_IGNORE = {"/ai/set"}

ROOT = Path(__file__).resolve().parent            # ra3-copilot/tools
SOURCE_CANDIDATES = [
    ROOT.parent / "src" / "httpapi.cpp",          # ra3-copilot/src/httpapi.cpp
    ROOT.parent.parent / "src" / "httpapi.cpp",
]
DOCS_CANDIDATES = [
    ROOT.parent.parent / "docs",                  # RA3/docs
    ROOT.parent / "docs",                         # ra3-copilot/docs
]
MEMORY_CANDIDATES = [
    ROOT.parent.parent / ".workbuddy" / "memory" / "MEMORY.md",
    ROOT.parent / ".workbuddy" / "memory" / "MEMORY.md",
]

CODE_ENDPOINT_RE = re.compile(r'strcmp\s*\(\s*path\s*,\s*"([^"]+)"')
# 端点行的"动词证据"：`GET /x` / 裸文本 GET /x / 表格 | GET | `...
VERB_EVIDENCE_RE = re.compile(
    r'\b(?:GET|POST|PUT|DELETE|PATCH)\b\s*`?\s*/'
    r'|\|\s*(?:GET|POST|PUT|DELETE|PATCH)\s*\|'
)
# 只剥 URL 的 scheme://host:port，保留其后的 path（否则 /ping 会被整条吃掉）
URL_PREFIX_RE = re.compile(r'https?://[^\s/`\'"|)\]，。；]+')
# 路径候选：左侧必须是干净边界（防 docs/x、C:/x、../x）
PATH_RE = re.compile(r'(?<![\w./:\\-])/[A-Za-z][A-Za-z0-9_/]*')


def read_text(path):
    """源码/文档是 GBK/UTF-8 混合，统一用替换模式读，永不因编码崩溃。"""
    return path.read_text(encoding="utf-8", errors="replace")


def pick(candidates, label):
    for p in candidates:
        if p.exists():
            return p
    print("[警告] 找不到%s，已跳过该项：" % label, file=sys.stderr)
    for p in candidates:
        print("        tried %s" % p, file=sys.stderr)
    return None


# ---------------------------------------------------------------- 检查 1

def extract_code_endpoints(source_path):
    text = read_text(source_path)
    found = set()
    for raw in CODE_ENDPOINT_RE.findall(text):
        raw = raw.strip()
        if not raw.startswith("/"):
            continue
        path = raw.split("?", 1)[0].rstrip("/") or "/"
        found.add(path)
    return found


def normalize_doc_path(token, line, end):
    """把正则命中收敛成端点路径；不是端点则返回 None。"""
    path = token.rstrip("/")
    if not path or path == "/":
        return None
    # 紧邻的 . - 说明它是文件/连字符单词的一部分（/foo.md、/read-only）
    if end < len(line) and line[end] in ".-":
        return None
    segments = [s for s in path.split("/") if s]
    if not segments or len(segments) > 3:
        return None
    if any(s.startswith(".") for s in segments):
        return None
    return path


def extract_doc_endpoints(doc_path):
    """返回 {path: [('file.md', 行号), ...]}。"""
    hits = {}
    for lineno, line in enumerate(read_text(doc_path).splitlines(), 1):
        if not VERB_EVIDENCE_RE.search(line):
            continue
        # 剥掉 URL 前缀，让 http://127.0.0.1:17832/ping 里的 /ping 落在词边界上
        scan = URL_PREFIX_RE.sub(" ", line)
        for m in PATH_RE.finditer(scan):
            path = normalize_doc_path(m.group(0), scan, m.end())
            if path is None:
                continue
            hits.setdefault(path, []).append((doc_path.name, lineno))
    return hits


def is_proposal_doc(name):
    stem = Path(name).stem
    return any(marker in stem for marker in PROPOSAL_MARKERS)


def check_endpoints(source_path, docs_dir):
    code = extract_code_endpoints(source_path)
    doc_hits = {}
    proposal_hits = {}
    for md in sorted(docs_dir.glob("*.md")):
        hits = extract_doc_endpoints(md)
        bucket = proposal_hits if is_proposal_doc(md.name) else doc_hits
        for path, where in hits.items():
            bucket.setdefault(path, []).extend(where)

    missing_in_code = {p: w for p, w in doc_hits.items() if p not in code}
    missing_in_docs = {p: [] for p in sorted(code) if p not in doc_hits and p not in CODE_ONLY_IGNORE}
    proposal_only = {
        p: w for p, w in proposal_hits.items()
        if p not in code and p not in doc_hits
    }
    return {
        "code_endpoints": sorted(code),
        "doc_endpoints": sorted(doc_hits),
        "missing_in_code": missing_in_code,
        "missing_in_docs": missing_in_docs,
        "proposal_only": proposal_only,
    }


# ---------------------------------------------------------------- 检查 2

def check_status_words(docs_dir):
    findings = []
    for md in sorted(docs_dir.glob("*.md")):
        for lineno, line in enumerate(read_text(md).splitlines(), 1):
            hit = [w for w in STATUS_WORDS if w in line]
            if not hit:
                continue
            findings.append({
                "file": md.name,
                "line": lineno,
                "words": hit,
                "text": line.strip()[:120],
            })
    return findings


# ---------------------------------------------------------------- 检查 3

def check_memory(memory_path):
    if memory_path is None:
        return {"path": None, "bytes": 0, "limit": MAX_MEMORY_BYTES, "over": False,
                "skipped": True}
    size = memory_path.stat().st_size
    return {
        "path": str(memory_path),
        "bytes": size,
        "limit": MAX_MEMORY_BYTES,
        "over": size > MAX_MEMORY_BYTES,
        "skipped": False,
    }


# ---------------------------------------------------------------- 输出

def format_where(where):
    seen = []
    for name, lineno in where:
        item = "%s:%d" % (name, lineno)
        if item not in seen:
            seen.append(item)
    return "、".join(seen[:4]) + ("…" if len(seen) > 4 else "")


def print_report(result):
    ep = result["endpoints"]
    print("=== 1. 端点对账 ===\n")
    print("代码侧 %d 个端点，文档侧 %d 个（仅统计承诺类文档）"
          % (len(ep["code_endpoints"]), len(ep["doc_endpoints"])))
    print()

    bad = ep["missing_in_code"]
    print("【文档有、代码无】%d 个 —— 文档虚标，必须改" % len(bad))
    if bad:
        for path in sorted(bad):
            print("  %-28s ← %s" % (path, format_where(bad[path])))
    else:
        print("  （无）")
    print()

    gap = ep["missing_in_docs"]
    print("【代码有、文档无】%d 个 —— 文档漏记，建议补" % len(gap))
    if gap:
        for path in gap:
            print("  %s" % path)
    else:
        print("  （无）")
    print()

    if ep["proposal_only"]:
        print("【附录】调研/方案类文档中的未落地端点 %d 个（不计入判定）"
              % len(ep["proposal_only"]))
        for path in sorted(ep["proposal_only"]):
            print("  %-28s ← %s" % (path, format_where(ep["proposal_only"][path])))
        print()

    print("=== 2. 可疑状态词（仅提示，不判定对错）===\n")
    words = result["status_words"]
    print("命中 %d 行" % len(words))
    if words:
        print()
        for item in words:
            print("  %s:%d: %s" % (item["file"], item["line"], item["text"]))
    print()

    mem = result["memory"]
    print("=== 3. 记忆页体积 ===\n")
    if mem["skipped"]:
        print("  未找到 MEMORY.md，跳过")
    elif mem["over"]:
        print("  %s = %d B，超过上限 %d B（超出 %d B）⚠"
              % (mem["path"], mem["bytes"], mem["limit"],
                 mem["bytes"] - mem["limit"]))
    else:
        print("  %s = %d B（上限 %d B，余量 %d B）"
              % (mem["path"], mem["bytes"], mem["limit"],
                 mem["limit"] - mem["bytes"]))
    print()

    print("=== 结论 ===\n")
    if result["exit_code"] == 0:
        print("通过：文档与代码端点一致，记忆页未超限。")
    else:
        reasons = []
        if bad:
            reasons.append("文档虚标 %d 个端点" % len(bad))
        if mem["over"]:
            reasons.append("MEMORY.md 超限 %d B" % (mem["bytes"] - mem["limit"]))
        if result["fatal"]:
            reasons.append("关键文件缺失")
        print("未通过：" + "；".join(reasons))
    print("退出码 %d" % result["exit_code"])


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="检查 RA3 Copilot 文档与 HTTP 端点实现是否一致")
    parser.add_argument("--json", action="store_true", help="输出机器可读 JSON")
    args = parser.parse_args(argv)

    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass

    fatal = False
    source = pick(SOURCE_CANDIDATES, "源码 src/httpapi.cpp")
    docs_dir = pick(DOCS_CANDIDATES, "文档目录 docs/")
    memory_path = next((p for p in MEMORY_CANDIDATES if p.exists()), None)

    if source is None or docs_dir is None:
        fatal = True
        endpoints = {"code_endpoints": [], "doc_endpoints": [],
                     "missing_in_code": {}, "missing_in_docs": {},
                     "proposal_only": {}}
        status_words = []
    else:
        endpoints = check_endpoints(source, docs_dir)
        status_words = check_status_words(docs_dir)

    memory = check_memory(memory_path)
    exit_code = 1 if (endpoints["missing_in_code"] or memory["over"] or fatal) else 0

    result = {
        "source": str(source) if source else None,
        "docs_dir": str(docs_dir) if docs_dir else None,
        "endpoints": endpoints,
        "status_words": status_words,
        "memory": memory,
        "fatal": fatal,
        "exit_code": exit_code,
    }

    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print_report(result)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
