# RA3 日冕 · 单位代码（对象类型哈希）

> 红警3 日冕 Mod 修改器用到的**单位代码**的全部研究、数据和工具。
> 一句话结论：**单位代码 = SAGE 对象类型哈希 = `StringHasher.Hash(对象模板名.lower())`**，
> 已用 408 条运行时实测 + 社区《增援代码》表 + 官方单位图鉴三方交叉核对通过，**可以离线算，不再需要查表**。

当前状态（2026-10-15）：**可召唤且能用 = 408 条单位 + 54 个箱子**，已刷新进 `trainer-v2/src/units_corona.h`。

> 🌐 **浏览站点**：直接浏览器打开 `site/index.html`（纯静态，离线可用），可按阵营 / 类别 / 可召唤性筛选全量 5461 条。

---

## 1. 目录索引

```
unit-codes/
├── README.md                     ← 你正在看的这份
├── 单位代码与哈希算法.md           ← 完整研究报告（原理 / 验证 / 形态 / 场景物件 / 社区资源）
├── site/                         ★ 项目网站（纯静态，浏览器打开 index.html 即可）
│   ├── index.html                   介绍 / 算法 / 提取流程 / 可搜索总表
│   └── data.js                      单位代码数据（由 tools/gen_site.py 生成）
├── data/
│   ├── callable_final.tsv        ★ 修改器当前使用的召唤清单（运行时实测 ∩ 可用性筛选）
│   ├── callable_dropped.tsv      被剔除的条目 + 原因（留档，别删）
│   ├── callable_probe.tsv        逐条实测结果（ThingFactory::Find / HTTP /resolve）
│   ├── unit_codes_trainer.tsv|h  全量候选（未筛可用性）
│   ├── unit_codes_all.tsv        ★ 全量 5461 条（含场景/特效/投射物，带 summonable 列）
│   └── source/  （本地，不入库）   原始素材：官方图鉴 jsonl + 从 .big 抽出的 manifest/str/csf
│                                   版权材料，由 tools/ 重新生成，见 §4.2
├── tools/
│   ├── sage_hash.py              哈希算法既有实现（含自检）
│   ├── unitcode.py               命令行小工具
│   ├── official_db.py            官方图鉴数据访问器（含「可用性」判据）
│   ├── filter_usable.py          ★ 给召唤清单做可用性过滤
│   ├── fetch_official.py         ★ 重新拉取官方图鉴数据
│   ├── gen_unit_codes.py         生成 data/*.tsv|h
│   ├── gen_site.py               ★ 生成 site/（项目网站）
│   ├── csf_read2.py              RA3 CSF 字符串表解析器（值是按位取反的 UTF-16LE）
│   ├── big_ls.py                 BIG4 归档目录/统计
│   ├── scan_object_names.py      在 BIG 里按字节搜名字
│   └── strings_dump.py           从文件里提取可打印字符串
```

> **原始素材不入库**：`data/source/` 里的清单/字符串由游戏 `.big` 解出、图鉴数据来自官方站点，
> 均属第三方版权材料，不随仓库分发；用 `tools/` 里的脚本可自行重新生成。
> `data/*.json`（体积较大的派生副本）同理不入库，权威数据以 `.tsv` 为准。

---

## 2. 快速上手

```bash
cd unit-codes/tools

# 名字算代码（大小写随便写，算法内部先小写）
python unitcode.py CelestialDF41                 # -> 0x25241CE6
python unitcode.py AlliedAntiNavyShipTech3       # -> 0x9356389F  （波塞冬巡洋舰，可用）

# 代码反查
python unitcode.py --hex 0xDB5A5941              # -> AlliedChronoBeacon_Poseidon（不可用，见下）

# 模糊搜索（模板名 / 中文名 / 家族名）
python unitcode.py --find 波塞冬                  # 三条都列出来，标了 usable=yes/no
python unitcode.py --find 天启 --usable           # 只看"可召唤且能用"的

# 自检（必须全中）
python sage_hash.py                              # SAGE hash 复算通过 : 408/408
```

C++ 里直接查表：`#include "unit_codes_trainer.h"`（见第 4 节）。

---

## 3. 核心结论

### 3.1 算法

```
code = StringHasher.Hash( 对象模板名.lower() )     // 32 位无符号
```

* Paul Hsieh **SuperFastHash 的一个变体**，不是 CRC32 / FNV / DJB2 ——
  这就是过去"算不出来、只能查表"的原因。
* **必须先小写**。哈希不区分大小写，所以 `AlliedBarracks` 与 `ALLIEDBARRACKS` 结果相同。
* 出处：[WrathEd2012](https://github.com/Qibbi/WrathEd2012) 的 `SAGE/StringHasher.cs`。

### 3.2 验证（408/408）

| 证据 | 结果 |
|---|---|
| `callable_final.tsv` 里 408 条**运行时实测**过的 名字→代码 | **全部复算命中** |
| 社区《增援代码》表（盟军/苏联/帝国全量） | 逐条一致 |
| 官方单位图鉴 `editor_name` ↔ 我们算出的代码 | 逐条一致（含用户实测的 `AlliedAntiNavyShipTech3` = 0x9356389F） |

### 3.3 ★★★ "能召唤" ≠ "召唤出来能用"

**这是最容易踩的坑，也是 2026-10-15 用户实测发现的**：

```
波塞冬巡洋舰  0x9356389F  AlliedAntiNavyShipTech3       ✅ 能正常召唤出来用
              0xDB5A5941  AlliedChronoBeacon_Poseidon    ❌ 能创建出来，但不可选、不可用
```

两个模板在运行时**都能通过 `ThingFactory::Find`（HTTP `/resolve`）**，所以光靠实测筛不出来。

**判据 = 官方单位图鉴里的 `kind_of` 位掩码**：

* 含 **`SELECTABLE`** → 玩家能选中、能操作 = **可用**
* 缺 `SELECTABLE` → 召唤出来是个"壳"，典型几类：

| 特征标志位 | 是什么 | 例 |
|---|---|---|
| `IGNORES_SELECT_ALL` | 超时空传送 / 空投的**抵达体** | `AlliedChronoBeacon_*`（21 个）、`SovietTeslaAirAssault*` |
| `TRANSPORT` | 运输 / 搭载体 | `CelestialAirDrop_Land` |
| `SUMMONED` + `UNATTACKABLE` | 技能/超武召唤的临时装置、建筑子物件 | `AlliedPowerPlantTurbine` |
| （无标志） | 自杀 / 俯冲类特攻机 | `JapanYumiAircraft`（飞燕特攻机） |

本次据此从 441 条实测清单里**剔除 31 条**，剩 **410 条**。

### 3.4 同一个单位为什么有好几个代码

每个「形态」都是**各自独立的 ObjectTemplate**：`_Naval`/`_Water`/`_Ground`/`_Air`（两栖/变形介质形态）、
`_Enhanced`（强化）、`_Dropped`/`AirDrop`（空投）、`_HeavyTransport`（搭载态）、`_AI`/`_NoReplace`、
`ChronoBeacon_*`（超时空落点）、`_Movie`/`S0x_*`（战役一次性）、`Firework`/`_EMC`/`_2024A~D`/`_Old`
（彩蛋/活动/残留）、`WithTrailSomke`（挂点特效）等。

**找"基准形态"（UI 去重用）的判据 = `DESC:` 键**：同一家族里只有会出现在建造/生产面板的
基础模板有 `DESC:`（面板说明文字），所有变体都没有。命中率 ≈ 90%。

### 3.5 "没有中文名"的那一大片是什么

主要是 **2400+ 条地图场景 / 中立物**（`XX_` 前缀 = 某张图的房子/广告牌/集装箱/树/道具，
39 个地区前缀如 `ST_`斯大林格勒 / `TH_`东京湾 / `HB_`哈瓦那 / `NY_`纽约 …），
以及残骸、投射物、特效生成器、桥部件、战役专用对象 —— 它们在游戏里**从不显示名字**。
它们照样可以召唤（名字只存在哈希）。

---

## 4. 修改器怎么用这份数据

### 4.1 当前链路

```
cor-games.com/data/game_objects/game_objects.jsonl     ← 官方单位图鉴（官方数据源）
        │  fetch_official.py
        ▼
unit-codes/data/source/game_objects.jsonl
        │
        ├─ gen_unit_codes.py ─→ unit_codes_all.tsv / unit_codes_trainer.tsv（全量候选）
        │
        │  trainer-v2/tools/probe_callable.py     ← 逐条 ThingFactory::Find 实测"模板存不存在"
        │  trainer-v2/tools/fix_callable.py       ← 变体替换 + ★ 可用性过滤（SELECTABLE）
        ▼
unit-codes/data/callable_final.tsv        （410 条：能创建 + 能用）
        │  trainer-v2/tools/gen_units_corona.py
        ▼
trainer-v2/src/units_corona.h             （408 单位 + 54 箱子）
```

### 4.2 单独重跑某一步

```bash
# 只用官方数据重建全量表 + 网站（不需要游戏）
cd unit-codes/tools
python fetch_official.py --proxy http://127.0.0.1:7890   # 可选：拉最新图鉴
python gen_unit_codes.py
python gen_site.py                                      # 生成 site/index.html + site/data.js

# 给现有召唤清单再做一次可用性过滤（不需要游戏）
python filter_usable.py            # 就地过滤 callable_final.tsv，原文件自动备份

# 需要游戏运行 + 修改器已注入
cd ../../trainer-v2/tools
python probe_callable.py           # 逐条实测
python fix_callable.py             # 变体替换 + 可用性过滤
python gen_units_corona.py         # 刷新 units_corona.h
```

### 4.3 C++ 侧

```cpp
#include "unit_codes_trainer.h"
// struct TrainerUnit { faction, name(中文), id(模板名), cls(类型), code(哈希), primary }
for (int i = 0; i < ra3::kTrainerUnitCount; ++i) { ... }
```

`units_corona.h` 里的 `SupportUnit` 多带一列拼音（`py`，UI 拼音搜索用），
两者都是 `gen_units_corona.py` 的产物，**优先用 `units_corona.h`**。

### 4.4 建议的后续改造

* 给 HTTP 调试接口（`127.0.0.1:17832`）加 `GET /unitcode?name=xxx`，把哈希函数暴露出来。
* 面板加"输入单位名 → 召唤"，立刻支持全量 5461 个对象。

---

## 5. 注意事项

* **代码是权威，名字只是参考。** 名字来自多个来源，大小写会有差异；哈希不区分大小写。
* `data/source/data_mod.str` 来自 **英文**资源包，个别条目中文名会是英文（生成器已优先取中文）。
* **不要为了"去重"去删游戏里的对象定义。** SAGE 的引用是哈希，删了定义后引用它的
  CommandSet / OCL / 升级 / 战役脚本**不报错、直接崩**。去重只做展示层。
* `../work/hash_pairs.json`（72 条）是早期**名字↔代码错位**的废数据，已作废。
* `callable_dropped.tsv` 是被判定"召唤出来不能用"而剔除的条目，**留档用，不要当垃圾删掉** ——
  里面每一条都写了原因，将来若发现误判可以回捞。

---

## 6. 社区资源

| 资源 | 用途 |
|---|---|
| [cor-games.com/units](https://cor-games.com/units/) | 日冕官方**单位图鉴**（数据：`/data/game_objects/game_objects.jsonl`）—— 本目录的权威数据源 |
| [WrathEd2012](https://github.com/Qibbi/WrathEd2012) | SAGE `.manifest/.bin/.relo/.imp` 编译/反编译；**哈希算法出处** `SAGE/StringHasher.cs` |
| WrathEd 1.07 Gamma / 1.10_HN | GUI 版，可直接浏览 `StaticStream.big` 里 manifest 的全部对象 XML |
| [《增援代码》](https://redalert3-armorrush.fandom.com/zh/wiki/增援代码) | 盟军/苏联/帝国全量单位代码（交叉验证用） |
| [OpenSAGE](https://github.com/OpenSAGE/OpenSAGE) | SAGE 引擎开源复刻，`.big/.map/.w3d/RefPack` 解析参考 |
| [RA3BattleNet](https://github.com/RA3BattleNet) | 国内红警 3 战网（Content / Metadata 仓库） |
| PPM / CNCNZ | 工具合集、Mod SDK、Jonwil 的 SAGE 源码片段 |
| B站《解码红警3》 | 中文向的红警 3 XML 数据科普 |

---

## 7. 已知限制

* 名册是**"所有已知来源的并集"**：原版对象的完整 XML 定义没有随游戏发布，
  只能从官方图鉴 + manifest 符号表 + 字符串表 `NAME:`/`TYPE:` 键反推。
  极少数从不被引用、也没有字符串条目的组件级对象拿不到名字 —— 但它们没有任何 UI 入口，
  不影响修改器。
* 原版对象名在字符串表里是全大写；已尽量还原驼峰，还原不完整的保持大写（不臆造拼写）。
* 可用性判据依赖官方图鉴数据；**没被官方图鉴收录的对象无法核验**，
  目前这类（4 条）按"保守保留"处理并标注。
* 仍有少量真单位在运行时 `ThingFactory::Find` 找不到（拼写修正版也不行），
  判定为 mod 未启用定义，待查。
