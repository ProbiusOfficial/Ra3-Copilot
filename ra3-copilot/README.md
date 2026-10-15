# Ra3-Copilot — 注入型修改器（进程内 D3D9 覆盖层）

面向**红色警戒 3 日冕 Mod**（`ra3_1.12.game`，v1.12.3444.25830，32 位）的注入型进程内作战辅助系统：以局内覆盖层（overlay）形式接入游戏，按 `INSERT` 呼出菜单，提供资源 / 战斗 / 单位 / 召唤 / 空投 / 战场感知 / AI 指挥等能力，并通过 **MCP 脑机接口**把战局开放给 AI。

> 仅用于单机学习与研究。**版本强绑定**：钩子 RVA 硬编码于 v1.12.3444.25830；注入前执行 AOB 字节级校验（fail-closed，不符绝不 patch）。

## 功能一览

| 分类 | 能力 |
|---|---|
| 渲染 | MinHook inline-hook d3d9 `Present`/`EndScene`，ImGui 直接画在游戏画面内（不抢焦点、切窗口不静音） |
| 资源 | 加钱 / 设置资金、电力满、科技点满、全科技、快速建造、敌方无法建造、免费建造 |
| 战斗 | 玩家无敌、选中单位弹药满、超武就绪 / 禁用敌方超武、危险等级干预、全图去迷雾、秒杀 |
| 单位 | 选中单位变速 / 改血 / 收编 / 摧毁 / 满级；召唤单位与基地车；空投箱子（支持周期自动空投） |
| 感知 | 全图活体对象枚举（单位 / 建筑 / AI / 坐标 / 血量，按归属分组）；`/objects` `/obj` 即席查询 |
| 指挥 | AIUpdate 直调：移动 / 攻击 / 建造（`/aicall`，均已实机验证） |
| 接口 | 进程内 HTTP `127.0.0.1:17832`（`/state` `/read` `/peek` `/summon` `/poke` `/call` `/aicall` …）；MCP stdio 服务器（`mcp/`）把上述能力开放给任意 AI 客户端 |

## 架构

```
injector.exe (x86) ──CreateRemoteThread(LoadLibraryW)──► ra3copilot.dll (in-game)
                                                            │
                       ┌────────────────────────────────────┼──────────────────────────┐
                       ▼                                    ▼                          ▼
              features.cpp (功能核)               overlay.cpp (渲染层)            输入线程
              ├ PlayerId 钩子 0xFF95B              ① 造 dummy window + dummy D3D9 device
              │   push/ret detour                   ② 读 device vtable[16/17/42]
              │   -> g_localPlayerId                 = Reset/Present/EndScene 的**函数地址**
              └ Money 钩子 0x6CFDFE                 ③ MinHook inline-hook 这些函数(+SwapChain::Present)
                  E9 detour -> cave -> stub         ④ 销毁 dummy
                  → [player+4] += g_moneyValue      ⑤ 在 Present detour 里用**游戏 device+窗口**
                                                       渲染 ImGui → 再调原 Present
```

注入器（x86，与游戏同位数）用 `CreateRemoteThread(LoadLibraryW)` 把 DLL 载入游戏；DLL 随后安装**功能钩子**（AOB 校验 → 代码洞 → detour）与**渲染钩子**（MinHook inline hook）。

### 为什么是 inline-hook 函数地址，而不是改 vtable / 自建窗口

| 走过的弯路 | 结果 |
|---|---|
| D3D9 **vtable hook**（改 dummy device 的 vtable 数组项） | 游戏自己的 device 用**另一张数组** → 改了对游戏无效（计数恒 0） |
| 扫内存找游戏 device（找"vtable 落在 d3d9 内"的对象） | 三次崩溃，成功率低、风险高 |
| 自建置顶窗口 + 自建设备 | 那是**独立窗口**，不是局内渲染（切窗口游戏会静音） |
| MinHook inline-hook `d3d9` 的 `Present`/`EndScene` **函数本身** | 计数持续增长，且**不抢焦点**（切换游戏窗口不再静音） |

关键：`d3d9.dll` 里 `Present`/`EndScene` 是**同一份函数代码**，被游戏自己的 device 共用；hook 函数本身后，任何 device 调用它都会命中。函数地址从 dummy device 的 vtable 读出。
dummy 窗口类名 `ra3copilot_overlay_dummy`，兜底窗口类名 `Command & Conquer(tm) Red Alert(tm) 3`。
详见 [`../docs/局内渲染解决方案.md`](../docs/局内渲染解决方案.md)。

## 构建

```
build.bat
```

要求：VS2019 BuildTools（x86 工具链 + MASM `ml.exe`）+ Windows SDK 10.0.19041。
依赖：`third_party/minhook-1.3.3`（BSD-2，静态编入；单独编译到 `obj_mh\`，避免 `hook.c` 与 `src/hook.cpp` 的 `hook.obj` 同名冲突）。
产物：`ra3copilot.dll`、`injector.exe`，均为 **x86**（游戏是 32 位进程 → 注入器必须同位数）。

> **工具链三坑（`build.bat` 已规避）**：
> ① `vcvars32.bat` 内部调 `reg.exe` 探测 SDK，而本机安全策略**黑名单硬拦 reg.exe** → INCLUDE 缺 SDK 路径。故不调 vcvars，改为**显式设 PATH/INCLUDE/LIB**。
> ② `.bat` 必须 **CRLF 行尾 + 纯 ASCII**（错乱会啃掉行结构）。
> ③ 64 位进程**无法**向 32 位进程 `CreateRemoteThread` → 注入只能用 `injector.exe`（x86）；Python（64 位）只做诊断（`ReadProcessMemory` 跨位数可用）。

改钩子 / 改 AOB 后，交付前用真实游戏 exe 做字节级预检：

```
python tools/precheck_aob.py --game "<ra3_1.12.game 路径>"
```

## 使用

1. **先启动游戏并进入对局**（遭遇战 / 战役均可）
2. **运行 `deploy.bat`**（自动 UAC 提权 → `injector.exe` 注入 → 打印诊断）
3. 游戏内 **`INSERT`** 显示/隐藏菜单；**`F7`** 加钱

菜单**画在游戏画面内**（非独立窗口）。显示菜单时会抢焦点（RTS 失焦会暂停），这是**有意的** —— 鼠标键盘才能操作 UI。

### 界面

- 顶栏分类页签：**资源 / 战斗 / 单位 / 设置 / 状态**
- 底部固定**输出记录**日志区（带时间戳、按级别着色：信息/成功/警告/错误）
- 「状态」页：钩子安装数、符号解析来源、渲染帧数、诊断按钮

### 热键

| 键 | 动作 |
|---|---|
| `INSERT` | 显示/隐藏菜单窗口 |
| `F7` | 按"每次金额"加钱（默认 10000） |

## 功能清单（AOB 钩子）

| 分类 | 功能 | 钩子 RVA | 洞内动作 |
|---|---|---|---|
| 资金 | 加钱 / 设为指定值 | `0x6CFDFE` | `[player+4]` 加/设（任意值） |
| | 本地玩家追踪 | `0x0FF95B` | 记录对象指针 + ID |
| 玩家属性 | 电力满 | `0x6CFD0D` | `[eax+4]=100000` |
| | 科技点满 | `0x6CFE6C` | `[eax+0x34]=15` |
| | 全科技 | `0x6CFEB5` | `[edi+0x2c]=400.0f` |
| | 快速建造 | `0x30F42E` `0x30F38C` `0x2F6CFF` | `[esi+0x1c]=100.0f`（3 钩子） |
| 建造 | 敌方无法建造 | `0x30F530` | 敌方 `[eax+4]` 压到 5000 / 1 |
| 战斗 | 玩家无敌 | `0x12EEDF` | 己方 `[esi+4]=[esi+0xc]=9999999.0f` |
| | 选中单位弹药满 | `0x128735` | `[ecx+0x1c]=0x7FFFFFFF` |
| | 矿厂复原 | `0x2D4933` | `[ecx+0x14]=0`（一次性） |
| | 危险等级 | `0x438ED0` | 最高 50000 / 最低 0 / 不干预 |
| | 超武就绪 | `0x2EBB69` | 己方 `[esi+0x20]=1` |
| | 禁用敌方超武 | `0x43ECA6` | `[eax+0xc]+=1` |
| 地图 | 全图（去迷雾） | `0x3C226C` | `[ebp+0x260]=99999.0f` |

**共 16 个钩子点 / 13 个功能**，每个都在安装前做 AOB 字节级校验；交付前用 `tools/precheck_aob.py` 对真实 exe 预检（应 20/20）。

**架构：表驱动**（`features.cpp` 的 `kHooks[]`）—— 新增功能 = 表里加一行 + 写一个壳（`stubs.asm`）+ UI 加个勾选框。

**免费建造**（`0x0E7565`）是 1 字节补丁（`75 0C` jne → `EB 0C` jmp），不走代码洞；单位操作 / 召唤 / 空投（`actions.cpp`）与 AI 指挥（`aicmd.cpp`）则直调游戏函数。

### 当前未接入

以下钩子点在 `sdk.h` 已登记 RVA/AOB，尚未接线：

| 钩子 | RVA | 备注 |
|---|---|---|
| SuperPower2 | `0x43EC19` | 洞内用 `[esp+0x28]` 比较（依赖 pushad 后的栈布局） |
| DisableAllSP2 | `0x30A5F6` | 该挂钩点只有原指令重放、无改值逻辑 |
| Zoom | `0x1EC7CD` | `fadd`/`fsub` 两档与 flag 的分支关系绕，语义待实测 |

## 模块清单（`src/`）

| 文件 | 职责 |
|---|---|
| `sdk.h` | 钩子 RVA / AOB / 结构偏移常量表 |
| `hook.h` / `hook.cpp` | 进程内补丁引擎：AOB 校验、代码洞、detour |
| `stubs.asm` | 钩子壳代码（MASM） |
| `features.h` / `features.cpp` | 功能核：双钩子（表驱动）+ 资金接口 + 遥测 |
| `overlay.h` / `overlay.cpp` | 渲染层：dummy device 取址 → MinHook → 局内绘制；菜单 / 日志 / 面板 |
| `actions.h` / `actions.cpp` | 单位操作 / 召唤 / 空投：直调游戏函数 + 创建后状态字后处理 |
| `aicmd.h` / `aicmd.cpp` | AI 指挥：AIUpdate 直调（移动 / 攻击 / 待机） |
| `battlefield.h` / `battlefield.cpp` | 感知层：全图战场枚举 + 快照缓存 |
| `battle_catalog.h` | 可玩对象目录（自动生成） |
| `symres.h` / `symres.cpp` | 符号解析：AOB 全模块扫描 + 结构推导 |
| `httpapi.h` / `httpapi.cpp` | 进程内 HTTP 调试接口 + MCP 端点 |
| `units_corona.h` | 可召唤单位 / 箱子代码表（自动生成） |
| `dllmain.cpp` | DLL 入口 |
| `injector.cpp` | 注入器 |

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
├── src/                见上「模块清单」
├── mcp/
│   ├── ra3_mcp.py          MCP stdio 服务器（零依赖 Python）
│   └── unit_catalog.json   单位目录（自动生成）
├── tools/              AOB 预检 / 注入诊断 / 渲染探针 / 目录生成等脚本
├── ra3copilot.dll      产物：注入模块
├── injector.exe        产物：注入器
└── ra3copilot.map      产物：链接器符号表（诊断工具用）
```

## 技术要点

### 1. 双钩子（功能核）

**PlayerId 钩子 `0xFF95B`**（6 字节 push/ret detour）：洞内重放原指令后把结果存 `g_localPlayerId`。
该值是**玩家 ID（整数）而非对象指针** —— 战役里 ID=4、遭遇战曾见 6，**槽位随关卡变**，不可硬编码。

**Money 钩子 `0x6CFDFE`**（5 字节 E9 detour → cave → 68/C3 → stub）：

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

### 2. 壳代码编译进 DLL（不动态生成机器码）

壳写成 MASM 过程编进 DLL `.text`：
- 全局变量引用 → `8B 0D <abs>` + **PE 重定位表**自动补址
- 跳回游戏 → `FF 25 <abs>` 间接跳，**无距离限制**
- detour 三段：游戏 `E9` → cave → `68/C3` 绝对跳 → stub

> **MASM 陷阱**：整数**默认十进制**！`[eax+28]` 会被编成 `[eax+1Ch]`（0x1C=28），
> 十六进制必须写 `28h`。壳代码**必须 `dumpbin /disasm` 逐字节核对**。

### 3. 渲染层（MinHook inline hook + 局内用游戏 device 绘制）

`overlay.cpp` 的 `InstallRenderHooks()`：
- `RegisterClassExW`(`ra3copilot_overlay_dummy`) + `CreateWindowExW` 建 **dummy 窗口**（64×64，用完即毁）
- `GetProcAddress(d3d9,"Direct3DCreate9")` → `IDirect3D9` → 对顶点处理 flag `{0x40,0x80,0x20}` 各 `CreateDevice`
  （另试 `Direct3DCreate9Ex`/`CreateDeviceEx`）→ 从 dummy device 读 `vtable[16/17/42]` = `Reset/Present/EndScene` 的**函数地址**
- `HookFunction()` 用 `MH_CreateHook(函数地址, detour, &原函数)` 挂钩（去重：同一地址只挂一次）→ 再经 `GetSwapChain(0)` 挂 `SwapChain::Present`(`vtable[3]`)
- 销毁 dummy，`MH_EnableHook(MH_ALL_HOOKS)`
- `HookPresent` 内：首帧 `EnsureImGui(dev)`（窗口来自 `dev->GetCreationParameters().hFocusWindow`，
  兜底 `FindWindowW(L"Command & Conquer(tm) Red Alert(tm) 3")` → `EnumWindows` → `GetForegroundWindow`；
  再 `SetWindowLongPtrW(GWLP_WNDPROC)` 子类化）→ `RenderOverlay(dev)` → 调原 `Present`
- `HookReset` 前后 `ImGui_ImplDX9_Invalidate/CreateDeviceObjects`；`HookEndScene` 仅计数
- 字体：`C:\Windows\Fonts\msyh.ttc`（失败回退默认）；输入用 `GetAsyncKeyState` 轮询 + WndProc 子类化

### 4. fail-closed 安全阀

两处钩子安装前都做 **AOB 字节级校验**（`03 78 04 8B 11` / `8B 50 28 8B 42 20`），不符**绝不 patch**。

### 5. 符号解析（AOB + 结构推导）

`symres.cpp` 对每个符号独立解析：特征码全模块扫描唯一命中即用；多命中取"离已知 RVA 最近"者；0 命中退回已知 RVA，但必须通过函数入口形态校验，否则标为解析失败（不盲调）。全局变量（ThingFactory / 选中管理器）用结构关系推导，不靠固定地址。换版本后只要特征码还在即可重新定位。

## 已知限制

- **版本绑定**：钩子 RVA 硬编码于 v1.12.3444.25830（AOB fail-closed 优雅失败）
- **观战模式**：资金钩子自动失效（设计如此）
- **窗口不记忆位置/尺寸**；字体不随缩放变化（仅控件缩放）
- **未提供卸载**（进程退出自动清理）
- 详细内存版图 / 钩子清单见 [`../docs/单位操作与召唤-已接入.md`](../docs/单位操作与召唤-已接入.md)、[`../docs/开发铁律.md`](../docs/开发铁律.md)
