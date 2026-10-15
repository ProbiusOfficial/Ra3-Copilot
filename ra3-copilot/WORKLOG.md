# RA3 日冕修改器 — 工作日志

> 项目：`RA3/ra3-copilot`（注入型修改器，C++ / x86）
> 记录：2026-10-14（探宝 × 晨曦）
> 目标游戏：红色警戒 3 日冕 mod（`ra3_1.12.game`，v1.12.3444.25830，32 位）

---

## 一、目标与现状

**需求**：游戏内菜单（画在游戏画面内），带各种功能开关；玩法本体是**"点按钮 → 空投箱子 → 单位拾取拿奖励"**。

| 模块 | 状态 |
|---|---|
| 注入通道（x86 injector + 自动提权） | ✅ 稳定可用 |
| 功能钩子（进程内内存 patch） | ✅ **16 个钩子 / 13 个功能**，AOB 全部通过校验 |
| 独立窗口菜单 | ✅ 可用（v0.6，无边框 + 贴游戏窗口） |
| **游戏内渲染菜单** | ❌ **未打通**（详见第二节） |
| 箱子空投 | ⚠️ Lua 侧已验证可行（隔壁会话 v24），C++ 侧未接 |

---

## 二、核心技术障碍（当前卡点）

**现象**：往游戏 device 的 vtable 挂 `EndScene`/`Present`，**挂载成功（`d3dHooked=1`）但计数恒为 0** —— 游戏从不走我们改的表。

### 2.1 实测数据（关键证据）

**① 本机的 d3d9 走了 wrapper**

游戏进程加载了：
```
d3d9.dll        base=0x66410000
dxgi.dll        base=0x641E0000
atidx9loader32.dll  base=0x6B0C0000
AMDXN32.DLL     base=0x642F0000
EnhancerCorona.dll  base=0x795D0000
```

**② dummy device 拿到的 vtable 不在任何模块里**

| 来源 | vtable 地址 | 归属 |
|---|---|---|
| `CreateDevice`（dummy） | `0x9f5f18fc` | **NOT IN ANY MODULE**（运行时堆上构造） |
| `CreateDeviceEx`（dummy） | `0x9f57ea1c` | **NOT IN ANY MODULE** |
| **游戏真实 device** | `0x66412c88` | **d3d9.dll +0x2C88**（模块内） |

**结论**：`Direct3DCreate9` 返回的是 **wrapper 实现**，其 device 的 vtable 是**堆上动态生成的**；而**游戏自己的 device 用的是 d3d9.dll 镜像内的标准表**。**两者完全不同** —— 这就是"挂上却不触发"的最终根因。

**③ d3d9.dll 内有 10 张不同的 device vtable**

```
0x66412c88  0x66411324  0x66411308  0x66414804  0x6641170c
0x66411794  0x664126bc  0x664126a0  0x664124c0  0x664124b8
```
（各表的 `Present`/`EndScene` 地址互不相同 → d3d9.dll 内有多个 device 实现类）

### 2.2 尝试过的方案与结果

| 方案 | 结果 |
|---|---|
| dummy device vtable hook（普通） | ❌ 计数恒 0（拿到的是 wrapper 堆表） |
| dummy device vtable hook（+ Ex 双 dummy） | ❌ 同上 |
| 扫全部内存找 device 并全挂 | ❌ `Present=1` 后闪退（**误挂**：其他接口/对象的表被当成 device） |
| 扫描 + `TestCooperativeLevel()` 试探验证 | ❌ 闪退（非 device 对象的该方法**带输出参数** → 写坏栈） |
| 扫描 + 119 方法纯内存判定 | ❌ 仍 `Present=1` 后闪退（误挂率仍高） |
| **扫描范围收窄到 `game.exe` 的 `.data` 段** | ⏳ **已编译，待测**（假设：游戏持有的 device 指针在引擎全局变量里） |

### 2.3 尚未尝试的可靠方案（B 计划，零误判）

**hook `IDirect3D9::CreateDevice`** —— 游戏创建设备必然经过它，直接拿到游戏传出的 device 指针：
- 优点：**零扫描、零猜测、零误判**
- 代价：游戏在我们注入前已建好设备 → 需**用户触发一次设备重建**（`Alt+Enter` 或改分辨率）

---

## 三、渲染方案调研结论

**目标**：把修改器菜单**画在游戏画面内**（进程内覆盖层），而不是独立窗口。

**为什么不能做独立窗口**：游戏失去焦点会静音，独立窗口抢焦点会打断游戏。

**为什么必须 inline-hook 函数而非改 vtable**：
- 改 dummy device 的 vtable 数组对游戏无效 —— dummy device 的 vtable 是运行时堆上构造的，与游戏真实 device 不是同一张表。
- 游戏真实 device 的 vtable 在 `d3d9.dll` 镜像内（本机 `0x66412c88`，= +0x2C88）。正解 = inline-hook `d3d9.dll` 里 `Present`/`EndScene` 的**函数本身**。

**验证通过**：挂钩后 `EndScene/Present` 计数持续增长，且**不影响游戏运行、不抢焦点**（切窗口才静音）。

**目标 UI 形态**：左侧分类导航（资源 / 战斗 / 单位 / 设置 / 状态）+ 顶部工具栏 + 「单位属性」折叠栏 + 底部「输出记录」日志区。

**版本无关性**：渲染逻辑与游戏版本无关；功能钩子地址需按版本重新定位（见 `src/symres.*`）。

---

## 四、崩溃排查记录（三次，各自根因不同）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 一注入就崩 | 10 张 vtable 共用一个 `g_origEndScene` → 第 2 张表被调用时跳到第 1 张表的函数 | 每张表各自保存原始函数（`VtblEntry` 数组，按 device 的 vtable 查询） |
| 2 | 挂上后调用即崩（`Present=1`） | 判定只看"索引是否落在 d3d9 内" → 误挂非 device 接口 | 收紧判定（7 个索引 + BeginScene≠EndScene） |
| 3 | 扫描阶段就崩（`Present=0`） | **试探调用 `TestCooperativeLevel()`** 对非 device 对象 → 其方法带输出参数 → **写坏栈** | 改**零调用的纯内存判定**；后又发现误挂仍未根除 → 收窄扫描范围到 `.data` 段 |

**教训**：在别人进程里做推断性操作，**任何"调用不确定对象的方法"都可能因参数错位破坏栈**；优先纯内存判定；扫描范围越窄越安全。

---

## 五、已实现的功能（16 钩子 / 13 功能）

| 分类 | 功能 | 钩子 RVA | 洞内动作 |
|---|---|---|---|
| 资金 | 加钱 / 设值 | `0x6CFDFE` | `[player+4]` 加/设任意值 |
| | 本地玩家追踪 | `0x0FF95B` | 记录对象指针 + ID |
| 玩家属性 | 电力满 | `0x6CFD0D` | `[eax+4]=100000` |
| | 科技点满 | `0x6CFE6C` | `[eax+0x34]=15` |
| | 全科技 | `0x6CFEB5` | `[edi+0x2c]=400.0f` |
| | 快速建造 | `0x30F42E` `0x30F38C` `0x2F6CFF` | `[esi+0x1c]=100.0f` |
| 建造 | 敌方无法建造 | `0x30F530` | 敌方 `[eax+4]` 压到 5000 / 1 |
| 战斗 | 玩家无敌 | `0x12EEDF` | 己方 `[esi+4]=[esi+0xc]=9999999.0f` |
| | 选中单位弹药满 | `0x128735` | `[ecx+0x1c]=0x7FFFFFFF` |
| | 矿厂复原 | `0x2D4933` | `[ecx+0x14]=0`（一次性） |
| | 危险等级 | `0x438ED0` | 最高 50000 / 最低 0 / 不干预 |
| | 超武就绪 | `0x2EBB69` | 己方 `[esi+0x20]=1` |
| | 禁用敌方超武 | `0x43ECA6` | `[eax+0xc]+=1` |
| 地图 | 全图（去迷雾） | `0x3C226C` | `[ebp+0x260]=99999.0f` |

**架构**：`features.cpp` 的表驱动 `kHooks[]`（名/RVA/AOB/壳/返回地址/重放长度）→ 新增功能 = 表加一行 + 写一个壳 + UI 加个勾选框。

**未移植**（原因）：
- `SuperPower2` `0x43EC19`：洞内 `[esp+0x28]` 依赖 pushad 栈布局，需实测
- `DisableAllSP2` `0x30A5F6`：该挂钩点只有原指令重放、无改值逻辑
- `Zoom` `0x1EC7CD`：`fadd`/`fsub` 两档分支关系绕
- **选择单位动作分发**（升级/击杀/克隆/召唤部队/基地车）：需遍历玩家-单位链表 + 调用游戏内部函数，参数语义未确定

---

## 六、交付物与工具

**产物**（`ra3-copilot/`）：
- `ra3copilot.dll`（注入模块）、`injector.exe`（x86 注入器）、`build.bat`、`deploy.bat`（自提权部署）、`README.md`

**诊断工具**（`ra3-copilot/tools/`）：

| 工具 | 用途 |
|---|---|
| `check_loaded.py` | 验证 DLL 是否已注入（模块枚举） |
| `inject_and_probe.py` | 注入 + 全遥测读取（读 map 符号表） |
| `probe_state.py` | 钩子/遥测状态 |
| `probe_render.py` | 渲染层专项诊断（vtable / inline hook 检测） |
| `d3d9_probe.cpp` | D3D9 vtable / 函数字节探针（x86 独立程序） |
| `scan_hooks.py` | 全部钩子 AOB 与真实 exe 逐字节校验（20/20 通过） |
| **`scan_windows.py`** | 窗口结构扫描（含子窗口 + style/exstyle 标志解析） |
| **`find_device.py`** | 提权扫内存定位 device 对象（vtable∈d3d9 且 [16]/[17]/[42] 均在 d3d9） |
| `ida_mcp.py` | IDA Pro MCP 直连客户端（支持多端口，解析 SSE） |

**配置**：`~/.workbuddy/mcp.json` 已加 `ida-pro-mcp`（`http://127.0.0.1:13337/mcp`，streamable HTTP）。

---

## 七、关键数据与地址备忘

```
游戏 exe      D:\GAME\Game_RTS\Red Alert 3\Data\ra3_1.12.game  (9,306,120 B)
主模块基址    0x00400000
d3d9.dll      0x66410000  (size 1,548,288)
游戏主窗口    class="41DAF790-16F5-4881-8754-59FD8CF3B8D2" title="红色警戒：日冕"
              style=0x94C80000 (WS_POPUP+WS_VISIBLE+WS_CAPTION+WS_SYSMENU)  exstyle=0x00000100
游戏行为      失去焦点 → 战场静音（所以覆盖层不能抢焦点）
菜单热键        INSERT（我们当前使用）
```

---

## 八、下一步（按优先级）

1. **确认当前版本**（扫描 `.data` 段）是否解决渲染挂载 —— 看「状态」页 `EndScene/Present` 是否增长
2. 若仍失败 → **切 B 计划**：hook `IDirect3D9::CreateDevice` + 用户 `Alt+Enter` 触发一次设备重建（**零误判**）
3. 渲染打通后 → **UI 重构**为左侧分类导航（资源 / 战斗 / 单位 / 设置 / 状态）
4. 接**箱子空投**：按钮 → `CreateUnit`（需解析 `UnitCrate` 对象类型指针）**或**驱动 Lua 桥
5. 补剩余功能（选择单位动作分发等，需实测参数）

**并行另一条路**（隔壁会话，Lua 正门）：地图脚本注入 Lua 声明 → 用官方 `SpecialPowerModule`/`ProtocolModule` 开协议/技能；已产出协议名全表（67 条）在 `RA3/research/tools/corona_protocol_names.txt`。
