#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_site.py - 生成「RA3 日冕 · 单位代码」项目网站（纯静态）

输出：
  unit-codes/site/index.html   项目站点（介绍 / 算法 / 提取流程 / 可搜索总表）
  unit-codes/site/data.js      单位代码数据（由 data/unit_codes_all.tsv 生成）

用法：python gen_site.py
"""
import os
import io
import csv
import json

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE = os.path.abspath(os.path.join(HERE, '..'))
SRC = os.path.join(ARCHIVE, 'data', 'unit_codes_all.tsv')
FINAL = os.path.join(ARCHIVE, 'data', 'callable_final.tsv')
OUTDIR = os.path.join(ARCHIVE, 'site')

FAC_ORDER = {'神州': 0, '盟军': 1, '苏联': 2, '帝国': 3, '中立': 4, '平民': 5, '特殊': 6, '': 9}
CLASS_ORDER = {'建筑': 0, '步兵': 1, '载具': 2, '舰船': 3, '飞行器': 4, '箱子': 5, '其他': 6, '': 9}


def load_rows():
    with io.open(SRC, encoding='utf-8') as f:
        rows = list(csv.DictReader(f, delimiter='\t'))
    rows.sort(key=lambda r: (0 if r['summonable'] == 'yes' else 1,
                             FAC_ORDER.get(r['faction'], 8),
                             CLASS_ORDER.get(r['unit_class'], 9),
                             0 if r['is_primary'] == '1' else 1,
                             r['name'].lower()))
    return rows


def build_data_json(rows):
    out = []
    for r in rows:
        out.append({
            'c': r['code_hex'],
            'd': r['code_dec'],
            'n': r['name'],
            'cn': r['chinese_name'],
            'f': r['faction'],
            'fam': r['family'],
            'p': r['is_primary'],
            'note': r['note'],
            't': r['type_line'],
            'k': r['kind'],
            'kc': r['kind_cn'],
            's': r['summonable'] or 'na',
            'db': r['db_category'],
            'u': r['unit_class'],
            'src': r['source'],
        })
    return out


def stats(rows):
    n = len(rows)
    n_sum = sum(1 for r in rows if r['summonable'] == 'yes')
    n_play = sum(1 for r in rows if r['kind_cn'] == '可玩对象(单位/建筑)')
    n_cn = sum(1 for r in rows if r['chinese_name'])
    n_build = sum(1 for r in rows if r['unit_class'] == '建筑')
    n_inf = sum(1 for r in rows if r['unit_class'] == '步兵')
    n_veh = sum(1 for r in rows if r['unit_class'] == '载具')
    # 经可用性筛选后的最终可召唤清单（callable_final.tsv）
    n_final = 0
    if os.path.exists(FINAL):
        with io.open(FINAL, encoding='utf-8') as f:
            n_final = max(0, sum(1 for _ in f) - 1)
    return dict(n=n, n_sum=n_sum, n_play=n_play, n_cn=n_cn, n_final=n_final,
                n_build=n_build, n_inf=n_inf, n_veh=n_veh)


TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RA3 日冕 · 单位代码 | Ra3-Copilot</title>
<meta name="description" content="红色警戒3 日冕 Mod 的单位代码（SAGE 对象类型哈希）总表与提取方法。">
<style>
:root{
  --bg:#0c1016; --bg2:#111823; --pnl:#151d29; --line:#242f3e; --line2:#1b2431;
  --tx:#dfe7f1; --dim:#8b9bad; --dim2:#5d6b7c;
  --red:#e8534a; --red2:#ff7a70; --gold:#e2b33c; --grn:#5fd08a; --blu:#59a9ff;
  --mono:"Cascadia Mono",Consolas,"SFMono-Regular",Menlo,monospace;
}
*{box-sizing:border-box}
html{scroll-behavior:smooth}
body{margin:0;background:var(--bg);color:var(--tx);
  font:15px/1.7 "Segoe UI","Microsoft YaHei",system-ui,-apple-system,sans-serif}
a{color:var(--blu);text-decoration:none}
a:hover{text-decoration:underline}
code,kbd{font-family:var(--mono)}
.wrap{max-width:1180px;margin:0 auto;padding:0 24px}

/* ---------- hero ---------- */
header.hero{position:relative;overflow:hidden;border-bottom:1px solid var(--line);
  background:
    radial-gradient(1100px 380px at 82% -60px, rgba(232,83,74,.20), transparent 60%),
    radial-gradient(900px 420px at 8% 120%, rgba(89,169,255,.12), transparent 60%),
    linear-gradient(180deg,#0e141d,#0c1016)}
.hero .wrap{padding:66px 24px 52px}
.badge{display:inline-flex;align-items:center;gap:8px;font-size:12.5px;color:var(--red2);
  border:1px solid rgba(232,83,74,.35);background:rgba(232,83,74,.08);
  border-radius:999px;padding:5px 13px;letter-spacing:.4px}
.hero h1{margin:18px 0 10px;font-size:40px;line-height:1.15;font-weight:750;letter-spacing:-.5px}
.hero h1 span{color:var(--red)}
.hero .lead{max-width:760px;color:var(--dim);font-size:16px}
.hero .lead b{color:var(--tx)}
.formula{margin:22px 0 0;display:inline-block;font-family:var(--mono);font-size:15px;
  background:#0a0f16;border:1px solid var(--line);border-radius:10px;padding:13px 18px;color:var(--tx)}
.formula em{color:var(--gold);font-style:normal}
.formula i{color:var(--grn);font-style:normal}

/* ---------- stats ---------- */
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin-top:34px}
.card{background:var(--pnl);border:1px solid var(--line);border-radius:12px;padding:16px 18px}
.card .num{font-size:26px;font-weight:700;font-family:var(--mono);letter-spacing:-.5px}
.card .num.g{color:var(--grn)} .card .num.r{color:var(--red2)} .card .num.b{color:var(--blu)}
.card .lbl{color:var(--dim);font-size:12.5px;margin-top:2px}
@media(max-width:760px){.stats{grid-template-columns:repeat(2,1fr)}.hero h1{font-size:30px}}

/* ---------- sections ---------- */
section{padding:56px 0 8px;border-bottom:1px solid var(--line2)}
section h2{font-size:22px;font-weight:700;margin:0 0 6px;letter-spacing:-.2px}
section .hint{color:var(--dim);font-size:14px;margin:0 0 22px}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:20px}
@media(max-width:860px){.grid2{grid-template-columns:1fr}}
.box{background:var(--pnl);border:1px solid var(--line);border-radius:12px;padding:18px 20px}
.box h3{margin:0 0 8px;font-size:15px;color:var(--tx)}
.box p{margin:6px 0;color:var(--dim);font-size:14px}
pre{margin:0;background:#0a0f16;border:1px solid var(--line);border-radius:10px;
  padding:15px 17px;overflow:auto;font-family:var(--mono);font-size:13px;line-height:1.65;color:#cfe0f2}
pre .c{color:var(--dim2)} pre .k{color:var(--red2)} pre .s{color:var(--grn)} pre .f{color:var(--gold)}

/* 流程 */
.flow{display:flex;flex-wrap:wrap;align-items:stretch;gap:10px;margin-top:6px}
.step{flex:1 1 190px;background:var(--pnl);border:1px solid var(--line);border-radius:10px;padding:13px 15px;min-width:180px}
.step .s-n{font-family:var(--mono);font-size:11px;color:var(--red2);letter-spacing:1px}
.step .s-t{font-weight:600;margin:3px 0 4px;font-size:14px}
.step .s-d{color:var(--dim);font-size:12.5px;line-height:1.55}
.step .s-d code{color:var(--gold);font-size:11.5px;word-break:break-all}
.arrow{align-self:center;color:var(--dim2);font-size:18px}

/* 判据 */
table.kind{width:100%;border-collapse:collapse;font-size:13.5px}
table.kind th,table.kind td{text-align:left;padding:8px 10px;border-bottom:1px solid var(--line2)}
table.kind th{color:var(--dim);font-weight:600;font-size:12px;letter-spacing:.4px}
table.kind code{color:var(--gold);font-size:12.5px}

/* ---------- 数据表 ---------- */
.bar{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:4px 0 14px}
input,select{background:#0a0f16;color:var(--tx);border:1px solid var(--line);border-radius:8px;
  padding:9px 12px;font:inherit;font-size:13.5px;outline:none}
input[type=text]{min-width:280px;flex:1}
input:focus,select:focus{border-color:var(--blu)}
label.chk{display:flex;align-items:center;gap:6px;font-size:13px;white-space:nowrap;
  background:#0a0f16;border:1px solid var(--line);border-radius:8px;padding:9px 12px;cursor:pointer}
label.chk input{accent-color:var(--blu)}
.stat{color:var(--dim);font-size:13px;white-space:nowrap}
.stat b{color:var(--tx)}
.tablewrap{border:1px solid var(--line);border-radius:12px;overflow:hidden;max-height:72vh;overflow-y:auto}
table.data{width:100%;border-collapse:collapse}
table.data th{position:sticky;top:0;z-index:2;background:var(--pnl);text-align:left;font-weight:600;
  font-size:11.5px;color:var(--dim);padding:10px 13px;border-bottom:1px solid var(--line);
  letter-spacing:.4px;text-transform:uppercase;white-space:nowrap}
table.data td{padding:7px 13px;border-bottom:1px solid var(--line2);font-size:13px;vertical-align:top}
table.data tbody tr{cursor:pointer}
table.data tbody tr:hover td{background:#17213000}
table.data tbody tr:hover{background:#172130}
tr[data-u="na"]{opacity:.5}
tr[data-u="yes"]{background:#12201a} tr[data-u="yes"]:hover{background:#16281f}
td.code{font-family:var(--mono);color:var(--red2);white-space:nowrap}
td.name{font-family:var(--mono);color:var(--grn);word-break:break-all;max-width:340px}
td.cn,td.fac,td.uc{white-space:nowrap}
td.fac{color:var(--blu)} td.uc{color:var(--gold)} td.tl{color:#7fb3d5;font-size:12px;white-space:nowrap}
td.note{color:var(--dim);font-size:12px;white-space:nowrap}
td.src{color:var(--dim2);font-size:11.5px;white-space:nowrap}
.pill{display:inline-block;font-size:11px;padding:1px 7px;border-radius:999px;border:1px solid var(--line)}
.pill.y{color:var(--grn);border-color:rgba(95,208,138,.4);background:rgba(95,208,138,.08)}

.res{display:flex;flex-wrap:wrap;gap:10px;margin-top:8px}
.res a{display:block;background:var(--pnl);border:1px solid var(--line);border-radius:10px;
  padding:11px 15px;font-size:13.5px}
footer{padding:40px 0 60px;color:var(--dim2);font-size:12.5px;line-height:1.9}
footer b{color:var(--dim)}
.toast{position:fixed;left:50%;bottom:34px;transform:translate(-50%,20px);opacity:0;
  background:#1c2736;border:1px solid var(--line);color:var(--tx);border-radius:999px;
  padding:9px 18px;font-size:13px;transition:.25s;pointer-events:none;z-index:50}
.toast.on{opacity:1;transform:translate(-50%,0)}
</style>
</head>
<body>

<header class="hero">
  <div class="wrap">
    <span class="badge">◆ Ra3-Copilot · 单位代码研究</span>
    <h1>红色警戒 3 日冕 <span>单位代码</span>总表</h1>
    <p class="lead">
      游戏里每个单位 / 建筑 / 对象都有一个 32 位 <b>对象类型哈希</b>，创建对象时用它查模板。
      本项目把 <b>全量 __N__ 条</b>代码整理成可离线复算的表 ——
      代码不再需要逐个查，<b>用名字就能算出来</b>。
    </p>
    <div class="formula">代码 = <em>StringHasher.Hash</em>( <i>对象模板名</i>.lower() )</div>

    <div class="stats">
      <div class="card"><div class="num b">__N__</div><div class="lbl">全量对象代码</div></div>
      <div class="card"><div class="num g">__FINAL__</div><div class="lbl">可召唤且能用（已筛）</div></div>
      <div class="card"><div class="num r">__PLAY__</div><div class="lbl">可玩对象（单位/建筑）</div></div>
      <div class="card"><div class="num">__CN__</div><div class="lbl">带中文名</div></div>
    </div>
  </div>
</header>

<div class="wrap">

<section id="algo">
  <h2>哈希算法</h2>
  <p class="hint">代码不是随便编的随机数 —— 它是 SAGE 引擎对「对象模板名」做的一次字符串哈希。</p>
  <div class="grid2">
    <div class="box">
      <h3>三条要点</h3>
      <p><b>① 先小写。</b>哈希不区分大小写，<code>AlliedBarracks</code> 与 <code>ALLIEDBARRACKS</code> 同码。</p>
      <p><b>② 是 SuperFastHash 的变体</b>（Paul Hsieh 算法），不是 CRC32 / FNV / DJB2 ——
         这就是早期"算不出来、只能查表"的原因。</p>
      <p><b>③ 32 位无符号结果</b>，一般写成 <code>0x</code> 十六进制。</p>
    </div>
    <div class="box">
      <h3>既有实现（自检 408/408 全中）</h3>
      <pre><span class="c">// 代码 = hash(name.lower())</span>
<span class="k">uint32_t</span> <span class="f">StringHasher_Hash</span>(<span class="k">const char</span>* s){
  <span class="k">uint32_t</span> h = len;
  <span class="k">for</span> (每 2 字节) {              <span class="c">// SuperFastHash 主体</span>
    h += lo; h ^= h&lt;&lt;<span class="s">16</span>; h *= <span class="s">0x21f0aaad</span>;
    h ^= h&gt;&gt;<span class="s">15</span>;
    h += hi; h ^= h&lt;&lt;<span class="s">13</span>; h *= <span class="s">0x2c1b3c6d</span>;
    h ^= h&gt;&gt;<span class="s">16</span>;
  }
  <span class="c">// 尾部按剩余字节数做 avalanche</span>
}</pre>
      <p>完整实现见 <code>tools/sage_hash.py</code>。</p>
    </div>
  </div>
</section>

<section id="usable">
  <h2>「能召唤」≠「召唤出来能用」</h2>
  <p class="hint">最容易踩的坑：模板能创建出来，不代表它在游戏里可用。</p>
  <div class="grid2">
    <div class="box">
      <pre>波塞冬巡洋舰
  0x9356389F  AlliedAntiNavyShipTech3     <span class="s">✓ 正常召唤可用</span>
  0xDB5A5941  AlliedChronoBeacon_Poseidon <span class="k">✗ 能创建，但不可选、不可用</span></pre>
      <p>两个模板运行时都能在模板库里找到，所以光靠"能不能创建"筛不出来。</p>
    </div>
    <div class="box">
      <h3>判据：官方图鉴的 <code>kind_of</code> 位掩码</h3>
      <table class="kind">
        <tr><th>标志</th><th>含义</th></tr>
        <tr><td>含 <code>SELECTABLE</code></td><td>玩家能选中、能操作 = <b>可用</b></td></tr>
        <tr><td><code>IGNORES_SELECT_ALL</code></td><td>超时空传送 / 空投的抵达体</td></tr>
        <tr><td><code>TRANSPORT</code></td><td>运输 / 搭载体</td></tr>
        <tr><td><code>SUMMONED</code>+<code>UNATTACKABLE</code></td><td>技能召唤的临时装置</td></tr>
      </table>
    </div>
  </div>
</section>

<section id="pipeline">
  <h2>提取流程</h2>
  <p class="hint">整条链路都是可复现的 —— 从官方数据到修改器用的头文件，每一步都有脚本。</p>
  <div class="flow">
    <div class="step"><div class="s-n">STEP 1</div><div class="s-t">官方图鉴</div>
      <div class="s-d">日冕官方单位图鉴数据<br><code>cor-games.com</code> 的 <code>game_objects.jsonl</code></div></div>
    <div class="arrow">→</div>
    <div class="step"><div class="s-n">STEP 2</div><div class="s-t">名字 → 代码</div>
      <div class="s-d">用 SAGE 哈希算法复算<br><code>tools/sage_hash.py</code>（自检全中）</div></div>
    <div class="arrow">→</div>
    <div class="step"><div class="s-n">STEP 3</div><div class="s-t">全量清单</div>
      <div class="s-d">合并清单 / 图鉴 / 字符串表<br><code>tools/gen_unit_codes.py</code></div></div>
    <div class="arrow">→</div>
    <div class="step"><div class="s-n">STEP 4</div><div class="s-t">运行时实测</div>
      <div class="s-d">逐条查模板是否存在<br><code>probe_callable.py</code></div></div>
    <div class="arrow">→</div>
    <div class="step"><div class="s-n">STEP 5</div><div class="s-t">可用性过滤</div>
      <div class="s-d">按 <code>SELECTABLE</code> 判据剔除<br><code>tools/filter_usable.py</code></div></div>
    <div class="arrow">→</div>
    <div class="step"><div class="s-n">STEP 6</div><div class="s-t">生成头文件</div>
      <div class="s-d">供修改器直接查表<br><code>units_corona.h</code></div></div>
  </div>
</section>

<section id="browse">
  <h2>浏览全量代码</h2>
  <p class="hint">点任意一行可复制它的代码。搜索支持模板名 / 中文名 / 十六进制代码。</p>
  <div class="bar">
    <input type="text" id="q" placeholder="搜索：模板名 / 中文名 / 0x9356389F ..." autocomplete="off">
    <select id="f"><option value="">全部阵营</option>
      <option>神州</option><option>盟军</option><option>苏联</option><option>帝国</option>
      <option>中立</option><option>平民</option><option>特殊</option></select>
    <select id="k"><option value="">全部类别</option>
      <option value="建筑">建筑</option><option value="步兵">步兵</option><option value="载具">载具</option>
      <option value="舰船">舰船</option><option value="飞行器">飞行器</option><option value="箱子">箱子</option>
      <option value="其他">其他</option></select>
    <select id="u">
      <option value="">可召唤性（全部）</option>
      <option value="yes">表内标记：可召唤</option>
      <option value="no">标记：不可用</option>
      <option value="na">未标记</option>
    </select>
    <label class="chk"><input type="checkbox" id="p"> 只看基准形态</label>
    <span class="stat" id="st"></span>
  </div>
  <div class="tablewrap">
    <table class="data">
      <thead><tr><th>代码</th><th>模板名 (ObjectTemplate)</th><th>中文名</th>
        <th>阵营</th><th>类型</th><th>面板行</th><th>备注</th></tr></thead>
      <tbody id="tb"></tbody>
    </table>
  </div>
</section>

<section id="links">
  <h2>数据与工具</h2>
  <p class="hint">完整报告、命令行小工具、C++ 头文件都在仓库的 <code>unit-codes/</code> 目录。</p>
  <div class="res">
    <a href="../README.md">unit-codes/README.md · 目录索引与结论</a>
    <a href="../单位代码与哈希算法.md">单位代码与哈希算法.md · 完整研究报告</a>
    <a href="../tools/sage_hash.py">tools/sage_hash.py · 哈希既有实现</a>
    <a href="../tools/unitcode.py">tools/unitcode.py · 命令行查询</a>
    <a href="../data/callable_final.tsv">data/callable_final.tsv · 可召唤清单</a>
  </div>
</section>

<footer>
  <div>
    <b>Ra3-Copilot</b> · 单位代码研究。数据由 <code>tools/gen_site.py</code> 从
    <code>data/unit_codes_all.tsv</code> 生成。<br>
    哈希算法出处：SAGE 引擎的 <code>StringHasher</code>（SuperFastHash 变体）。
    仅供学习与研究使用；本页只收录代码哈希与名称这类事实性数据，不含游戏素材。
  </div>
</footer>

</div>
<div class="toast" id="toast"></div>
<script src="data.js"></script>
<script>
const ROWS = window.RA3_UNITS || [];
const tb = document.getElementById('tb'), q = document.getElementById('q'),
      fs = document.getElementById('f'), ks = document.getElementById('k'),
      us = document.getElementById('u'), ps = document.getElementById('p'),
      st = document.getElementById('st'), toast = document.getElementById('toast');
const fmt = document.createElement('template');

function esc(s){return (s==null?'':String(s)).replace(/[&<>"]/g, c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}

function render(){
  const parts = [];
  for (const r of ROWS){
    const hl = (r.n+' '+r.cn+' '+r.fam+' '+r.c).toLowerCase();
    parts.push(
      '<tr data-u="'+esc(r.s)+'" data-h="'+esc(hl)+'" data-f="'+esc(r.f)+'" '+
      'data-k="'+esc(r.u)+'" data-p="'+esc(r.p)+'" title="点击复制代码">'+
      '<td class="code">'+esc(r.c)+'</td>'+
      '<td class="name">'+esc(r.n)+'</td>'+
      '<td class="cn">'+esc(r.cn)+'</td>'+
      '<td class="fac">'+esc(r.f)+'</td>'+
      '<td class="uc">'+esc(r.u)+'</td>'+
      '<td class="tl">'+esc(r.t)+'</td>'+
      '<td class="note">'+esc(r.note)+'</td></tr>');
  }
  tb.innerHTML = parts.join('');
}
render();
const all = [...tb.rows];

function run(){
  const key = q.value.trim().toLowerCase(), f = fs.value, k = ks.value,
        u = us.value, onlyP = ps.checked;
  let n = 0;
  for (const tr of all){
    const ok = (!key || tr.dataset.h.includes(key))
            && (!f || tr.dataset.f === f)
            && (!k || tr.dataset.k === k)
            && (!u || tr.dataset.u === u)
            && (!onlyP || tr.dataset.p === '1');
    tr.style.display = ok ? '' : 'none';
    if (ok) n++;
  }
  st.innerHTML = '显示 <b>' + n + '</b> / ' + all.length;
}
q.addEventListener('input', run); fs.addEventListener('change', run);
ks.addEventListener('change', run); us.addEventListener('change', run);
ps.addEventListener('change', run); run();

function showToast(msg){
  toast.textContent = msg; toast.classList.add('on');
  clearTimeout(showToast._t);
  showToast._t = setTimeout(()=>toast.classList.remove('on'), 1200);
}
tb.addEventListener('click', e=>{
  const tr = e.target.closest('tr'); if (!tr) return;
  const code = tr.querySelector('td.code').textContent.trim();
  if (navigator.clipboard) navigator.clipboard.writeText(code).then(
    ()=>showToast('已复制 '+code), ()=>showToast(code));
  else showToast(code);
});
</script>
</body>
</html>
"""


def main():
    rows = load_rows()
    os.makedirs(OUTDIR, exist_ok=True)

    data = build_data_json(rows)
    js = 'window.RA3_UNITS = ' + json.dumps(data, ensure_ascii=False, separators=(',', ':')) + ';\n'
    with io.open(os.path.join(OUTDIR, 'data.js'), 'w', encoding='utf-8') as f:
        f.write(js)

    s = stats(rows)
    doc = (TEMPLATE
           .replace('__N__', str(s['n']))
           .replace('__FINAL__', str(s['n_final']))
           .replace('__SUM__', str(s['n_sum']))
           .replace('__PLAY__', str(s['n_play']))
           .replace('__CN__', str(s['n_cn'])))
    with io.open(os.path.join(OUTDIR, 'index.html'), 'w', encoding='utf-8') as f:
        f.write(doc)

    print('wrote site/index.html + site/data.js')
    print('  rows=%d final=%d marked=%d playable=%d cn_named=%d (build=%d inf=%d veh=%d)'
          % (s['n'], s['n_final'], s['n_sum'], s['n_play'], s['n_cn'],
             s['n_build'], s['n_inf'], s['n_veh']))


if __name__ == '__main__':
    main()
