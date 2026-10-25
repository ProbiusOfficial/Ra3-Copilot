# AI 接入指南 —— 用脚本/智能体操作本修改器

> 面向**任意 AI 智能体或脚本**。读完本文即可接入，无需其它文档。
> 前提：修改器 DLL 已注入游戏，进程内 HTTP 接口 `http://127.0.0.1:17832` 已在监听（注入即启动）。

> ⚠️ **本机单机接口**：只绑 `127.0.0.1`，仅限单人离线对局。联机/对战使用会破坏公平性，禁止。

---

## 0. 30 秒上手

```bash
curl http://127.0.0.1:17832/ping       # -> pong
curl http://127.0.0.1:17832/snapshot   # -> 战场态势 JSON
```

`/ping` 通 = 游戏在跑、DLL 在跑、接口可用。不通则先确认游戏已启动并注入。

---

## 1. 两条接入路径

| 路径 | 适合 | 特点 |
|---|---|---|
| **MCP（推荐）** | 支持 MCP 协议的客户端 | 配一个 `mcpServers` 条目即可，得到 19 个工具（含参数校验、中文名检索）；其中 `raw` 与 HTTP 直连等价，`op=get/post` 可调任意路由，另有内存读写与按 RVA 调函数 |
| **HTTP 直连** | 任意语言 / 脚本 / Agent 框架 | 直接 `GET/POST 127.0.0.1:17832`，最灵活、无中间层 |

两条路径**等价**：MCP 服务器 `mcp/ra3_mcp.py` 就是一个零依赖的 Python 包装，内部全是对该 HTTP 接口的调用。

### 1.1 MCP 客户端配置

```json
{
  "mcpServers": {
    "ra3-copilot": {
      "command": "python",
      "args": ["<绝对路径>\\ra3-copilot\\mcp\\ra3_mcp.py"]
    }
  }
}
```

游戏内「脑机接口」面板提供**一键复制**的配置片段（路径自动填好，见 §7）。

### 1.2 HTTP 直连

无需任何 SDK。所有端点均为无状态 GET/POST；返回纯文本或 JSON。示例见 §5。

---

## 2. 端点总表

### 2.1 只读端点

| 端点 | 作用 |
|---|---|
| `GET /ping` | 存活探测 → `pong` |
| `GET /state` / `/diag` | 状态 + 符号解析 + 相机 + 落点 + 选中单位结构（排障用） |
| `GET /log?n=40` | 事件日志尾部 |
| `GET /read?addr=0x..&n=128` | 内存十六进制转储 |
| `GET /peek?addr=0x..&type=u32\|f32\|u8\|u16\|ptr` | 单值读取 |
| `GET /resolve?hash=0x..` | 类型哈希 → 模板指针（**召唤前预检**） |
| `GET /help` | 全部端点文本说明 |
| `GET /snapshot[?fresh=1]` | **战场快照**：资金 / 我方 / 敌方各单位数+构成 / 选中 / 扫描统计 |
| `GET /objects[?q=&owner=&cls=&cat=&limit=40]` | **活体对象列表**（Thing 地址 / AI 地址 / 坐标 / 血量）——感知层核心 |
| `GET /obj?addr=0x..` | 单个对象详情（`addr` = Thing 地址） |
| `GET /enemy` | 仅敌方明细 |
| `GET /units[?q=&side=&cls=&limit=]` | 单位目录检索（名字 / 模板名 / 哈希） |
| `GET /ai/status` | 接口状态 JSON（监听地址 / 调用计数 / 目录条数） |
| `GET /scratch?x=&y=&z=[&idx=0\|1]` | 写一个 Coord3D 传参缓冲并返回其地址（配合 `/aicall`） |
| `GET /postproc?on=0\|1` | 查询/切换"创建后处理链"（让召唤物登记进玩家体系） |
| `GET /view` | **视野中心**的世界坐标（屏幕正中看出去的地面点，含地形高度）——想投在"玩家正在看的地方"就用它 |
| `GET /selected_pos` | 当前选中单位的世界坐标 |
| `GET /cursor` | 鼠标光标处世界坐标（⚠️ 玩家切出游戏窗口后它停在旧值，远程操作别用） |
| `GET /spot` | 按「落点来源」设置自动挑一个可用落点（回 `water=1` 即在水面，别投） |

### 2.2 变更类端点

| 端点 | 作用 |
|---|---|
| `POST /summon?hash=0x..&count=N[&owner=idx][&place=view\|selection\|cursor][&x=&y=&z=]` | **按哈希召唤**（`owner` 见 `/snapshot`）。落点：`place` 指定来源，或给 `x/y/z`；都不给则按「落点来源」设置。回执带实际落点 `at=x,y,z via=来源` |
| `POST /copy` / `/wnb` / `/mcv` | 复制选中 / 预设增援（数量可配） / 基地车 |
| `POST /capture` / `/kill` / `/rank` | 收编 / 摧毁选中 / 满级 |
| `POST /speed?mode=0..3` | 超速 / 慢速 / 定住 / 恢复 |
| `POST /health?mode=0..2` | 无敌 / 残血 / 恢复 |
| `POST /crate?idx=N&count=N` | 空投：`idx=54` 海面折跃信标（投下即播折跃动画，2~5 秒后落成随机海军/空军/两栖单位，13 选 1；可加 `&kind=naval|air|amph` 限定兵种）/ `idx=55` 地面折跃信标（落地即出地面随机单位）/ `idx=56` 空投仓·海·成品箱 / `idx=57` 空投仓（两栖仓） |
| `POST /summon?pool=naval&count=N` | 按兵种随机召唤（`naval`/`vehicle`/`infantry`/`air`/`structure`/`any`），回执 `hash=` 是抽中的模板 |
| `POST /toggle?f=N&on=0\|1` | 功能开关（索引见 `features.h`） |
| `POST /freebuild?on=0\|1` | 免费建造 |
| `POST /poke?addr=0x..&type=u32&val=0x..` | 写内存（RE 用） |
| `POST /call?rva=0x..&cc=fc\|cdecl&a0=..` | 按 RVA 调函数 |
| `POST /aicall?fn=0xVA&this=0x..&a0=&a1=&a2=&na=0..3` | **正统 thiscall 调用**（指挥层核心；`na` 必须精确等于栈参数个数） |
| `POST /order?unit=0x..&act=attack&target=0x..` | **指挥单位**：攻击目标 Thing |
| `POST /order?unit=0x..&act=move&x=&y=&z=` | **指挥单位**：走向世界坐标 |
| `POST /order?unit=0x..&act=idle` | 待机 —— **未实现**（`privateIdle` 槽位未认），返回失败 |
| `POST /postproc?obj=0x..` | 对单个对象手动跑创建后处理 |

> **变更类请求的执行时机**：HTTP 线程写入命令槽 → 渲染线程每帧取走执行 → HTTP 线程等结果（超时 2.5s）。
> 所以单条变更命令的典型延迟是**一到几帧**（约 0.05–1s）。批量操作请**串行**发（接口单线程）。

---

## 3. 核心心智模型

1. **一切都是对象（Thing）**。每个单位/建筑都有一个 `Thing` 地址，是所有操作的句柄。
   `/objects` 返回 `thing` 字段 = 该地址；`/obj?addr=` 读详情。
2. **感知 → 决策 → 行动**：
   `/snapshot` 看全局 → `/objects` 拿具体地址 → `/order`、`/aicall`、`/summon` 动手。
3. **建造 = 召唤**：建筑本身也是对象、也有类型哈希 → `/summon?hash=<建筑码>` 就是"建造"，
   绕过科技前置/生产队列/资源。**这是让 AI 帮你盖基地的捷径**。
4. **越权 = 指定归属**：`/summon?...&owner=<idx>` 给**任意一方**刷兵/刷建筑（含敌方），
   可用于给 AI 对手补兵或做对照实验。
5. **地址会失效**：对象被摧毁后其内存可能被新对象复用 → 缓存的地址可能已指向别人。
   批量操作要**取到地址立即用**，不要隔几秒再回读同一地址做核对。

---

## 4. 关键坑速查

| # | 坑 | 正确做法 |
|---|---|---|
| 1 | **批量召唤 `count=N`**：单位会自动铺开（不重叠）；**建筑会叠在同一点** | 单位放心批量；**建筑必须逐点给坐标** |
| 2 | 多建筑**连发会偶发丢包** | 建完用 `/objects?owner=` 回读核对，缺的补 |
| 3 | `/objects` 结果有 **~3 秒缓存** | 需要最新状态时用 `/snapshot?fresh=1`，或接受 3s 内的一致性 |
| 4 | 击杀函数 `0x79EA50` 的 **ecx 必须是 Thing**（不是血量体） | `/aicall?fn=0x79EA50&this=<Thing>&a0=6&a1=19&a2=0&na=3` |
| 5 | **击杀别在游戏流程之外写 HP=0**：那样不触发死亡判定 → 留下「幽灵单位」（对象不回收、模型仍在、甚至还能操作，但失去单位属性、不能攻击） | 用 `POST /kill`（作用于选中），或 `/aicall?fn=0x79EA50&this=<Thing>&a0=6&a1=19&a2=0&na=3`。修改器内置「秒杀」走的是游戏自身写血量的 hook，**不会**留幽灵 |
| 5 | `|x|+|y|+|z| < 1` 是**坏坐标** | 落点前先校验，别拿坏值当坐标用 |
| 6 | 单位默认**只主动打单位**，不主动拆建筑 | 打建筑要显式 `/order?act=attack&target=<建筑Thing>` |
| 7 | `na` 参数错一个就 **esp 失衡崩溃** | `na` 必须精确等于被调函数的栈参数个数 |
| 8 | 有步兵模板**裸创建后崩溃**的记录（是否与模板类型相关未复核） | 新类型先单个试；入表门槛永远是"实测可创建"，不稳的类型别批量刷 |
| 9 | 创建单位只是"半条路"，未登记进玩家体系（清理后果未实测） | 让 `/postproc?on=1` 保持开启 |

---

## 5. 常用配方

### 5.1 看局势

```bash
curl http://127.0.0.1:17832/snapshot
# {"ok":1,"tick":6124,"player":2,"money":100000,
#  "me":{"units":24,"structs":1},
#  "myUnits":[{"name":"维和步兵","n":18}, ...],
#  "enemies":[{"idx":3,"units":25,"structs":12,"side":"盟军","top":[...]}],
#  "totals":{"playable":77,"unknown":257},
#  "scan":{"ms":672,"mb":1312,"vt":"0x00C4DB60","cached":0}}
```

```bash
# 列出我方所有对象
curl "http://127.0.0.1:17832/objects?owner=2&limit=100"
# 列出敌方单位
curl "http://127.0.0.1:17832/objects?owner=3&cat=unit&limit=100"
```

### 5.2 召唤单位 / 建造建筑

```bash
# 召唤 12 个守护者坦克到 (3660,1960)
curl -X POST "http://127.0.0.1:17832/summon?hash=0xCBD60D8C&count=12&x=3660&y=1960&z=210"

# 建造一座发电厂：建筑即对象，直接召唤
curl -X POST "http://127.0.0.1:17832/summon?hash=0x89B86E3D&count=1&x=3950&y=2100&z=210"

# 召唤前预检模板是否存在（返回 0 = 无此模板）
curl "http://127.0.0.1:17832/resolve?hash=0x89B86E3D"
```

### 5.3 指挥单位

```bash
# 1) 先拿到单位与目标地址
U=$(curl -s "http://127.0.0.1:17832/objects?owner=2&cat=unit&limit=1" | python -c "import sys,json;print(json.load(sys.stdin)['objects'][0]['thing'])")
T=$(curl -s "http://127.0.0.1:17832/objects?owner=3&cat=struct&limit=1" | python -c "import sys,json;print(json.load(sys.stdin)['objects'][0]['thing'])")

# 2) 攻击
curl -X POST "http://127.0.0.1:17832/order?unit=$U&act=attack&target=$T"
# 2') 或移动
curl -X POST "http://127.0.0.1:17832/order?unit=$U&act=move&x=2000&y=2400&z=210"
```

### 5.4 低层直调（`/aicall`）

```bash
# 攻击：this = 单位的 AI 指针 [Thing+0x374]，a0 = 受害者 Thing
curl -X POST "http://127.0.0.1:17832/aicall?fn=0x76E690&this=<AI>&a0=<victimThing>&a1=0&a2=0&na=3"
# 击杀任意对象：this = 该对象的 Thing 地址
curl -X POST "http://127.0.0.1:17832/aicall?fn=0x79EA50&this=<Thing>&a0=6&a1=19&a2=0&na=3"
```

### 5.5 参考脚本

`research/memscan/auto_attack.py` 是一个完整范例：自动分析敌方建筑血量，按
**`战略权重 × 血量`** 比例把己方单位分配到各建筑发起进攻。

```bash
python auto_attack.py --dry            # 只看分配方案
python auto_attack.py                   # 分析并下达攻击命令
python auto_attack.py --top 6           # 只打权重最高的 6 个（集中火力）
python auto_attack.py --loop 20         # 每 20 秒重新分析、分配
```

---

## 6. MCP 工具清单

| 工具 | 作用 |
|---|---|
| `get_battle_state` | 战场态势（感知入口） |
| `get_enemy_report` | 敌方明细 |
| `list_objects` / `object_detail` | 活体对象列表 / 单对象详情（拿地址） |
| `search_units` | 单位目录检索（中文 / 拼音 / 英文 / 哈希） |
| `get_spawn_point` | 查可用落点（视野中心 / 选中单位 / 光标 / 自动） |
| `unit_status` | 对选中单位施加 / 解除状态（铁幕 / 无敌 / 加速 / 减速 / 易碎 / 输出增强 / 重置） |
| `summon_unit` | 召唤单位（可指定坐标、归属） |
| `build_structure` | 建造建筑（等价于召唤建筑码） |
| `command_unit` | 指挥单位（攻击 / 移动；待机未实现） |
| `drop_crate` | 空投箱子 |
| `burst_crate` | 空投箱子并引爆：在目标位置释放效果（`freeze` 冰冻 / `bomb` 爆炸 / `vacuum` 等）；走 DLL 原生 `POST /burst`，创建后下一帧引爆 |
| `selected_action` | 复制 / 增援 / 基地车 / 收编 / 摧毁 / 满级 |
| `set_units` | 调速 / 改血 |
| `toggle_feature` / `set_freebuild` | 功能开关 / 免费建造 |
| `get_status` / `get_log` | 接口状态 / 事件日志 |

`raw` 内存读写工具默认不注册，需显式设置环境变量才会出现。

---

## 7. 游戏内查看与配置

1. 进对局 → 呼出键（默认 `INSERT`，可在启动器里改）打开菜单；
2. 勾选主面板的 **「脑机接口」** → 弹出面板；
3. 面板内可**一键复制 MCP 配置片段**、查看**实时战场态势**（我方/敌方部队数，悬停敌行看兵种构成）
   与 **AI 调用记录**。

> 面板只作状态显示与配置入口；接口随注入即监听。

---

## 8. 安全边界与免责

- **仅单机**：接口只绑 `127.0.0.1`，且只应用于**单人离线对局**；任何联机/对战场景禁止使用。
- **能力等同于修改器本身**：AI 拿到的就是你手动能按的所有按钮（含召唤、摧毁、改血、调函数）。
  想平稳，先只跑**只读**（`/snapshot`、`/objects`）做"顾问"，确认无误再逐步放开变更类。
- **变更调用可追溯**：带 `ai=1` 的调用会计数并记入面板的「AI 调用记录」。
- **扫描是只读**：全图枚举只读内存、不调游戏函数、不改内存，最坏情况是读到脏值 —— 读前做了范围/指针校验。
- **风险自担**：这是修改器，按 RVA 调游戏函数、写内存都可能**直接崩游戏**（未保存的进度会丢）。
