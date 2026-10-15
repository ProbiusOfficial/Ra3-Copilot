"""Build a self-contained, searchable HTML browser for the RA3 unit-code table."""
import os, sys, csv, io, html

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE = os.path.abspath(os.path.join(HERE, '..'))
SRC = os.path.join(ARCHIVE, 'data', 'unit_codes_all.tsv')
OUT = os.path.join(ARCHIVE, 'out', 'ra3_unit_codes.html')

with io.open(SRC, encoding='utf-8') as f:
    rows = list(csv.DictReader(f, delimiter='\t'))

FAC_ORDER = {'神州': 0, '盟军': 1, '苏联': 2, '帝国': 3, '中立': 4, '平民': 5, '特殊': 6, '': 9}
CLASS_ORDER = {'建筑': 0, '步兵': 1, '载具': 2, '舰船': 3, '飞行器': 4, '箱子': 5, '其他': 6, '': 9}
rows.sort(key=lambda r: (0 if r['summonable'] == 'yes' else 1,
                         FAC_ORDER.get(r['faction'], 8),
                         CLASS_ORDER.get(r['unit_class'], 9),
                         0 if r['is_primary'] == '1' else 1,
                         r['name'].lower()))

kinds = sorted({r['kind'] for r in rows}, key=lambda k: -sum(1 for r in rows if r['kind'] == k))
KIND_CN = {r['kind']: r['kind_cn'] for r in rows}
kopt = '\n'.join('<option value="%s">%s</option>' % (html.escape(k), html.escape(KIND_CN.get(k, k)))
                 for k in kinds)

TR = []
for r in rows:
    TR.append(
        '<tr data-f="{f}" data-n="{n}" data-p="{p}" data-k="{k}" data-u="{u}">'
        '<td class="code">{c}</td><td class="name">{nm}</td><td class="cn">{cn}</td>'
        '<td class="fac">{f}</td><td class="uc">{uc}</td><td class="tl">{tl}</td>'
        '<td class="note">{nt}</td><td class="srcc">{s}</td></tr>'.format(
            c=r['code_hex'], nm=html.escape(r['name']), cn=html.escape(r['chinese_name']),
            f=html.escape(r['faction']), uc=html.escape(r['unit_class']),
            tl=html.escape(r['type_line']), nt=html.escape(r['note']),
            s=html.escape(r['source']), k=html.escape(r['kind']), u=r['summonable'] or 'na',
            p=r['is_primary'],
            n=html.escape((r['name'] + ' ' + r['chinese_name'] + ' ' + r['family']).lower())))
TR = '\n'.join(TR)

n_sum = sum(1 for r in rows if r['summonable'] == 'yes')
n_unit = sum(1 for r in rows if r['kind'] == 'unit')
n_cn = sum(1 for r in rows if r['chinese_name'])

html_doc = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RA3 日冕 · 单位代码总表</title>
<style>
:root{--bg:#0f1419;--pnl:#161d26;--line:#26303d;--tx:#dbe4ee;--dim:#8b9aab;--acc:#4da3ff;--red:#ff6b6b;--grn:#5fd08a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);
 font:14px/1.55 "Segoe UI","Microsoft YaHei",system-ui,sans-serif}
header{padding:22px 26px 16px;border-bottom:1px solid var(--line);background:var(--pnl)}
h1{margin:0 0 6px;font-size:19px;font-weight:600;letter-spacing:.3px}
.sub{color:var(--dim);font-size:12.5px;line-height:1.7}
.sub code{background:#0b1017;border:1px solid var(--line);border-radius:3px;padding:1px 5px;color:var(--acc)}
.sub b{color:var(--tx)}
.bar{display:flex;gap:10px;flex-wrap:wrap;align-items:center;padding:14px 26px;border-bottom:1px solid var(--line);
 position:sticky;top:0;background:rgba(15,20,25,.95);backdrop-filter:blur(6px);z-index:5}
input,select{background:#0b1017;color:var(--tx);border:1px solid var(--line);border-radius:6px;
 padding:8px 11px;font:inherit;font-size:13px;outline:none}
input[type=text]{min-width:260px;flex:1}
input:focus,select:focus{border-color:var(--acc)}
label.chk{display:flex;align-items:center;gap:6px;font-size:13px;white-space:nowrap;
 background:#0b1017;border:1px solid var(--line);border-radius:6px;padding:8px 11px;cursor:pointer}
label.chk input{accent-color:var(--acc)}
.stat{color:var(--dim);font-size:12.5px;white-space:nowrap}
.stat b{color:var(--tx)}
table{width:100%;border-collapse:collapse}
th{position:sticky;top:62px;background:var(--pnl);text-align:left;font-weight:600;font-size:12px;
 color:var(--dim);padding:9px 14px;border-bottom:1px solid var(--line);letter-spacing:.4px;text-transform:uppercase}
td{padding:7px 14px;border-bottom:1px solid #1c2530;font-size:13px;vertical-align:top}
tr:hover td{background:#1a2330}
tr[data-u="na"] td{opacity:.55}
tr[data-u="yes"] td{background:#131c17}
tr[data-u="yes"]:hover td{background:#18251d}
.code{font-family:"Cascadia Mono",Consolas,monospace;color:var(--red);white-space:nowrap}
.name{font-family:"Cascadia Mono",Consolas,monospace;color:var(--grn);word-break:break-all}
.cn,.fac,.uc{white-space:nowrap}
.fac{color:var(--acc)}
.uc{color:#c9a227}
.tl{color:#7fb3d5;font-size:12px;white-space:nowrap}
.note{color:#8b9aab;font-size:12px;white-space:nowrap}
.srcc{color:#5d6b7c;font-size:11.5px;white-space:nowrap}
footer{padding:16px 26px 40px;color:#5d6b7c;font-size:12px;line-height:1.8}
footer code{color:var(--acc)}
</style></head><body>
<header>
  <h1>RA3 日冕 · 单位 / 对象代码总表</h1>
  <div class="sub">
    共 <b>__N__</b> 条，其中<b>可召唤且能用</b> <b>__SUM__</b> 条（绿底）、可玩对象 <b>__UNIT__</b> 条、带中文名 <b>__CN__</b> 条。<br>
    代码 = SAGE 对象类型哈希 = <code>StringHasher.Hash(模板名.lower())</code>（SuperFastHash 变体），
    已用 439 条实机 / 官方数据交叉核对。
  </div>
</header>
<div class="bar">
  <input type="text" id="q" placeholder="搜索：模板名 / 中文名 / 0x9356389F ..." autocomplete="off">
  <select id="f"><option value="">全部阵营</option>
    <option>神州</option><option>盟军</option><option>苏联</option><option>帝国</option>
    <option>中立</option><option>平民</option><option>特殊</option></select>
  <select id="k"><option value="">全部类别</option>
__KOPT__
  </select>
  <select id="u">
    <option value="">可召唤性(全部)</option>
    <option value="yes">可召唤且能用</option>
    <option value="no">召唤出来不可用</option>
    <option value="na">未在官方库核验</option>
  </select>
  <label class="chk"><input type="checkbox" id="p"> 只看基准形态</label>
  <span class="stat" id="st"></span>
</div>
<table><thead><tr><th>代码</th><th>模板名 (ObjectTemplate)</th><th>中文名</th><th>阵营</th>
<th>类型</th><th>面板类型行</th><th>备注</th><th>来源</th></tr></thead>
<tbody id="tb">__ROWS__</tbody></table>
<footer>
  <b>「可召唤且能用」怎么判的？</b>官方单位图鉴数据里 <code>kind_of</code> 含 <code>SELECTABLE</code> →
  玩家能选中、能操作。缺 <code>SELECTABLE</code> 的（如 <code>AlliedChronoBeacon_Poseidon</code> 超时空传送抵达体、
  <code>CelestialAirDrop_Land</code> 空投运输体）会创建出来但不可选、不可用，已剔除。<br>
  <b>运行时实测（<code>ThingFactory::Find</code>）只证明模板存在</b>，分不出这个差别 ——
  波塞冬巡洋舰的两个模板当时都能 resolve，所以必须叠加这条判据。
</footer>
<script>
const tb=document.getElementById('tb'),q=document.getElementById('q'),
      fs=document.getElementById('f'),ks=document.getElementById('k'),
      us=document.getElementById('u'),ps=document.getElementById('p'),st=document.getElementById('st');
const all=[...tb.rows];
function run(){
  const k=q.value.trim().toLowerCase(),f=fs.value,kk=ks.value,uu=us.value,onlyP=ps.checked;
  let n=0;
  for(const r of all){
    const ok=(!k||r.dataset.n.includes(k)||r.cells[0].textContent.toLowerCase().includes(k))
           &&(!f||r.dataset.f===f)&&(!kk||r.dataset.k===kk)&&(!uu||r.dataset.u===uu)
           &&(!onlyP||r.dataset.p==='1');
    r.style.display=ok?'':'none'; if(ok)n++;
  }
  st.innerHTML='显示 <b>'+n+'</b> / '+all.length;
}
q.addEventListener('input',run);fs.addEventListener('change',run);ks.addEventListener('change',run);
us.addEventListener('change',run);ps.addEventListener('change',run);run();
</script></body></html>
"""

html_doc = (html_doc.replace('__ROWS__', TR).replace('__KOPT__', kopt)
            .replace('__N__', str(len(rows))).replace('__SUM__', str(n_sum))
            .replace('__UNIT__', str(n_unit)).replace('__CN__', str(n_cn)))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
io.open(OUT, 'w', encoding='utf-8').write(html_doc)
print('wrote %s  (%d rows: %d summonable, %d playable, %d with cn name)'
      % (OUT, len(rows), n_sum, n_unit, n_cn))
