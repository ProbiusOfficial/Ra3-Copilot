# MCP 开放（脑机接口）—— 让 AI 接入你的战局

> 一句话：把修改器已有的进程内调试接口，用标准 **MCP 协议**开放给任意 AI —— AI 能看到你的部队和 AI 的部队，并能给你投放单位/箱子。

---

## 一、这是什么

在游戏内面板上多了一个 **「脑机接口」** 面板。打开它之后，任意支持 MCP 的 AI 客户端
（Claude Desktop、Cursor、各类 agent 框架…）就能：

| AI 能做什么 | 怎么做到 |
|---|---|
| **看局势** | 读取我方/敌方的**部队数、建筑数**、敌人阵营与兵种构成、资金、当前选中单位 |
| **查单位** | 1008 单位 + 54 箱子的目录检索（中文名 / 拼音 / 英文模板名 / 哈希） |
| **加部队** | 召唤任意单位（暴兵、救场、空投支援） |
| **发奖励** | 空投治疗箱 / 现金箱 / 晋升箱 / 属性箱 |
| **动单位** | 对选中单位：复制 / 收编 / 摧毁 / 满级 / 调速 / 改血 |
| **改全局** | 电力无限 / 全科技 / 免费建造 / 去迷雾 / 无敌 / 秒杀 … 一整套开关 |

> **接口只监听 `127.0.0.1`**（单机）——这是唯一的安全边界；变更类端点无闸门，但仍投递渲染线程执行。
> 完整接入说明见 [`AI接入指南.md`](AI接入指南.md)。

---

## 二、架构

```
┌──────────────────┐   MCP (stdio JSON-RPC)   ┌────────────────────┐
│ 任意 MCP 客户端    │ ◀──────────────────────▶ │ mcp/ra3_mcp.py     │
│ Claude/agent …    │                          │ （零依赖 Python）    │
└──────────────────┘                          └─────────┬──────────┘
                                                        │ HTTP 127.0.0.1:17832
                                              ┌─────────▼──────────┐
                                              │  ra3copilot.dll     │
                                              │  （游戏进程内）       │
                                              │  · 只读端点→HTTP线程  │
                                              │  · 变更端点→渲染线程  │
                                              └─────────┬──────────┘
                                                        │ 只读内存 / 调游戏函数
                                              ┌─────────▼──────────┐
                                              │ 红警3 日冕 mod       │
                                              └────────────────────┘
```

- **没有引入新进程依赖**：MCP 服务器就是个 Python 脚本，只连本机 HTTP 接口。
- **可执行单元（DLL）不变**：执行能力全部走已有的 30 多个 HTTP 端点，MCP 服务器只是标准协议门面。
- **感知能力是新增的**：全图单位枚举（见下）。

---

## 三、感知层的原理

只读「游戏里选中的单位」回答不了"你有多少部队、AI 有多少部队" —— 必须**枚举全图对象**。

### 3.1 关键偏移

| 字段 | 位置 | 说明 |
|---|---|---|
| Thing vtable | `[Thing+0]` | 本构建 = `0x00C4DB60`（所有 Object 统一；运行时优先从选中单位推导） |
| **对象模板指针** | `[Thing+0x04]` | 指向对象模板 |
| **对象类型哈希** | `[模板+0x08]` | 哈希 → 名字/阵营/类型 |
| **归属（owner）** | `[Thing+0x418]` | 指向 Team 对象（vtable 统一 `0x00C5BCE0`） |
| Team 索引 | `[Team+0x20]` | 实测 = 玩家 ID（本地玩家 = 2） |

> ⚠️ 真实单位是 **Drawable**（vtable `0x00BED890`），它的 `[+0x138]` 才是 **Thing**；
> 两者数据区相同但 **owner 只在 Thing 上**（`Drawable+0x418` = 0）。所以枚举扫 **Thing vtable**。

### 3.2 算法

```
扫本进程堆内存（MEM_PRIVATE 提交页，~2.1GB）→ 找所有 [addr]==ThingVtable
  → 对每个命中：读 owner=[T+0x418]、hash=[ [T+0x04] +0x08 ]
  → 用 battle_catalog（938 个"可玩对象"）过滤掉矿晶/树/弹道等场景物
  → 按 owner 分组：部队数 / 建筑数 / 阵营(多数派) / 种类 Top10
```

- **安全性**：纯只读、**不调任何游戏函数** → 在 HTTP 线程执行，与游戏主线程无竞态。
- **性能**：SSE2 加速扫描，一次约 **数百毫秒**；结果带 **3 秒缓存**（面板/HTTP 共用，不重复扫）。
- **版本适应性**：Thing vtable 优先从「当前选中单位 → `[d+0x138]`」动态推导，退化才用已知常量。
  推导结果要做合法性校验（候选命中数兜底），否则在"选中半初始化的召唤物"时会推错 vtable，导致态势全 0。

### 3.3 实测样例

| 归属 | 部队 | 建筑 | 阵营 | 构成（节选） |
|---|---|---|---|---|
| **idx 2（本地玩家）** | 44 | 15 | 神州 | 迅雷天罡×15、蜂鸟侦查无人机×6、坦克杀手×5、揽星亭×5、伏龙殿×2 |
| idx 6 | 58 | 17 | 神州 | 破甲铁卫×25、凌波护卫战车×8、磁弩防空系统×4 |
| idx 5 | 43 | 15 | 盟军 | 标枪兵×22、冰冻直升机×5、光棱坦克×4 |
| idx 4 | 37 | 20 | 苏联 | 磁爆部队×10、恐怖机器人×7、战熊×4 |
| idx 0 | 1 | 19 | 中立 | 矿脉节点×18、钻油塔×1 |

（另有一批"未识别对象"≈400 个：矿晶、树、弹道等场景物，已排除出部队统计。）

---

## 四、DLL 侧新增端点

| 端点 | 说明 |
|---|---|
| `GET /snapshot[?fresh=1]` | **战场快照 JSON**：资金 / 我方部队建筑 / 敌方各归属明细与构成 / 选中 / 扫描统计 |
| `GET /enemy` | 只返回敌方部分 |
| `GET /units?q=&side=&cls=&limit=` | 单位目录检索（名字/模板名/哈希） |
| `GET /ai/status` | 接口状态 JSON（监听地址 / 调用计数 / 目录条数） |
| `POST /summon?hash=&count=&owner=` | 召唤单位并可**指定归属玩家**；`owner` 缺省或 0 = 本地玩家 |
| `POST /burst?hash=[&x=&y=&z=\|&place=][&delay=ms]` | **箱子即爆**：一次请求完成「创建箱子 → 下一帧击杀」，落点释放该箱子的死亡开火效果（冰冻/爆炸/真空…）。不依赖 `/aicall` |
| `POST /action?name=&a1=&a2=&a3=` | 执行引擎脚本动作（`ExecuteAction`，RVA **`0x182C50`**）。参数只支持名字串，最多 3 个（URL 编码）。**2026-10-19 起可用**（设钱 / 设协议点 / 授予技能均实测） |
| `POST /grant?power=SpecialPower_X[&who=]` | **按内部名授予协议技能**。`who` 缺省 `<This Player>`；也认 `<All Human Players>` / `<All Players>` / 裸玩家名（如 `Player_2`）⇒ **可跨阵营、可指定敌方**。仅当技能没有「可授予对象」（单位能力、被模组禁用的超武）时静默跳过 |
| `POST /lua?rva=0x..&a1=&a2=&a3=` | 按 RVA 直调引擎 Lua C 函数，同样只支持名字串 |
| `GET /luatest` | Lua 栈自检：压一个串、报 tag/类型/取回内容。定位「参数到底进没进去」用 |

**`/action` 现状：已打通（2026-10-19 下午终局结论）**

> ⚠ **下面这段「参数表示是唯一卡点」的分析已作废**，保留只为记录踩坑路径。真正的两条原因：
> ① 地址错了 —— `0x178670` 是 **`ObjectDescription`**，真 `ExecuteAction` = **`0x182C50`**；
> ② 压栈类型错了 —— 真入口是 Lua 全局函数，实参必须是**普通 Lua 串（tag=3）**，不是引擎串（tag=4）。
> 完整证据 / 错位对照表 / 复现命令见 `research/protocol-skills/findings/引擎Lua脚本接口.md` §八、§九。
>
> 实测三项：`PLAYER_SET_MONEY` → money 10000→7777；`PLAYER_SET_PLAYER_POWER_POINTS` → 3→20；
> `PLAYER_GRANT_SPECIAL_POWER` → `SCPoint+0x18` 列表 13→14→15→16→17，且探宝实测**技能上栏、可点可放（有语音播报）**。
> 另：**跨阵营授予成立**（盟军 TimeBomb / CryoSatellite、苏军 OrbitalRefuse 给天朝玩家全部 +1），无阵营检查。

以下为作废分析原文：

**`/action` 旧分析（作废）：栈通道已通，参数表示是唯一卡点（2026-10-19 上午，误判）**

`ExecuteAction`（`0x178670`）开头就是：

```asm
push 1; push L; call 0xA5D0    ; 取 arg1 为 C 字符串
mov esi,eax
test esi,esi; jne 查表         ; 取到了 → 走引擎 +0xBC 动作表
push 1; push L; call 0xA520    ; 否则看类型
cmp eax,1; je 查表(esi=0)      ; 1 == nil → 也放行
call 0xA8B0; mov eax,1; ret     ; 否则 early-out（返回 1、无结果串）
```

而 `0xA5D0` 的实现写死了类型：

```asm
slot = base + idx*16 - 16
if (slot >= top)      return NULL
if (*(int*)slot != 4) return NULL     ; 只认 tag 4
return *(u32*)(*(u32*)(slot+8) + 0xC) ; 字符在 value+0xC
```

**本引擎的栈布局（实测，与标准 Lua 5.1 不同，别照抄 luajit 经验）**

- `lua_State`：`+0x00` = top，`+0x10` = base，槽宽 **16 字节**；`gettop = (top-base)/16`
- 槽结构：`{ tag@+0, pad@+4, value@+8, pad@+12 }`
- 类型号：**4 = 引擎字符串**（`{?,?,len,char*}`，字符指针在 `+0xC`）——tag 4 实证（`/luatest` 四参数回读）；**tag 1/3 的含义未验证**（旧文档的"1 = nil、3 = Lua 串"无实测支撑）。详见 `docs/秘密协议-重写.md`
- ⇒ `lua_pushstring` 压出的槽不是 tag 4，而 `ExecuteAction` 的取值器只认 tag 4 —— **两者不是一种表示**

**已解决**：DLL 里自带 `PushEngineStr()` 手工构 tag-4 槽，`/luatest` 自检已能原样取回文本
（`引擎串(tag4): tostr=RA3SELFTEST`）。动作名的查表这一关**已通过**。

**仍失败**：授予无效果。作者自己的写法与我们一致（`scriptevents.xml` 1655 行：
`ExecuteAction("PLAYER_GRANT_SPECIAL_POWER", "SpecialPower_CelestialOrbitalStrike", "<This Player>")`），
问题落在**参数解析**：`<This Player>` 依赖脚本上下文。`SetScriptContext()` 现在写
`engine+0x17D88` —— 依据反汇编 `0x17FA80`：该字段为 0 时 `<This Player>` 直接解析失败
（旧代码写的 `+0x17D68/+0x17D70` 偏移错 0x20）。设为本地玩家对象后，授予仍无效果；
换 `Player_1`/`Player_2`/`LocalPlayer` 亦无效果（`/snapshot` 的 `player=2`）。原因未定位。
`ret` 恒为 1，**不能当成功判据**；`ProtocolCount()` 是本地静态表长度（当前 294），也不能当探针。

**下一步**：① 按 `engine+0xBC` 表取出描述符，直接调 `0x727140(描述符, 参数)` 绕开名字解析；
② 顺带确认数字参数的 tag（`PLAYER_SET_MONEY` 可当探针，钱在 `/snapshot` 可读）。

**`/summon` 的归属（`owner`）**：索引见 `/snapshot` 的 `owners[].idx` / `enemies[].idx`，可为任意一方（含 AI）加兵。
DLL 侧 `SummonByHashFor(hash, count, teamOverride, …)`，`FindTeamByIdx(idx)` 用缓存的全图枚举把索引翻成队伍对象地址（在 HTTP 线程执行）。

**`/burst` 的两条约束**（都是实测踩出来的，改动前先读）：

1. **不能同帧击杀**。`CreateUnit` 当帧对象尚未初始化完，`Thing+4` 的模板指针读回 0；此时调 `KillUnit` 既不消耗箱子、也不触发死亡开火（靶无任何冻结指纹）。所以创建后**至少隔一帧**发力，由渲染线程的 `BurstPump()` 到期执行。
2. **`ecx` 必须是 Thing（`CreateUnit` 的返回值），不是 body**。传 body 同样「无异常、无效果」（箱子留在原地 hp=0、靶状态不变），极易误判为「函数调用成功」。

引爆用拾取链原生参数 `KillUnit(thing, 6, 1, 0)`；开火前用模板哈希（`[thing+4]+8`）校验身份，地址被引擎复用则拒绝开火。

**变更类端点无闸门，但仍投递渲染线程执行**；`&ai=1` 仅作遥测（计数 + 日志），MCP 服务器的变更调用会自动带上它。

示例：

```bash
curl "http://127.0.0.1:17832/snapshot"
# {"ok":1,"tick":21098,"player":2,"money":15000,"selection":1,
#  "me":{"units":44,"structs":15},
#  "myUnits":[{"name":"迅雷天罡","n":15},...],
#  "enemies":[{"idx":6,"units":58,"structs":17,"side":"神州","top":[...]}],
#  "totals":{"playable":269,"unknown":420},
#  "scan":{"ms":312,"mb":2160,"vt":"0x00C4DB60","cached":0}}
```

---

## 五、MCP 工具清单（`mcp/ra3_mcp.py`）

| 工具 | 作用 | 风险 |
|---|---|---|
| `get_battle_state` | 战场态势（AI 的"眼睛"） | 只读 |
| `get_enemy_report` | 敌方明细 | 只读 |
| `search_units` | 单位/箱子检索（中文·拼音·英文·哈希） | 只读 |
| `get_status` / `get_log` | 接口状态 / 事件日志 | 只读 |
| `summon_unit` | 召唤单位（`owner` 可指定给哪一方，含 AI） | 中（写游戏） |
| `drop_crate` | 空投箱子 | 中 |
| `burst_crate` | 空投箱子并立即引爆 —— **在目标位置释放效果**（`freeze` 冰冻 / `bomb` 爆炸 / `superbomb` / `vacuum` / `particle` / `radar`…）；效果对敌我无差别 | 中 |
| `selected_action` | 复制/增援/基地车/收编/摧毁/满级 | 中 |
| `set_units` | 调速 / 改血 | 中 |
| `toggle_feature` / `set_freebuild` | 功能开关 | 中 |
| `raw`（🎛️默认关） | 任意内存读写 / 按 RVA 调函数 | **危险** |

> `raw` 需要客户端环境变量 `RA3_MCP_ALLOW_RAW=1` 才注册；不设就根本看不到这个工具。

---

## 六、怎么用

### 6.1 客户端配置

把这段贴进 MCP 客户端的配置（路径按你的实际位置）：

```json
{
  "mcpServers": {
    "ra3-copilot": {
      "command": "python",
      "args": ["C:\\Users\\admin\\WorkBuddy\\RA3\\ra3-copilot\\mcp\\ra3_mcp.py"]
    }
  }
}
```

游戏内「脑机接口」面板里有一键复制的配置片段（路径由 DLL 自动填）。

### 6.2 游戏内操作

1. 进对局 → `INSERT` 开菜单；
2. 勾选主面板的 **「脑机接口」** → 弹出面板（**接口默认即开放，无需点任何开关**）；
3. 面板显示 **接口地址 / 已服务次数**、**我方与敌方部队数**（悬停敌行看兵种构成）与 **AI 调用记录**。

### 6.3 示例对话

> **你**：帮我看下现在局势。
> **AI**：`get_battle_state` → 你（神州）44 部队 15 建筑；三家里 idx6 神州最凶（58 部队，25 个破甲铁卫），idx5 盟军标枪兵海（22 个）。
> **你**：给前线空投两个治疗箱，再补 6 辆磁爆坦克。
> **AI**：`drop_crate("治疗", 2)` → 已空投；`search_units("磁爆")` → `summon_unit("磁爆坦克", 6)` → 已生成。

---

## 七、安全与已知风险

1. **隔离靠只监听 `127.0.0.1`（单机）**：要收回能力就停掉注入 / 退出游戏。
2. **只读端点无副作用**：读的是内存，不改游戏状态。
3. **调用可追溯**：所有 AI 变更调用进「输出记录」和面板的「AI 调用记录」。
4. **扫描是只读**：全图枚举不调游戏函数、不改内存，最坏情况是读到脏值 —— 读前做了指针/范围校验。
5. **⚠️ 已知风险**：
   - **裸 `CreateUnit` 有过步兵类模板召唤后崩溃的记录** —— 实测 `C0000005`，约 0.3s 后崩在 `0x006FE7E3`；是否由模板类型决定未复核。新类型建议**先单个实测试验**，并维护"创建失败黑名单"（入表门槛 = 实测可创建）；
   - **召唤物"隐形"（小地图图标半透明 / 敌方不主动攻击）已修复**：成因是 `[obj+0xC8] |= 0x800` 会把对象置为隐藏态；`SummonByHash` 创建后清除 `+0xC8` / `+0x28` / `+0x208` 三位（见 [`单位操作与召唤-已接入.md`](单位操作与召唤-已接入.md) 的状态字一节）；
   - 说白了：**这是修改器，AI 拿着真枪**。想让 AI 稳健，先只在 L1（只读顾问）跑通再放权。

---

## 八、文件清单

| 文件 | 作用 |
|---|---|
| `ra3-copilot/src/battlefield.{h,cpp}` | 全图单位枚举 + 快照数据 + 缓存 + 后台刷新 |
| `ra3-copilot/src/battle_catalog.h` | 938 个"可玩对象"目录（自动生成；过滤 + 命名） |
| `ra3-copilot/tools/gen_battle_catalog.py` | 生成上表 |
| `ra3-copilot/src/httpapi.{h,cpp}` | 感知 / MCP 端点 + 调用记录 |
| `ra3-copilot/src/overlay.cpp` | 「脑机接口」面板 + 主面板开关 |
| `ra3-copilot/mcp/ra3_mcp.py` | MCP 服务器（零依赖，stdio） |
| `ra3-copilot/mcp/unit_catalog.json` | 1008 单位 + 54 箱子（自动生成） |
| `ra3-copilot/tools/gen_mcp_catalog.py` | 生成上表 |
| `research/memscan/snap_objects.py` | 枚举方案的外部验证原型（不注入即可跑） |
