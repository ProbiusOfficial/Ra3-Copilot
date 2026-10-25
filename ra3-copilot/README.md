# Ra3-Copilot — 注入型修改器

面向**红色警戒 3 日冕 Mod**（`ra3_1.12.game`，v1.12.3444.25830，32 位）的注入型进程内作战辅助系统：以局内覆盖层（overlay）形式接入游戏，按 `INSERT` 呼出菜单（可改键），提供资源 / 战斗 / 单位 / 召唤 / 空投 / 战场感知 / AI 指挥等能力，并通过 **MCP 脑机接口**把战局开放给 AI。

> 仅用于单机学习与研究。**二进制强绑定**：hook RVA 硬编码于一份具体的 `ra3_1.12.game`（指纹见[受支持的二进制](#受支持的二进制)），同一版本号的不同发行版互不通用；注入前执行 AOB 字节级校验（fail-closed，不符绝不 patch）。

## 功能一览

| 分类 | 能力 |
|---|---|
| 渲染 | MinHook inline-hook d3d9 `Present`/`EndScene`，ImGui 直接画在游戏画面内（不抢焦点、切窗口不静音） |
| 资源 | 加钱 / 设置资金、电力满、科技点满、全科技、快速建造、敌方无法建造、免费建造 |
| 战斗 | 玩家无敌、无限弹药（赋予后取消选中仍保持）、超武就绪 / 禁用敌方超武、危险等级干预、全图去迷雾、秒杀、无限缩放 |
| 单位 | 选中单位变速 / 改血 / 收编 / 摧毁 / 满级；根据单位代码（支援代码）召唤单位和建筑；空投箱与空投仓（海 / 陆两种）、海面折跃信标（按兵种抽取） |
| 协议 | 秘密协议与兵种升级直授（`/protocol` `/grant` `/upgrade`）；目录带授予门槛，不可授予项灰显 |
| 感知 | 全图活体对象枚举（单位 / 建筑 / AI / 坐标 / 血量，按归属分组）；`/objects` `/obj` `/snapshot` 即席查询与页级差分 |
| 指挥 | AIUpdate 直调：移动 / 攻击 / 建造（`/aicall`）；`/place` 建筑落点候选打分，引擎判定只做校验 |
| 接口 | 进程内 HTTP `127.0.0.1:17832`（57 条路由，`/state` `/read` `/peek` `/summon` `/protocol` `/place` `/snapshot` …）；MCP stdio 服务器（`mcp/`，19 个工具）把上述能力开放给任意 AI 客户端 |

## 架构

```
injector.exe (x86) ──CreateRemoteThread(LoadLibraryW)──► ra3copilot.dll (in-game)
                                                            │
                       ┌────────────────────────────────────┼──────────────────────────┐
                       ▼                                    ▼                          ▼
              features.cpp (功能核)               overlay.cpp (渲染层)            输入线程
              ├ PlayerId hook 0xFF95B              ① 造 dummy window + dummy D3D9 device
              │   push/ret detour                   ② 读 device vtable[16/17/42]
              │   -> g_localPlayerId                 = Reset/Present/EndScene 的**函数地址**
              └ Money hook 0x6CFDFE                 ③ MinHook inline-hook 这些函数(+SwapChain::Present)
                  E9 detour -> cave -> stub         ④ 销毁 dummy
                  → [player+4] += g_moneyValue      ⑤ 在 Present detour 里用**游戏 device+窗口**
                                                       渲染 ImGui → 再调原 Present
```

注入器（x86，与游戏同位数）用 `CreateRemoteThread(LoadLibraryW)` 把 DLL 载入游戏；DLL 随后安装**功能 hook**（AOB 校验 → code cave → detour）与**渲染 hook**（MinHook inline hook）。

渲染层 hook 的是 `d3d9` 的 `Present`/`EndScene` **函数本身**（不是改 vtable 数组项、也不是自建窗口），故不抢焦点、切窗口不静音。原理与实测见 [`../docs/局内渲染解决方案.md`](../docs/局内渲染解决方案.md)。

## 构建

```
build.bat
```

要求：VS2022 BuildTools（x86 工具链 + MASM `ml.exe`）+ Windows SDK 10.0.22621。
依赖：`third_party/minhook-1.3.3`（BSD-2，静态编入；单独编译到 `obj_mh\`，避免 `hook.c` 与 `src/hook.cpp` 的 `hook.obj` 同名冲突）。
产物：`build_out\Ra3Copilot.exe`（单文件发布版）、`ra3copilot.dll`、`injector.exe`，均为 **x86**（游戏是 32 位进程 → 注入器必须同位数）。

> **工具链三坑**：
> ① `vcvars32.bat` 内部调 `reg.exe` 探测 SDK，而本机安全策略**黑名单硬拦 reg.exe** → INCLUDE 缺 SDK 路径。故不调 vcvars，而**显式设 PATH/INCLUDE/LIB**。
> ② `.bat` 必须 **CRLF 行尾 + 纯 ASCII**（错乱会啃掉行结构）。
> ③ 64 位进程**无法**向 32 位进程 `CreateRemoteThread` → 注入只能用 x86 的 `Ra3Copilot.exe` / `injector.exe`；Python（64 位）只做诊断（`ReadProcessMemory` 跨位数可用）。

改 hook / 改 AOB 后，交付前用真实游戏 exe 做字节级预检：

```
python tools/precheck_aob.py --game "<ra3_1.12.game 路径>"
```

### 受支持的二进制

hook RVA 绑定的是**具体那一份 exe**，不是版本号：同为 `1.12.3444.25830` 的 Steam 版、整合包版、
被启动器静态改过的版本，大小与代码布局都不同，`precheck_aob.py` 会整体失配（0/30）。

| 项 | 受支持的值 |
|---|---|
| 大小 | `9306120`（`0x8E0008`） |
| SHA256 | `518ef27297d490590f0315b6143cc7ab0010bf606d2f351ca64d447c3f7b0221` |
| MD5 | `b77185b0df7868b95421c0f2c0493a16` |
| SHA1 | `1ab6cba7d9e3099fe9832c6eece8c994df187bf8` |
| 链接时间 | 2009-07-07 21:17:27 UTC |
| 节 / 入口 | 6 节；入口 `0xD398D`，ImageBase `0x400000`，SizeOfImage `0x93B000` |

自查（只读，打印指纹并给出是否受支持的判定）：

```
python tools/exe_fingerprint.py --game "<ra3_1.12.game 路径>"
```

**日冕 Mod 不是必需**：不带 Mod 直接用原版游戏也能注入并工作，仅日冕专属内容（协议树、新增单位代码）不可用。

## 使用

1. **先启动游戏并进入对局**（遭遇战 / 战役均可）
2. **双击 `Ra3Copilot.exe`**：自动识别游戏；点「注入修改器」
   - 游戏以管理员权限运行时，启动器会请求一次 UAC 提权
   - 已注入时主按钮变为「卸载修改器」；「重载修改器」= 卸载后重新注入
3. 游戏内 **`INSERT`** 显示/隐藏菜单（无此键的键盘见下「呼出键」）；能力按资源 / 战斗 / 单位 / 召唤 / 协议 / 感知分组

菜单**画在游戏画面内**（非独立窗口）。显示菜单时会抢焦点（RTS 失焦会暂停），这是**有意的** —— 鼠标键盘才能操作 UI。

单文件版会把运行文件释放到 `%LOCALAPPDATA%\Ra3Copilot\`；日志、按键配置、面板布局都在该目录。

### 界面

多浮动面板（呼出键总开关；面板可各自拖动，由主面板控制显隐）。启动默认只展开主面板。

| 面板 | 内容 |
|---|---|
| 主面板 | 面板显隐开关 / 全部展开 · 全部收起 / 隐藏全部 |
| 资源科技 | 本地玩家与资金；加钱、锁定金额；电力无限 / 科技点无限 / 全科技 / 快速建造 / 免费建造 / 恢复矿场 |
| 单位操作 | 作用于选中单位：无限弹药 / 复制 / 升级 / 锁定血量 / 无限血量 / 恢复血量 / 移速滑条 / 接管 / 摧毁 / 单位状态（铁幕 / 无敌 / 加速 / 减速 / 易碎 / 输出增强 / 重置）；调试详情可整段复制 |
| 支援召唤 | 阵营列表 + 单位·箱子列表 + 候选栏；支持中文名 / 拼音（首字母或全拼）/ 模板名 / 类型搜索；海面折跃信标可限定兵种 |
| 协议技能 | 秘密协议 / 兵种升级目录（按阵营过滤、带授予门槛），一键直授 |
| 杂项操作 | 玩家无敌 / 清除迷雾 / 超武就绪 / 禁用敌方超武 / 禁止电脑建造 / 危险等级 / 秒杀模式 / 无限缩放 |
| 设置 | 背景透明度；安全护栏（dry-run、禁用游戏函数调用）；崩溃日志落盘；鼠标拦截与落点来源；快捷键（总开关、逐条改键、恢复默认） |
| 输出记录 | 带时间戳、按级别着色（信息 / 成功 / 警告 / 错误） |
| 脑机接口 | MCP 战场态势 + 客户端配置 + AI 调用记录 |

### 热键

| 键 | 动作 |
|---|---|
| 呼出键（默认 `INSERT`） | 显示/隐藏菜单窗口 |

各功能热键均为默认值，见「设置 → 快捷键」：可总开关、可逐条改键，配置存在 DLL 同目录的 `ra3copilot_keys.ini`。

### 呼出键与快捷键

默认呼出键是 `INSERT`。75 配列等没有这个键的键盘，打不开面板也就进不了面板里的设置，所以两处都能改：

- **启动器**（`Ra3Copilot.exe`）→「设置」→「快捷键」：呼出键和全部功能热键都能改，点键名即可，Esc 取消；与已有绑定重复会被拒绝并提示。同一页还有「静默功能快捷键」——勾上后功能键不再触发任何动作，只保留呼出键。
- **局内面板** →「设置」→「快捷键」：同一份绑定，同样能改。

两处写的是同一个 `ra3copilot_keys.ini`。改完 1 秒内生效，不必重注入。

## 功能清单

| 分类 | 功能 | hook RVA | cave 内动作 |
|---|---|---|---|
| 资金 | 加钱 / 设为指定值 | `0x6CFDFE` | `[player+4]` 加/设（任意值） |
| | 本地玩家追踪 | `0x0FF95B` | 记录对象指针 + ID |
| 玩家属性 | 电力满 | `0x6CFD0D` | `[eax+4]=100000` |
| | 科技点满 | `0x6CFE6C` | `[eax+0x34]=15` |
| | 全科技 | `0x6CFEB5` | `[edi+0x2c]=400.0f` |
| | 快速建造 | `0x30F42E` `0x30F38C` `0x2F6CFF` | `[esi+0x1c]=100.0f`（3 hook） |
| 建造 | 敌方无法建造 | `0x30F530` | 敌方 `[eax+4]` 压到 5000 / 1 |
| | 移除科技锁定 | `0x43EA70` | 本机玩家 → `al=0`（可用对象集合判定恒过，黑锁变正常） |
| | 补全扩展建筑 | `0x6CB352` | 构建器取到 cs 后，把 `cs+4` 指向同族「扩展完整版」inner（四阵营） |
| | 扩展建筑解除灰色 | `0x6BB6F0` | 模板哈希命中 `EXT_BLD` → 直接 `setEnabled`，绕开前置科技链 |
| | 建造页探针 | `0x6B7070` | 只记录按钮对象（默认关） |
| 战斗 | 玩家无敌 | `0x12EEDF` | 己方 `[esi+4]=[esi+0xc]=9999999.0f` |
| | 抓取选中对象 | `0x128735` | 只记录 `g_selUnit`，不写任何字段（弹药见下） |
| | 矿厂复原 | `0x2D4933` | `[ecx+0x14]=0`（一次性） |
| | 危险等级 | `0x438ED0` | 最高 50000 / 最低 0 / 不干预 |
| | 超武就绪 | `0x2EBB69` | 己方 `[esi+0x20]=1` |
| | 禁用敌方超武 | `0x43ECA6` | `[eax+0xc]+=1` |
| | 秒杀模式 | `0x3651AE` | 命中时把敌方 `[esi+4]` 写 0（辅以 `0x3FE714` / `0x2E24E3` 记录当前血量体） |
| 地图 | 全图（去迷雾） | `0x3C226C` | `[ebp+0x260]=99999.0f` |
| | 无限缩放 | `0x1EC7CD` `0x1EC730` `0x1EC82D` | 开关开 → 出口置 ZF=1，`0x1EC7D2` 的 `je` 跳过 clamp；另两站只采样 |
| 协议 | 秘密协议格解锁 | `0x69A9B3` | `g_flags+0x18` 开 → `al=0`（跳过前置检查），同时记录原始 / 强制后标志 |
| | 协议格点击 / 锁定探测 | `0x69AB03` `0x67E9AB` | 只记录，不改任何行为 |

**共 30 个 hook 点（`kHooks[]` 30 条）**，每个都在安装前做 AOB 字节级校验；交付前用 `tools/precheck_aob.py` 对真实 exe 预检（应 30/30）。

**无限弹药不走 hook**：`UnitAmmo` 那一站现在只负责抓选中对象。真弹药在 `Thing+0x37C`（武器模块）→ 槽数组 `+0x24/+0x28` → 槽 `+0x94` 的条目数组 → 条目 `+0x1C`（int，实测 3→2→1→0 后换弹）。`actions.cpp AmmoFill` 逐条写 `0x7FFFFFFF`，并由常驻列表每帧补弹。⚠ 不要写 `Thing+0x1C` —— 那是 float 1.0，对它写值会崩（`eip=0x359DFA`）。

**架构：表驱动**（`features.cpp` 的 `kHooks[]`）—— 新增功能 = 表里加一行 + 写一个 stub（`stubs.asm`）+ UI 加个勾选框。

**免费建造**（`0x0E7565`）是 1 字节补丁（`75 0C` jne → `EB 0C` jmp），不走 code cave；单位操作 / 召唤 / 空投（`actions.cpp`）与 AI 指挥（`aicmd.cpp`）则直调游戏函数。

**无限缩放**（`Zoom` `0x1EC7CD` + `ZoomEntry` `0x1EC730` + `ZoomPost` `0x1EC82D`）：**核心是出口处的 ZF 位**，不是哪个字段。

距离每轮在 `0x1EC7CD` 写入，紧随其后：

```asm
0x5EC7C4  cmp  byte [esi+0x48], bl      ; ZF 由此 cmp 定；原生态走 clamp 分支
0x5EC7CD  movss [esi+0x44], xmm0        ; ← hook 点，5 字节
0x5EC7D2  je   0x5EC80D                 ; ZF=1 → 跳过整段 clamp
0x5EC7D4  .. 取 vt[0]/vt[1] 上下限 → call 0x4FDBD0 → fstp [esi+0x44]
```

`ZoomCaveStub` 先 `pushfd` 存下自然 flags，重放原指令，再用 `cmp byte [g_flags+0x0F],1` 自己造 ZF：开关开 → ZF=1，用 `pop eax` 把保存的自然 flags **丢掉**（`pop` 不改 EFLAGS），于是 `0x1EC7D2` 的 `je` 成立、整段 clamp（含写回）被跳过，`[esi+0x44]` 保留透传值，滚轮每档 ×1.0525 复利、无上限；开关关 → `popfd` 还原自然 flags，与无 hook 的原版逐字节一致。

⚠ 开态丢弃 flags 必须用 `pop eax`，**不能写 `add esp,4`**——`add` 会重算 EFLAGS，把 ZF 打回 0，静默失效。

⚠ 不要写 `[+0x34]`：它是游戏按距离派生的相机系数（600 ↔ 1.7），外部硬写必和引擎约束打架——实测写 3.4 更近、写 17 贴脸特写、写 0.17 直接黑屏。`0x1EC82D` 只采样 `[+0x44]` / `[+0x34]` / `[+0x74]`。

⚠ 不要写 `[this+0x2704]`（clamp 上限）：clamp 跑不跑由 ZF 决定，写上限治标不治本；而且关闭态没有任何代码把它算回 600，会污染原生行为。`ZoomEntryStub` 现在纯遥测。

⚠ 不要照抄 CE 的 `fadd`：上游 `0x1EC7A0~0x1EC7BC` 的 `fld` / `fst` / `fstp` 已经把 x87 栈弹空，`call 0x51D170` 也不留浮点值，进入 `0x1EC7CD` 时深度为 0。空栈 `fadd m32` 触发栈下溢，`ST(0)` 变 indefinite QNaN（`0xFFC00000`），随后 `fstp` 写进 `[+0x44]`，函数入口的 `fsub [+0x44]` 再把 NaN 累进 `[+0x2730]`；`NaN + x` 恒为 NaN，关开关也不自愈，只能重开对局。这就是最初「不能缩放且无法恢复」的原因。

CE 原表 Ctrl+F8 只翻 `iEnable+0xF` / `+0x10` 两个标志字节，是**纯透传**；它自己那段 `fadd` / `fsub` 因 `jne` 后挡着无条件 `jmp` 而不可达（同表「Player Map」写法正相反），所以那份脚本的弹簧扰动从未生效——真正起作用的是「跳过 clamp」这条路。

### 未接线的 hook 点

以下 hook 点在 `tools/scan_hooks.py` 已登记 RVA/AOB，但**未进 `kHooks[]`**（未接线）：

| hook | RVA | 备注 |
|---|---|---|
| SuperPower2 | `0x43EC19` | cave 内用 `[esp+0x28]` 比较（依赖 pushad 后的栈布局） |
| DisableAllSP2 | `0x30A5F6` | 该 hook 点只有原指令重放、无改值逻辑 |

## 模块清单（`src/`）

| 文件 | 职责 |
|---|---|
| `sdk.h` | hook RVA / AOB / 结构偏移常量表 |
| `hook.h` / `hook.cpp` | 进程内补丁引擎：AOB 校验、code cave、detour |
| `stubs.asm` | hook stub（MASM） |
| `features.h` / `features.cpp` | 功能核：双 hook（表驱动）+ 资金接口 + 遥测 |
| `overlay.h` / `overlay.cpp` | 渲染层：dummy device 取址 → MinHook → 局内绘制；菜单 / 日志 / 面板 |
| `actions.h` / `actions.cpp` | 单位操作 / 召唤 / 空投：直调游戏函数 + 创建后状态字后处理 |
| `aicmd.h` / `aicmd.cpp` | AI 指挥：AIUpdate 直调（移动 / 攻击；待机未实现，`privateIdle` 槽位待认） |
| `battlefield.h` / `battlefield.cpp` | 感知层：全图战场枚举 + 快照缓存 |
| `battle_catalog.h` | 可玩对象目录（自动生成） |
| `protocol.h` / `protocol.cpp` | 秘密协议 / 兵种升级授予：目录差分判定、多拼写重试 |
| `protocols_corona.h` | 技能名与授予门槛表（自动生成） |
| `symres.h` / `symres.cpp` | 符号解析：AOB 全模块扫描 + 结构推导 |
| `httpapi.h` / `httpapi.cpp` | 进程内 HTTP 调试接口 + MCP 端点 |
| `units_corona.h` | 可召唤单位 / 箱子代码表（自动生成） |
| `dllmain.cpp` | DLL 入口 |
| `launcher.cpp` | 单文件启动器（ImGui / D3D9；注入 / 卸载 / 重载 / 状态与日志） |
| `injectcore.h` / `injectcore.cpp` | 注入、卸载、模块表 / HTTP 判活（启动器与命令行共用） |
| `ui_theme.h` | 局内 overlay 与启动器共用的 ImGui 主题 / 中文字体 |
| `injector.cpp` | 命令行注入器 |

## 目录

```
ra3-copilot/
├── build.bat           一键构建（含独立编译 MinHook 到 obj_mh\）
├── deploy.bat          一键部署（自提权 → 注入 → 诊断）
├── refresh_dll.bat     重编译并覆盖产物
├── README.md           本文档
├── imgui/              Dear ImGui v1.91.4（核心 + dx9/win32 backend）
├── third_party/
│   └── minhook-1.3.3/  MinHook（inline hook，BSD-2）
├── res/                启动器资源（图标 / manifest / 内嵌 DLL 与 MCP）
├── src/                见上「模块清单」
├── mcp/
│   ├── ra3_mcp.py          MCP stdio 服务器（零依赖 Python）
│   └── unit_catalog.json   单位目录（自动生成）
├── tools/              AOB 预检 / 注入诊断 / 渲染探针 / 目录生成 / 图标生成等脚本
├── build_out/          Ra3Copilot.exe（单文件发布版）/ ra3copilot.dll
├── ra3copilot.dll      产物：注入模块
├── injector.exe        产物：命令行注入器
└── ra3copilot.map      产物：链接器符号表（诊断工具用）
```

## 技术要点

### 1. 双 hook

**PlayerId hook `0xFF95B`**（6 字节 push/ret detour）：cave 内重放原指令后把结果存 `g_localPlayerId`。
该值是**玩家 ID（整数）而非对象指针** —— 战役里 ID=4、遭遇战为 6，**槽位随关卡变**，不可硬编码。

**Money hook `0x6CFDFE`**（5 字节 E9 detour → cave → 68/C3 → stub）：

```
pushad/pushfd                  全保现场
inc  [g_telemEnter]            遥测
mov  ecx, g_localPlayerId
test ecx,ecx / je skip         观战保护
cmp  eax, ecx      / je match  双条件 OR：
cmp  [eax+0Ch],ecx / jne skip     a) 对象指针相等  b) [player+0xC] == ID
match:
mov  [g_localPlayerObj], eax   记录玩家对象（UI 读资金用）
mov  ecx, g_moneyOp / test / je skip
cmp  ecx,1 / jne setcase
  mov ecx,g_moneyValue / add [eax+4],ecx    op=1 加钱
jmp clear / setcase:
  mov ecx,g_moneyValue / mov [eax+4],ecx    op=2 设值
clear:
mov  [g_moneyOp],0 / inc [g_telemApplied]   一次性消费
skip:
popfd/popad
add  edi,[eax+4]               原指令 1 重放（与 AOB 逐字节一致）
mov  edx,[ecx]                 原指令 2 重放
jmp  [g_retMoney]              间接跳回（无 ±2GB 限制）
```

**双条件 OR 是关键设计**：该全局量到底存 ID 还是指针，静态分析无法 100% 确定，OR 两种都兼容。

### 2. stub 编译进 DLL

stub 写成 MASM 过程编进 DLL `.text`：
- 全局变量引用 → `8B 0D <abs>` + **PE 重定位表**自动补址
- 跳回游戏 → `FF 25 <abs>` 间接跳，**无距离限制**
- detour 三段：游戏 `E9` → cave → `68/C3` 绝对跳 → stub

> **MASM 陷阱**：整数**默认十进制**！`[eax+28]` 会被编成 `[eax+1Ch]`（0x1C=28），
> 十六进制必须写 `28h`。stub**必须 `dumpbin /disasm` 逐字节核对**。

### 3. 渲染层

`overlay.cpp` 的 `InstallRenderHooks()`：
- `RegisterClassExW`(`ra3copilot_overlay_dummy`) + `CreateWindowExW` 建 **dummy 窗口**（64×64，用完即毁）
- `GetProcAddress(d3d9,"Direct3DCreate9")` → `IDirect3D9` → 对顶点处理 flag `{0x40,0x80,0x20}` 各 `CreateDevice`
  （另试 `Direct3DCreate9Ex`/`CreateDeviceEx`）→ 从 dummy device 读 `vtable[16/17/42]` = `Reset/Present/EndScene` 的**函数地址**
- `HookFunction()` 用 `MH_CreateHook(函数地址, detour, &原函数)` hook（去重：同一地址只挂一次）→ 再经 `GetSwapChain(0)` 挂 `SwapChain::Present`(`vtable[3]`)
- 销毁 dummy，`MH_EnableHook(MH_ALL_HOOKS)`
- `HookPresent` 内：首帧 `EnsureImGui(dev)`（窗口来自 `dev->GetCreationParameters().hFocusWindow`，
  兜底 `FindWindowW(L"Command & Conquer(tm) Red Alert(tm) 3")` → `EnumWindows` → `GetForegroundWindow`；
  再 `SetWindowLongPtrW(GWLP_WNDPROC)` 子类化）→ `RenderOverlay(dev)` → 调原 `Present`
- `HookReset` 前后 `ImGui_ImplDX9_Invalidate/CreateDeviceObjects`；`HookEndScene` 仅计数
- 字体：`C:\Windows\Fonts\msyh.ttc`（失败回退默认）；输入用 `GetAsyncKeyState` 轮询 + WndProc 子类化

### 4. fail-closed 安全阀

两处 hook 安装前都做 **AOB 字节级校验**（`03 78 04 8B 11` / `8B 50 28 8B 42 20`），不符**绝不 patch**。

### 5. 符号解析

`symres.cpp` 对每个符号独立解析：特征码全模块扫描唯一命中即用；多命中取"离已知 RVA 最近"者；0 命中退回已知 RVA，但必须通过函数入口形态校验，否则标为解析失败（不盲调）。全局变量（ThingFactory / 选中管理器）用结构关系推导，不靠固定地址。换版本后只要特征码还在即可重新定位。

## 已知限制

- **版本绑定**：hook RVA 硬编码于 v1.12.3444.25830（AOB fail-closed 优雅失败）
- **观战模式**：资金 hook 自动失效（设计如此）
- **窗口不记忆位置/尺寸**；字体不随缩放变化（仅控件缩放）
- **未提供卸载**（进程退出自动清理）
- 详细内存版图 / hook 清单见 [`../docs/单位操作与召唤-已接入.md`](../docs/单位操作与召唤-已接入.md)；本条实现细节以 `src/sdk.h` 的常量表为准
