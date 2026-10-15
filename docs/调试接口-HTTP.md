# RA3 修改器 · 进程内 HTTP 调试接口

> 目的：**不用退游戏**就能验证功能。修改器 DLL 内部起一个只在 `127.0.0.1` 监听的 HTTP 服务，
> 用 `curl` 就能读状态、改内存、调游戏函数、触发功能。
>
> 端口：**17832** ｜ 实现：`ra3-copilot/src/httpapi.cpp` ｜ 接线：`OverlayInit()` 启动、`BuildUI()` 每帧 `HttpApiPump()`

---

## 一、为什么要分「只读」和「变更」

- **只读端点**在 HTTP 线程直接执行（读内存跨线程安全）。
- **变更端点**（召唤/升级/写内存/调函数）**必须投递到渲染线程**执行 —— 渲染线程就是游戏主线程。
  跨线程调游戏函数会崩（历史铁律）。HTTP 线程投递后等结果，超时 2.5s。
- 同理 `/diag`、`/resolve` 也走渲染线程，因为它们内部会调游戏的模板查找函数。

> 等待期间如果渲染线程没在跑（还没进游戏画面），变更类端点会返回 `timeout（渲染线程没在跑？）`。

---

## 二、端点速查

### 只读（立刻返回）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/ping` | 存活探测，返回 `pong` |
| GET | `/state`、`/diag` | 状态大体检：模块基址、符号解析结果、相机/落点三来源、选中单位结构、模板查找结果 |
| GET | `/log?n=40` | 界面「输出记录」的尾部 n 行 |
| GET | `/read?addr=0x..&n=128` | 内存十六进制 + ASCII 转储（n 上限 256） |
| GET | `/peek?addr=0x..&type=u32` | 读单值，`type` = `u32`(默认) / `f32` / `u8` / `u16` / `ptr`（ptr 会多解一层） |
| GET | `/resolve?hash=0x..` | 对象类型哈希 → 模板指针 |
| GET | `/help` | 全部端点列表 |

### 变更（投递到游戏主线程）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/copy` | 复制选中单位到落点 |
| POST | `/wnb` | We Need Back（预设单位 ×8） |
| POST | `/mcv` | 召唤三阵营基地车 |
| POST | `/capture` | 收编（归属改成本地玩家） |
| POST | `/kill` | 摧毁选中 |
| POST | `/rank` | 满级 |
| POST | `/speed?mode=0..3` | 0 超速 / 1 慢速 / 2 冻结 / 3 恢复 |
| POST | `/health?mode=0..2` | 0 无敌 / 1 残血 / 2 恢复血量 |
| POST | `/summon?hash=0x..&count=N` | 按哈希召唤单位 |
| POST | `/crate?idx=N&count=N` | 空投箱（idx 对应 `kCrates[]`） |
| POST | `/toggle?f=N&on=0\|1` | 功能开关（索引见 `features.h` 的 `FeatureIdx`） |
| POST | `/freebuild?on=0\|1` | 免费建造 |
| POST | `/poke?addr=0x..&type=u32&val=0x..` | 写内存（RE 用） |
| POST | `/call?rva=0x..&cc=cdecl\|fc&a0=..&a1=..&a2=..` | 按 RVA 调游戏函数（RE 用，SEH 包裹） |

---

## 三、典型用法

```bash
# 探活
curl http://127.0.0.1:17832/ping

# 状态大体检（推荐第一件事）
curl http://127.0.0.1:17832/state

# 看日志尾部
curl "http://127.0.0.1:17832/log?n=40"

# 读一块内存
curl "http://127.0.0.1:17832/read?addr=0x8E6C58&n=64"

# 读一个 float
curl "http://127.0.0.1:17832/peek?addr=0x9E0A1000&type=f32"

# 哈希 -> 模板
curl "http://127.0.0.1:17832/resolve?hash=0xC33C66E7"

# 触发类
curl -X POST http://127.0.0.1:17832/rank
curl -X POST http://127.0.0.1:17832/capture
curl -X POST "http://127.0.0.1:17832/summon?hash=0xC33C66E7&count=3"

# 写内存 / 调函数（RE 现场试探）
curl -X POST "http://127.0.0.1:17832/poke?addr=0x9E0A1000&type=u32&val=0x00000000"
curl -X POST "http://127.0.0.1:17832/call?rva=0x3E4230&cc=fc&a0=0x8E6C58&a1=0x0"
```

---

## 四、为什么 `/read` `/poke` `/call` 是关键

定位「落点不对 / 满级无效 / 召唤物没入世」这类问题时，**可以不改代码、不退游戏**：
先 curl 读结构、试参数，试对了再固化成 C++。这把每次迭代的代价从「退游戏 → 改码 → 编译 → 注入 → 进对局」
降到「一条 curl」。

---

## 五、构建要点（踩过的坑）

- `build.bat` 编译列表要带 `src\httpapi.cpp`，`/link` 要加 **`ws2_32.lib`**。
- `httpapi.cpp` **必须**先定义 `WIN32_LEAN_AND_MEAN`、并把 **`<winsock2.h>` 放在 `<windows.h>` 之前**
  （否则 `windows.h` 会先拉进旧 `winsock.h`，`ws2tcpip.h` 报出一堆未定义标识符）。
- 顺序同理：`"overlay.h"`（含 `windows.h`）要排在 winsock 之后。
- 换了 DLL 必须**退游戏重注入**才会生效（DLL 是镜像映射，改文件不影响已加载的进程）。

---

## 六、当前状态

- 端点已全部实现并**编译通过**，DLL 已同步到 `ra3-copilot\ra3copilot.dll`（md5 `2eae16fb…`, 745984 B）。
- 游戏未启动；**下次进对局注入后即可直接用**。

---

## 七、v25 增补（2026-10-16）

### 1. 新增端点

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/snapshot[?fresh=1]` | 战场快照（我方/敌方部队统计） |
| GET | `/objects?owner=&cls=&cat=&limit=` | 全图对象列表（按归属/类别过滤） |
| GET | `/obj?addr=0x..` | 单个对象详情 |
| GET | `/enemy` | 敌方对象 |
| GET | `/units?q=..` | 单位检索 |
| GET | `/ai/status` | AI 通道遥测计数 |
| GET | `/scratch` | 临时草稿区 |
| POST | `/order?unit=..&act=attack\|move\|idle` | 指挥单位（AI 控制层） |
| POST | `/aicall?fn=0x..&this=..&a0=..&na=N` | 按 RVA 调 AI 类方法（RE 用） |
| POST | `/postproc?on=0\|1` | 后处理开关 |
| POST | `/ai/set?on=0\|1` | （保留）AI 通道标记 |

### 2. 常驻监听，无闸门

- 监听在**注入即常驻**（`overlay.cpp` 的 `HttpApiStart(17832)`），与任何开关无关。
- 曾经的"脑机接口开关"只拦带 `&ai=1` 的请求，容易被误当成能力总开关 → **已彻底移除**。
  现在变更类请求一律放行；`ai=1` 仅作遥测（`g_aiCalls` 计数 + 日志），`g_aiOpen` 恒 1、`g_aiRejects` 恒 0。
- **安全边界靠"只绑 `127.0.0.1`"**，不靠开关。接入说明见 `docs/AI接入指南.md`。

