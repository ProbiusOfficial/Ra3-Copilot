; stubs.asm - Ra3-Copilot hook stub（MASM, x86）
; 编译进 DLL .text：全局符号引用由 PE 重定位自动解析，无需运行时 patch 绝对地址
; 跳回游戏一律间接跳 jmp [g_retXxx]，无 +-2GB 距离限制
;
; 注意 1: MASM 整数默认十进制，十六进制必须写 h 后缀
; 注意 2: 每个 stub 末尾"原指令重放"的字节必须与 hook 点的 AOB 逐字节一致
;        （dumpbin /disasm 反汇编核对）
; 注意 3: cave 内 pushad/pushfd 全保现场，可自由破坏寄存器；重放前已恢复
; 注意 4: movss/cvttss2si 等为 SSE 指令，需 .686 + .XMM
.686
.XMM
.model flat, C

EXTERNDEF g_localPlayerId : DWORD   ; 本地玩家 ID（= [[eax+28]+0x20]，整数）
EXTERNDEF g_buildVer       : DWORD  ; 目标构建：0 = 零售 1.12，1 = Steam TUC 1.12
EXTERNDEF g_zoomEntrySrc   : DWORD  ; ZoomEntry 原指令 mov eax,[abs] 的 abs（两构建不同）
EXTERNDEF g_lockProbeCall  : DWORD  ; 技能锁定探针要 call 的函数地址（两构建不同）
EXTERNDEF g_btnSetEnabled  : DWORD  ; 建造按钮 setEnabled(true) 的函数地址（两构建不同）
EXTERNDEF g_localPlayerObj: DWORD   ; 本地"队伍/玩家"对象（= [eax+28]，PlayerID hook 写；召唤当 team）
EXTERNDEF g_moneyObj     : DWORD    ; 加钱 hook 看到的"资金对象"（UI 读资金；与上者不同类，勿混用）
EXTERNDEF g_selUnit      : DWORD    ; 弹药 hook 遍历到的"当前选中单位"（对象指针）
EXTERNDEF g_selHits      : DWORD    ; 选中单位命中计数（每单位每帧 +1，UI 估数量用）
EXTERNDEF g_selTick      : DWORD    ; 最近一次命中的时标（由 C++ 每帧写 g_tickNow）
EXTERNDEF g_tickNow      : DWORD    ; C++ 每帧更新的帧计数
EXTERNDEF g_okUnit       : DWORD    ; 秒杀用：当前处理的单位
EXTERNDEF g_okBody       : DWORD    ; 秒杀用：当前单位血量体（与 hook 点 esi 比较）
EXTERNDEF g_retZoom      : DWORD
EXTERNDEF g_retZoomPost  : DWORD
EXTERNDEF g_retOneKill   : DWORD
EXTERNDEF g_retOKData1   : DWORD
EXTERNDEF g_retOKData2   : DWORD
EXTERNDEF g_moneyOp      : DWORD    ; 0=无 1=加钱 2=设置（一次性）
EXTERNDEF g_moneyValue   : DWORD    ; 金额
EXTERNDEF g_flags        : BYTE     ; 功能开关数组（索引对齐 CE 的 FLAGS 偏移）
EXTERNDEF g_telemEnter   : DWORD    ; 遥测：money cave 执行次数
EXTERNDEF g_telemApplied : DWORD    ; 遥测：加钱生效次数
EXTERNDEF g_paCount      : DWORD    ; 遥测：协议格刷新记录条数
EXTERNDEF g_paBuf        : DWORD    ; 遥测：协议格刷新记录 {esi, obj, 原始标志, 强制标志} × 64
EXTERNDEF g_pcCount      : DWORD    ; 遥测：协议格点击记录条数
EXTERNDEF g_pcBuf        : DWORD    ; 遥测：协议格点击记录 {esi, parent, obj, owned, listBegin, listEnd} × 64
EXTERNDEF g_zoomHits     : DWORD    ; 遥测：Zoom cave 进入次数
EXTERNDEF g_zoomCam34    : DWORD    ; 遥测：ZoomPostStub 采样的 [+0x34] 相机系数
EXTERNDEF g_zoomPitch    : DWORD    ; 遥测：ZoomPostStub 采样的 [+0x74] 俯仰角
EXTERNDEF g_zoomObj      : DWORD    ; 遥测：最近一次 this 指针
EXTERNDEF g_zoomVal      : DWORD    ; 遥测：进入时 [esi+0x44]
EXTERNDEF g_zoomPost     : DWORD    ; 遥测：运算后 [esi+0x44]
EXTERNDEF g_zoomEntry    : DWORD    ; 遥测：缩放函数入口命中次数（含被早退的）

; ---- C++ 侧可调用函数（__cdecl）----
EXTERNDEF Ra3BuildExtFix : PROC     ; 战役建造页补全扩展建筑：入参 cs，返回新 inner（0=未改）
EXTERNDEF Ra3IsExtTpl    : PROC     ; 入参模板哈希，返回 1=扩展建筑（苏/盟/天朝任一）

; ---- 各 hook 返回地址（运行时由 C++ 填充 = site + 重放长度）----
EXTERNDEF g_retMoney        : DWORD
EXTERNDEF g_retPlayerId     : DWORD
EXTERNDEF g_retPower        : DWORD
EXTERNDEF g_retSCPoint      : DWORD
EXTERNDEF g_scPointObj      : DWORD
EXTERNDEF g_scPointVal      : DWORD
EXTERNDEF g_retHaveAllSC    : DWORD
EXTERNDEF g_retProtoAll     : DWORD
EXTERNDEF g_retProtoClick   : DWORD
EXTERNDEF g_retLockProbe     : DWORD    ; 技能按钮锁定探针返回地址
EXTERNDEF g_lpCount          : DWORD    ; 探针命中条数
EXTERNDEF g_lpBuf            : DWORD    ; 探针记录 {this, esi, [this+4]} × 64
EXTERNDEF g_retBuildLock     : DWORD    ; 建造按钮禁用态探针返回地址
EXTERNDEF g_blCount          : DWORD    ; 禁用探针去重后的模板条数
EXTERNDEF g_blBuf            : DWORD    ; 禁用探针记录 {tpl, thing, hits} × 128
EXTERNDEF g_retBuildTip      : DWORD    ; 禁用原因 tooltip 探针返回地址
EXTERNDEF g_tipCount         : DWORD    ; tooltip 探针去重后的条数
EXTERNDEF g_tipBuf           : DWORD    ; tooltip 探针记录 {ecx, arg0, arg1, hits} × 128
EXTERNDEF g_retScience       : DWORD    ; 建造前置科技判定桩返回地址
EXTERNDEF g_retClanGate      : DWORD    ; 可用对象集合判定桩返回地址（0x43EA7A）
EXTERNDEF g_localPlayerObj   : DWORD    ; 本机 Player 对象（PlayerIdCaveStub 写入）
EXTERNDEF g_clanCount        : DWORD    ; ClanGate 询问去重条数
EXTERNDEF g_clanBuf          : DWORD    ; ClanGate 询问记录 {template, 0, hits} × 128
EXTERNDEF g_retBtnProbe      : DWORD    ; 建造页按钮对象探针返回地址（0xAB7075）
EXTERNDEF g_bpCount          : DWORD    ; 按钮对象去重条数
EXTERNDEF g_bpBuf            : DWORD    ; 按钮对象地址表 × 64
EXTERNDEF g_retCsProbe       : DWORD    ; CommandSet 探针返回地址（0xACB35A）
EXTERNDEF g_retBtnEnable     : DWORD    ; 扩展建筑按钮强制可用桩返回地址（0xABB6F6）
EXTERNDEF g_csCount          : DWORD    ; CommandSet 去重条数
EXTERNDEF g_csBuf            : DWORD    ; CommandSet 指针表 × 16
EXTERNDEF g_retFastBuild1   : DWORD
EXTERNDEF g_retFastBuild2   : DWORD
EXTERNDEF g_retFastBuild3   : DWORD
EXTERNDEF g_retMap          : DWORD
EXTERNDEF g_retUnitAmmo     : DWORD
EXTERNDEF g_retDangerLevel  : DWORD
EXTERNDEF g_retOreMine      : DWORD
EXTERNDEF g_retGodMode      : DWORD
EXTERNDEF g_retEnemyCantBuild : DWORD
EXTERNDEF g_retSuperPower     : DWORD
EXTERNDEF g_retDisableAllSP   : DWORD
EXTERNDEF g_retZoomEntry      : DWORD
EXTERNDEF g_retBuildExtCs     : DWORD   ; = 构建器 hook 站 + 8（跳回点）
EXTERNDEF g_extReplaceFn      : DWORD   ; C 侧「同族完整版句柄」替换函数指针
EXTERNDEF g_btnForceCount     : DWORD   ; 0x1E 命中扩展建筑模板的次数（演练也记）
EXTERNDEF g_btnForceReal      : DWORD   ; 0 = 只计数不动作（安全演练）；1 = 真调 setEnabled
EXTERNDEF g_btnLastObj        : DWORD   ; 最近一次命中的按钮对象指针（只读取证用）
EXTERNDEF g_btnLastHash       : DWORD   ; 最近一次命中的模板哈希（只读取证用）
EXTERNDEF g_retBuildability   : DWORD   ; Buildability hook 跳回点（站 + 8）
EXTERNDEF g_retSetDis         : DWORD   ; setDisabled hook 跳回点（站 + 5）
EXTERNDEF g_retCsKey          : DWORD   ; cs key hook 跳回点（站 + 9，落在 call find 上）
EXTERNDEF g_cbResult           : DWORD   ; 第二道门（按钮刷新 0x65046E 那 call）的返回值
EXTERNDEF g_cbTemplate         : DWORD   ; 该门的 Template 参数（ebp）
EXTERNDEF g_cbEbx              : DWORD   ; 该门的第一个参数（ebx）
EXTERNDEF g_cbEsi              : DWORD   ; 该门看到的对象（esi，含 +0x470/+0x471 两个标志字节）
EXTERNDEF g_cbCount            : DWORD   ; 命中计数
EXTERNDEF g_cbTargetHash       : DWORD   ; 只记录这个模板哈希（默认 胡杨防空 6094F50D）
EXTERNDEF g_retCanProbe        : DWORD   ; 探针跳回点（站 + 6）
EXTERNDEF g_setDisRet          : DWORD   ; setDisabled 入口的调用者返回地址（只读取证）
EXTERNDEF g_setDisArg0         : DWORD   ; setDisabled 入口的第一个栈参数（只读取证）
EXTERNDEF g_mapCsKeyFn        : DWORD   ; C 侧 key 映射函数指针（入参 producer,key；返回 0=不改）
EXTERNDEF g_canProbeFn        : DWORD   ; C 侧取证回调：void(template, ret, arg0, arg1)
EXTERNDEF g_cbProbeOn         : DWORD   ; 门探针开关（1=记录，0=完全旁路）
EXTERNDEF g_csStoreSlot       : DWORD   ; CommandSet 注册表那个全局槽的地址（两构建不同）
EXTERNDEF g_csKeyPtr          : DWORD   ; CsKeyCaveStub 入口存下的 &key

.code

; ===========================================================================
; PlayerIdCaveStub  <- base+0x0FF95B  原指令 mov edx,[eax+28]; mov eax,[edx+20] (6B)
;   同时记录：对象指针 [eax+28] -> g_localPlayerObj
;            玩家 ID   [[eax+28]+20h] -> g_localPlayerId
; ===========================================================================
PlayerIdCaveStub PROC
    pushad                              ; 60
    pushfd                              ; 9C
    mov   edx, dword ptr [eax+28h]      ; 8B 50 28   原指令 1（同时也取到对象）
    test  edx, edx
    jz    pi_clear
    mov   g_localPlayerObj, edx         ; 89 15 <abs>  记录对象指针
    mov   eax, dword ptr [edx+20h]      ; 8B 42 20   原指令 2（取 ID）
    mov   g_localPlayerId, eax
    jmp   pi_replay
pi_clear:
    ; 不清零：保留"上一次有效值"（UI 按钮是异步点击的，清零会让收编/召唤当场失效）
pi_replay:
    popfd                               ; 9D
    popad                               ; 61
    mov   edx, dword ptr [eax+28h]      ; 8B 50 28   原指令 1 重放
    mov   eax, dword ptr [edx+20h]      ; 8B 42 20   原指令 2 重放
    jmp   dword ptr g_retPlayerId       ; FF 25 <abs>
PlayerIdCaveStub ENDP

; ===========================================================================
; MoneyCaveStub  <- base+0x6CFDFE  原指令 add edi,[eax+4]; mov edx,[ecx] (5B)
;   双条件 OR 匹配本地玩家；op=1 加钱 / op=2 设值；一次性消费
; ===========================================================================
MoneyCaveStub PROC
    pushad                              ; 60
    pushfd                              ; 9C
    inc   dword ptr g_telemEnter
    mov   ecx, g_localPlayerId
    test  ecx, ecx
    jz    mo_skip
    cmp   eax, g_moneyObj
    je    mo_match
    cmp   dword ptr [eax+0Ch], ecx
    jne   mo_skip
mo_match:
    mov   g_moneyObj, eax
    mov   ecx, g_moneyOp
    test  ecx, ecx
    jz    mo_skip
    cmp   ecx, 1
    jne   mo_setcase
    mov   ecx, g_moneyValue
    add   dword ptr [eax+04h], ecx      ; 01 48 04   [player+4] += value
    jmp   mo_clear
mo_setcase:
    mov   ecx, g_moneyValue
    mov   dword ptr [eax+04h], ecx      ; 89 48 04   [player+4] = value
mo_clear:
    mov   dword ptr g_moneyOp, 0
    inc   dword ptr g_telemApplied
mo_skip:
    popfd
    popad
    add   edi, dword ptr [eax+04h]      ; 03 78 04   原指令 1 重放
    mov   edx, dword ptr [ecx]          ; 8B 11      原指令 2 重放
    jmp   dword ptr g_retMoney
MoneyCaveStub ENDP

; ===========================================================================
; PowerCaveStub  <- base+0x6CFD0D  AOB 8B40048B8EB0030000 (9B)
;   [eax+4]=100000, [eax+8]=0
; ===========================================================================
PowerCaveStub PROC
    pushad
    pushfd
    cmp   byte ptr [g_flags+8], 0
    je    pw_skip
    mov   dword ptr [eax+04h], 000186A0h    ; 100000
    mov   dword ptr [eax+08h], 0
pw_skip:
    popfd
    popad
    mov   eax, dword ptr [eax+04h]      ; 8B 40 04   原指令 1 重放
    mov   ecx, dword ptr [esi+000003B0h]; 8B 8E B0 03 00 00  原指令 2 重放
    jmp   dword ptr g_retPower
PowerCaveStub ENDP

; ===========================================================================
; SCPointCaveStub <- base+0x6CFE6C  AOB 8B78348B4E3C (6B, push/ret)
;   [eax+0x34]=15
; ===========================================================================
SCPointCaveStub PROC
    pushad
    pushfd
    mov   dword ptr [g_scPointObj], eax
    mov   edi, dword ptr [eax+034h]
    mov   dword ptr [g_scPointVal], edi
    cmp   byte ptr [g_flags+9], 0
    je    sc_skip
    mov   dword ptr [eax+034h], 0000000Fh   ; 15
sc_skip:
    popfd
    popad
    mov   edi, dword ptr [eax+034h]     ; 8B 78 34   原指令 1 重放
    mov   ecx, dword ptr [esi+03Ch]     ; 8B 4E 3C   原指令 2 重放
    jmp   dword ptr g_retSCPoint
SCPointCaveStub ENDP

; ===========================================================================
; HaveAllSCCaveStub <- base+0x6CFEB5  AOB F30F10472C (5B)
;   [edi+0x2c]=400.0f
; ===========================================================================
HaveAllSCCaveStub PROC
    pushad
    pushfd
    cmp   byte ptr [g_flags+0Ah], 0
    je    ha_skip
    mov   dword ptr [edi+02Ch], 043C78000h  ; 400.0f
ha_skip:
    popfd
    popad
    movss xmm0, dword ptr [edi+02Ch]    ; F3 0F 10 47 2C  原指令重放
    jmp   dword ptr g_retHaveAllSC
HaveAllSCCaveStub ENDP

; ===========================================================================
; FastBuild 三处共用 g_flags+0Ch -> [.. +0x1c] = 100.0f
; ===========================================================================
FastBuild1CaveStub PROC                 ; base+0x30F42E  AOB F30F2C461C (5B)
    pushad
    pushfd
    cmp   byte ptr [g_flags+0Ch], 0
    je    fb1_skip
    mov   dword ptr [esi+01Ch], 042C80000h  ; 100.0f
fb1_skip:
    popfd
    popad
    cvttss2si eax, dword ptr [esi+01Ch] ; F3 0F 2C 46 1C  原指令重放
    jmp   dword ptr g_retFastBuild1
FastBuild1CaveStub ENDP

FastBuild2CaveStub PROC                 ; base+0x30F38C  AOB D9461C89465C (6B, push/ret)
    pushad
    pushfd
    cmp   byte ptr [g_flags+0Ch], 0
    je    fb2_skip
    mov   dword ptr [esi+01Ch], 042C80000h
fb2_skip:
    popfd
    popad
    fld   dword ptr [esi+01Ch]          ; D9 46 1C     原指令 1 重放
    mov   dword ptr [esi+05Ch], eax     ; 89 46 5C     原指令 2 重放
    jmp   dword ptr g_retFastBuild2
FastBuild2CaveStub ENDP

FastBuild3CaveStub PROC                 ; base+0x2F6CFF  AOB D986BC010000 (6B, push/ret)
    ; 基于 CE：这里**不动 [esi+0x1bc]**（那是别的计时器，写它会卡建造），
    ;   只做 [edi+0x14] = [edi+0x3c] + 1（建造队列计时一致性），且 ebp==1 时跳过
    pushad
    pushfd
    cmp   byte ptr [g_flags+0Ch], 0
    je    fb3_skip
    cmp   ebp, 1
    je    fb3_skip
    cmp   edi, 00010000h                ; 护栏：edi 不是原指令提供的，可能是垃圾值
    jb    fb3_skip
    mov   eax, dword ptr [edi+3Ch]
    inc   eax
    mov   dword ptr [edi+14h], eax
fb3_skip:
    popfd
    popad
    fld   dword ptr [esi+000001BCh]     ; D9 86 BC 01 00 00  原指令重放
    jmp   dword ptr g_retFastBuild3
FastBuild3CaveStub ENDP

; ===========================================================================
; MapCaveStub <- base+0x3C226C  AOB F30F118560020000 (8B, 单指令)
;   [ebp+0x260]=99999.0f
; ===========================================================================
MapCaveStub PROC
    ; 原指令是"写" [ebp+0x260]；必须先重放原指令、再覆盖，否则我们的值会被它冲掉（之前迷雾无效的根因）
    movss dword ptr [ebp+00000260h], xmm0 ; F3 0F 11 85 60 02 00 00  原指令**先**重放
    pushfd
    pushad
    cmp   byte ptr [g_flags+11h], 0
    je    mp_skip
    mov   dword ptr [ebp+00000260h], 047C35000h  ; 100000.0f（基于 CE：47c35000）
mp_skip:
    popad
    popfd
    jmp   dword ptr g_retMap
MapCaveStub ENDP

; ===========================================================================
; ZoomEntryStub <- base+0x1EC730  AOB A150B7CD00 5333DB 3898BC000000 (14B, 只重放前 5B)
;   只遥测，不写任何游戏字段。
;   历史：旧版开关开时把 [ecx+0x2704]（clamp 上限）写成 g_zoomMax 想放开滚轮上限，
;     实测时灵时不灵 —— clamp 跑不跑由 0x1EC7D2 的 je 决定（ZF 来自 0x1EC7C4 的
;     cmp [esi+0x48],bl），与写给 +0x2704 的值没关系；且关掉后 +0x2704 残留 20000
;     会让关闭态也能拉到 20000，反而破坏原生行为。放开上限已由 ZoomCaveStub 的
;     ZF 机制接管，本 stub 不再碰游戏内存。
; ===========================================================================
ZoomEntryStub PROC
    pushad
    inc   dword ptr [g_zoomEntry]
    mov   dword ptr [g_zoomObj], ecx
    popad
    mov   eax, dword ptr [g_zoomEntrySrc]  ; 原指令 A1 xx xx xx xx 的绝对地址（按构建取）
    mov   eax, dword ptr [eax]             ; 原指令重放：mov eax,[abs]
    jmp   dword ptr g_retZoomEntry
ZoomEntryStub ENDP

; ===========================================================================
; ZoomCaveStub <- base+0x1EC7CD  AOB F30F1146447439 (7B, 原指令 5B: movss [esi+0x44],xmm0)
;   机制（2026-10-17 破译，字节级复刻 2009 CE trainer 的 Ctrl+F8）：
;     0x1EC7C4: cmp byte [esi+0x48],bl        ← je 的 ZF 来源（bl=1 时常量走 clamp）
;     0x1EC7CD: movss [esi+0x44],xmm0         ← 本 hook（patch 5B）
;     0x1EC7D2: je 0x5EC80D                    ← ZF=1 即跳过 0x1EC7D4..0x1EC80C 的 clamp
;     0x1EC7D4..0x1EC806: vt[1]/vt[0] 取上下限 + call 0x4fdbd0 夹 [esi+0x44]
;   开关开 -> 本 stub 末尾 cmp [g_flags+0Fh],1 留下 ZF=1 -> je 跳过 clamp ->
;     距离原样透传，无上限（参考实现实测 600→631→663→…→810；本方 POC 600→1827，
;     每档 ×1.05 复利增长）。
;   开关关 -> popfd 还原自然 ZF（0x1EC7C4 的 cmp 结果）-> 与无 hook 原版逐字节一致
;     （原生基线实测：拉远封顶 600.00，拉近正常 600→575→551→528）。
;   ⚠ 绝不能写 fadd m32：上游 x87 栈已弹空，空栈 fadd 写 indefinite QNaN 污染 [+0x2730]。
;   ⚠ 开态丢弃 flags 用 pop eax 而非 add esp,4：add 会重建 EFLAGS 把 ZF 打回 0。
; ===========================================================================
ZoomCaveStub PROC
    pushad                       ; 通用寄存器（PUSH/POP 不动 EFLAGS）
    pushfd                       ; 保存自然 flags = 0x1EC7C4 cmp 的 ZF
    movss dword ptr [esi+44h], xmm0     ; F3 0F 11 46 44  原指令重放
    inc   dword ptr [g_zoomHits]
    mov   dword ptr [g_zoomObj], esi
    mov   eax, dword ptr [esi+44h]
    mov   dword ptr [g_zoomVal], eax
    mov   dword ptr [g_zoomPost], eax
    cmp   byte ptr [g_flags+0Fh], 1     ; 开 -> ZF=1（je 跳过 clamp）
    jne   zc_native
    pop   eax                           ; 开 -> 丢弃自然 flags，保留 ZF=1
    jmp   zc_out
zc_native:
    popfd                               ; 关 -> 还原自然 flags
zc_out:
    popad
    jmp   dword ptr g_retZoom
ZoomCaveStub ENDP

; ===========================================================================
; ZoomPostStub <- base+0x1EC82D  AOB 889E54260000 E8B8EAFFFF (11B, 重放前 6B)
;   只重放 + 遥测，**不写任何游戏字段**。
;   上一版在这里把 [esi+0x34] 强行写成 [esi+0x44] × 0.0028333，结果画面坏掉：
;   [esi+0x34] 是游戏自己从距离推出来的量（600 -> 1.7，实测反比/正比都试过，
;   写 0.17 直接黑屏、写 17 变贴脸），外部硬写一定和引擎的其它约束打架。
;   现在只采样 [+0x44]/[+0x34]/[+0x74]，供 actions.cpp 的 [诊断] 输出显示。
;   重放 `mov [esi+0x2654], bl`（6B）后跳回 0x1EC833 的 call，栈天然平衡。
; ===========================================================================
ZoomPostStub PROC
    mov   byte ptr [esi+2654h], bl      ; 88 9E 54 26 00 00  原指令重放
    pushad
    mov   eax, dword ptr [esi+44h]
    mov   dword ptr [g_zoomPost], eax   ; 采样：[+0x44] 逻辑距离
    mov   eax, dword ptr [esi+34h]
    mov   dword ptr [g_zoomCam34], eax  ; 采样：[+0x34] 相机系数
    mov   eax, dword ptr [esi+74h]
    mov   dword ptr [g_zoomPitch], eax  ; 采样：[+0x74] 俯仰
    popad
    jmp   dword ptr g_retZoomPost
ZoomPostStub ENDP

; ===========================================================================
; OneKillCaveStub <- base+0x3651AE  AOB F30F1146047420 (7B, 原指令 5B: movss [esi+4],xmm0)
;   基于 CE：若 esi 是"记录中的血量体"、对象状态位非全 0、归属不是自己 → HP 置 0
; ===========================================================================
OneKillCaveStub PROC
    push  eax
    cmp   esi, g_okBody
    jne   ok_exit
    mov   eax, dword ptr [esi-08h]
    test  eax, eax
    jz    ok_exit
    push  ebx
    mov   ebx, dword ptr [eax+00000390h]
    or    ebx, dword ptr [eax+00000394h]
    or    ebx, dword ptr [eax+000003A0h]
    or    ebx, dword ptr [eax+000003ACh]
    pop   ebx
    jz    ok_exit
    cmp   byte ptr [g_flags+17h], 1
    jne   ok_exit
    mov   eax, dword ptr [eax+00000418h]      ; 归属对象
    mov   eax, dword ptr [eax+20h]            ; 归属玩家 ID
    cmp   eax, g_localPlayerId
    je    ok_exit                              ; 自己的不动
    ; 注：此处把 HP 写 0 **不会**产生"幽灵单位"—— 本 hook 挂在游戏"写血量"的流程内
    ;   （原指令 movss [esi+4],xmm0），写完游戏会继续走自己的死亡判定与销毁。
    ;   实测：靶子 hp→0 后约 12s 被引擎回收（对象地址失效）。
    ;   ⚠️ 反之，在游戏流程**之外**直接写血量 0 才会留下幽灵（对象永不回收、模型仍在）。
    xor   eax, eax
    mov   dword ptr [esi+04h], eax
    mov   dword ptr [esi+0Ch], eax
    movss xmm0, dword ptr [esi+0Ch]
ok_exit:
    movss dword ptr [esi+04h], xmm0            ; 原指令重放
    pop   eax
    test  eax, eax
    jmp   dword ptr g_retOneKill
OneKillCaveStub ENDP

; ===========================================================================
; OKData1CaveStub <- base+0x3FE714  AOB 8B8E3C0300008B95 (8B, 原指令 6B: mov ecx,[esi+0x33c])
;   顺带记录 单位(esi) 与 血量体
; ===========================================================================
OKData1CaveStub PROC
    mov   ecx, dword ptr [esi+0000033Ch]       ; 原指令
    mov   g_okUnit, esi
    mov   g_okBody, ecx
    jmp   dword ptr g_retOKData1
OKData1CaveStub ENDP

; ===========================================================================
; OKData2CaveStub <- base+0x2E24E3  AOB 8B893C03000085C9740C (10B, 原指令 6B: mov ecx,[ecx+0x33c])
; ===========================================================================
OKData2CaveStub PROC
    mov   g_okUnit, ecx
    mov   ecx, dword ptr [ecx+0000033Ch]       ; 原指令
    mov   g_okBody, ecx
    jmp   dword ptr g_retOKData2
OKData2CaveStub ENDP

; ===========================================================================
; UnitAmmoCaveStub <- base+0x128735  AOB 8B0CBA85C9 (5B)
;   这个位置在"选中面板条状 UI"遍历选中对象向量的循环里：
;     lea eax,[esp+0x20] / push eax / call 0x79a940   ; 取选中对象向量 begin/end
;     mov edx,[esp+0x20] ... mov ecx,[edx+edi*4]      ; 逐项取出，元素 = 选中对象 Thing
;   本 stub 只做一件事：把遍历到的选中对象抓进 g_selUnit（选中动作的兜底来源）。
;
;   ⚠ 不写 [ecx+0x1C]：那是 Thing 上的 float 1.0，不是弹药字段（弹药在
;     WeaponEntry+0x1C，走 Thing+0x37C → 槽数组 → 条目，见 actions.cpp AmmoFill）。
;     旧实现无条件写 0x7FFFFFFF ⇒ 对护盾发生器这类对象写坏结构，
;     2026-10-19 / 10-20 两次崩在 RVA 0x359DFA（eip 落在指令中段）。
; ===========================================================================
UnitAmmoCaveStub PROC
    pushad
    pushfd
    ; 这段代码正在遍历"选中单位"数组（原指令 mov ecx,[edx+edi*4]），顺便抓一个出来
    mov   eax, dword ptr [edx+edi*4]
    test  eax, eax
    jz    ua_nosel
    mov   g_selUnit, eax
    inc   dword ptr g_selHits
    mov   eax, g_tickNow
    mov   g_selTick, eax
ua_nosel:
    popfd
    popad
    mov   ecx, dword ptr [edx+edi*4]    ; 8B 0C BA     原指令 1 重放
    test  ecx, ecx                      ; 85 C9        原指令 2 重放
    jmp   dword ptr g_retUnitAmmo
UnitAmmoCaveStub ENDP

; ===========================================================================
; DangerLevelCaveStub <- base+0x438ED0  AOB 8B8178120000 (6B, push/ret)
;   g_flags+13 == 1 -> [eax+8]=50000 ; == 2 -> [eax+8]=0
; ===========================================================================
DangerLevelCaveStub PROC
    ; 原指令 mov eax,[ecx+0x1278] **本身才把目标对象装进 eax**！
    ;    必须先重放它、再写 [eax+8]；否则 eax 是垃圾 → 写飞 → 闪退（本机闪退根因）
    mov   eax, dword ptr [ecx+00001278h] ; 8B 81 78 12 00 00  原指令**先**重放
    pushfd
    pushad
    test  eax, eax
    jz    dl_skip
    cmp   byte ptr [g_flags+13h], 1
    jne   dl_test2
    mov   dword ptr [eax+08h], 0000C350h   ; 50000
    jmp   dl_skip
dl_test2:
    cmp   byte ptr [g_flags+13h], 2
    jne   dl_skip
    mov   dword ptr [eax+08h], 0
dl_skip:
    popad
    popfd
    jmp   dword ptr g_retDangerLevel
DangerLevelCaveStub ENDP

; ===========================================================================
; OreMineCaveStub <- base+0x2D4933  AOB 8B40082B4114 (6B, push/ret)
;   [ecx+0x14]=0（一次性：置位后自动清零开关）
; ===========================================================================
OreMineCaveStub PROC
    pushad
    pushfd
    cmp   byte ptr [g_flags+14h], 0
    je    om_skip
    mov   byte ptr [g_flags+14h], 0        ; 一次性消费
    mov   dword ptr [ecx+014h], 0
om_skip:
    popfd
    popad
    mov   eax, dword ptr [eax+08h]      ; 8B 40 08     原指令 1 重放
    sub   eax, dword ptr [ecx+014h]     ; 2B 41 14     原指令 2 重放
    jmp   dword ptr g_retOreMine
OreMineCaveStub ENDP

; ===========================================================================
; GodModeCaveStub <- base+0x12EEDF  AOB 8B503C8BCE (5B)
;   己方单位（[ [object+0x138]+0x418 ] == g_localPlayerObj）：
;   [esi+4]=9999999.0f, [esi+0xc]=9999999.0f
; ===========================================================================
GodModeCaveStub PROC
    pushad
    pushfd
    cmp   byte ptr [g_flags+16h], 0
    je    gm_skip
    mov   ecx, dword ptr [ebx+00000138h]
    test  ecx, ecx
    jz    gm_skip
    mov   edi, dword ptr [ecx+00000418h]
    cmp   edi, g_localPlayerObj
    jne   gm_skip
    cmp   esi, 00010000h            ; 护栏：不像堆指针就别写（防闪退）
    jb    gm_skip
    mov   dword ptr [esi+04h], 0497423F0h  ; 9999999.0f
    mov   dword ptr [esi+0Ch], 0497423F0h
gm_skip:
    popfd
    popad
    mov   edx, dword ptr [eax+03Ch]     ; 8B 50 3C     原指令 1 重放
    mov   ecx, esi                      ; 8B CE        原指令 2 重放
    jmp   dword ptr g_retGodMode
GodModeCaveStub ENDP

; ===========================================================================
; EnemyCantBuildCaveStub <- base+0x30F530  AOB 0350043BD7 (5B)
;   敌方（[eax+0xC] != 本地玩家ID）资金压到 5000（flags+15==0）或 1（==1）
; ===========================================================================
EnemyCantBuildCaveStub PROC
    pushad
    pushfd
    mov   ecx, g_localPlayerId
    test  ecx, ecx
    jz    eb_skip
    cmp   dword ptr [eax+0Ch], ecx
    je    eb_skip                           ; 己方 -> 不动
    cmp   byte ptr [g_flags+15h], 0
    je    eb_5000
    cmp   byte ptr [g_flags+15h], 1
    jne   eb_skip
    mov   dword ptr [eax+04h], 1
    jmp   eb_skip
eb_5000:
    mov   dword ptr [eax+04h], 00001388h    ; 5000
eb_skip:
    popfd
    popad
    add   edx, dword ptr [eax+04h]      ; 03 50 04     原指令 1 重放
    cmp   edx, edi                      ; 3B D7        原指令 2 重放
    jmp   dword ptr g_retEnemyCantBuild
EnemyCantBuildCaveStub ENDP

; ===========================================================================
; SuperPowerCaveStub <- base+0x2EBB69  AOB 8B9818040000 (6B)
;   flags+0D: 己方超武就绪 ([esi+0x20]=1)
;   flags+0E: 敌方超武加冷却 ([esi+0x20]+=1)
;   比对 [eax+0x418] 与本地玩家对象指针
; ===========================================================================
SuperPowerCaveStub PROC
    pushad
    pushfd
    mov   ecx, g_localPlayerObj
    test  ecx, ecx
    jz    sp_skip
    cmp   esi, 00010000h                ; 护栏：esi 不是原指令提供的，可能是垃圾值
    jb    sp_skip
    cmp   byte ptr [g_flags+0Dh], 1
    jne   sp_chk_e
    cmp   dword ptr [eax+00000418h], ecx
    jne   sp_chk_e
    mov   dword ptr [esi+20h], 1
sp_chk_e:
    cmp   byte ptr [g_flags+0Eh], 1
    jne   sp_skip
    cmp   dword ptr [eax+00000418h], ecx
    je    sp_skip
    add   dword ptr [esi+20h], 1
sp_skip:
    popfd
    popad
    mov   ebx, dword ptr [eax+00000418h] ; 8B 98 18 04 00 00  原指令重放
    jmp   dword ptr g_retSuperPower
SuperPowerCaveStub ENDP

; ===========================================================================
; DisableAllSPCaveStub <- base+0x43ECA6  AOB 8B51503B500C (6B)
;   flags+0E: [eax+0xC] += 1（禁用超武：加冷却）
; ===========================================================================
DisableAllSPCaveStub PROC
    pushad
    pushfd
    cmp   byte ptr [g_flags+0Eh], 1
    jne   ds_skip
    add   dword ptr [eax+0Ch], 1
ds_skip:
    popfd
    popad
    mov   edx, dword ptr [ecx+050h]     ; 8B 51 50     原指令 1 重放
    cmp   edx, dword ptr [eax+0Ch]      ; 3B 50 0C     原指令 2 重放
    jmp   dword ptr g_retDisableAllSP
DisableAllSPCaveStub ENDP

ProtoAllCaveStub PROC
    pushad
    pushfd
    ; ---- 遥测：本格的 esi / 协议对象 / 游戏原始标志 / 强制后标志 ----
    movzx eax, al                        ; 游戏算出的原始标志
    mov   edx, dword ptr [g_paCount]
    cmp   edx, 63
    ja    pa_norec
    imul  ecx, edx, 16
    lea   ebx, dword ptr [g_paBuf]
    add   ecx, ebx
    mov   dword ptr [ecx], esi
    mov   ebx, dword ptr [esi+14h]
    mov   dword ptr [ecx+4], ebx
    mov   dword ptr [ecx+8], eax
    mov   ebx, eax
    cmp   byte ptr [g_flags+18h], 0
    je    pa_rec_done
    xor   ebx, ebx
pa_rec_done:
    mov   dword ptr [ecx+0Ch], ebx
    inc   edx
    mov   dword ptr [g_paCount], edx
pa_norec:
    popfd
    popad
    cmp   byte ptr [g_flags+18h], 0
    je    pa_replay
    xor   al, al                        ; 强制 al=0：未锁定 / 可购买
pa_replay:
    mov   byte ptr [esi+18h], al        ; 88 46 18  原指令重放
    mov   edx, dword ptr [ecx]          ; 8B 11     原指令重放
    jmp   dword ptr g_retProtoAll
ProtoAllCaveStub ENDP

; ---------------------------------------------------------------------------
; ProtoClickCaveStub <- base+0x69AB03  AOB 8B461485C07439 (7B, 实际覆盖 5B)
;   协议格「点击处理」入口。此处 esi = 该格控件（0x69AB01 已 mov esi,ecx）。
;   原指令: mov eax,[esi+14h] / test eax,eax
;   记录: esi / 父面板 / 协议对象 / 已拥有标志 / 父面板+0x74..+0x78 的 id 列表
ProtoClickCaveStub PROC
    pushad
    pushfd
    mov   edx, dword ptr [g_pcCount]
    cmp   edx, 63
    ja    pc_norec
    imul  eax, edx, 24                   ; 6 个 DWORD / 条
    lea   ecx, dword ptr [g_pcBuf]
    add   eax, ecx
    mov   dword ptr [eax], esi
    mov   ecx, dword ptr [esi+4]         ; 父面板
    mov   dword ptr [eax+4], ecx
    test  ecx, ecx
    je    pc_nopar
    mov   ebx, dword ptr [ecx+74h]
    mov   dword ptr [eax+16], ebx
    mov   ebx, dword ptr [ecx+78h]
    mov   dword ptr [eax+20], ebx
pc_nopar:
    mov   ecx, dword ptr [esi+14h]
    mov   dword ptr [eax+8], ecx
    movzx ecx, byte ptr [esi+18h]
    mov   dword ptr [eax+12], ecx
    inc   edx
    mov   dword ptr [g_pcCount], edx
pc_norec:
    popfd
    popad
    mov   eax, dword ptr [esi+14h]       ; 8B 46 14  原指令重放
    test  eax, eax                       ; 85 C0     原指令重放
    jmp   dword ptr g_retProtoClick
ProtoClickCaveStub ENDP
; ===========================================================================
; LockProbeCaveStub <- base+0xA7E9AB  AOB E810B50500（5 字节整指令，exe 内唯一）
;   UI「玩家技能按钮」画锁定态时调用的 call 0xAD9EC0。
;   前文 0xA7E9A9: lea ecx,[esi+8]  ⇒ ecx = 按钮体+8，esi = 按钮体。
;   本 stub 只做记录，不改任何行为：pushad/popfd 保现场 → 记录 → 恢复 →
;   重放原 call（eax/al 即引擎算出的锁定标志）→ 跳回 A7E9B0。
LockProbeCaveStub PROC
    pushad
    pushfd
    mov   edi, ecx                        ; this（= 按钮体 + 8）
    mov   edx, dword ptr [g_lpCount]
    cmp   edx, 63
    ja    lp_norec
    imul  eax, edx, 12                    ; 3 个 DWORD / 条
    lea   ecx, dword ptr [g_lpBuf]
    add   eax, ecx
    mov   dword ptr [eax], edi            ; this
    mov   dword ptr [eax+4], esi          ; 子对象（按钮体）
    mov   ecx, dword ptr [edi+4]          ; [this+4] = 锁定标志所在容器
    mov   dword ptr [eax+8], ecx
    inc   edx
    mov   dword ptr [g_lpCount], edx
lp_norec:
    popfd
    popad
    mov   eax, dword ptr [g_lockProbeCall]; 原 call 目标（绝对地址，按构建取）
    call  eax
    jmp   dword ptr [g_retLockProbe]
LockProbeCaveStub ENDP

; ===========================================================================
; BuildLockCaveStub <- base+0x6B7440
;   AOB 565 78BF18B0DB82FCE00 (10 字节；0x6B5F31 / 0x6B73E0 / 0x6B7440 三处同构，
;   本 hook 只装 0x6B7440 —— 它由 0x6C61DA call 与 0x6C6310 jmp 进入)
;   0x6B7440 = 建造/技能按钮「禁用态」的绘制入口：进去就 push 0x207（禁用原因 CSF 串）
;   并读 [esi+0x10]（Thing）与 [esi+0x18]（Template）。只有被引擎判为不可用的按钮才会来。
;   本站点即 0x6C6301 分支的落点：
;       0x6C62E3  call 0x445250 (Buildability)   al==0 → 正常返回
;       0x6C62FD  call [0xCDBBCC]->vt+0x44       eax!=0 → 正常返回
;       0x6C6310  jmp 0x6B7440                   ← 只剩这一条路 = 真·禁用
;   本 stub 按 Template 去重记录 {tpl, thing, hits}，不改变任何行为。
; ===========================================================================
BuildLockCaveStub PROC
    pushad
    pushfd
    cmp   ecx, 1000000h                    ; 指针基本校验
    jb    bl_done
    mov   edi, dword ptr [ecx+18h]         ; Template
    mov   edx, dword ptr [g_blCount]
    lea   ebx, dword ptr [g_blBuf]
    xor   esi, esi
bl_scan:
    cmp   esi, edx
    jae   bl_new
    cmp   dword ptr [ebx], edi
    je    bl_found
    add   ebx, 12
    inc   esi
    jmp   bl_scan
bl_found:
    inc   dword ptr [ebx+8]
    jmp   bl_done
bl_new:
    cmp   edx, 127
    jae   bl_done
    mov   ebx, edx
    imul  ebx, ebx, 12
    lea   eax, dword ptr [g_blBuf]
    add   ebx, eax
    mov   dword ptr [ebx+0], edi           ; Template
    mov   eax, dword ptr [ecx+10h]         ; Thing
    mov   dword ptr [ebx+4], eax
    mov   dword ptr [ebx+8], 1
    inc   edx
    mov   dword ptr [g_blCount], edx
bl_done:
    popfd
    popad
    push  esi                              ; 56      原指令重放
    push  edi                              ; 57
    mov   esi, ecx                         ; 8B F1
    jmp   dword ptr [g_retBuildLock]       ; = 6B7444
BuildLockCaveStub ENDP

; ===========================================================================
; BuildTipCaveStub <- base+0x6C5D90
;   AOB 83EC08568BF18B0D54B7 (10 字节)
;   0x6C5D90 是「禁用原因 tooltip」的构造器：__thiscall，ecx = tooltip 目标对象，
;   栈上两个参数（ret 8）。函数体内硬编码 push 0xC814EC = GUI:BuildabilityDisabled，
;   所以**只有被引擎判为禁用的东西才会进来**——包括建造面板上那些黑锁占位格。
;   记录 { ecx, arg0([esp+4]), arg1([esp+8]), hits }，按 ecx 去重，不改行为。
;   pushad(32)+pushfd(4)=0x24 ⇒ 原始 [esp+4] 现在位于 [esp+0x28]。
; ===========================================================================
BuildTipCaveStub PROC
    pushad
    pushfd
    mov   edi, ecx                          ; tooltip 目标对象
    mov   esi, dword ptr [esp+28h]          ; arg0
    mov   edx, dword ptr [esp+2Ch]          ; arg1
    mov   ebx, dword ptr [g_tipCount]
    lea   ebp, dword ptr [g_tipBuf]
    xor   ecx, ecx
bt_scan:
    cmp   ecx, ebx
    jae   bt_new
    mov   eax, ecx
    shl   eax, 4
    add   eax, ebp
    cmp   dword ptr [eax], edi
    je    bt_found
    inc   ecx
    jmp   bt_scan
bt_found:
    inc   dword ptr [eax+12]
    jmp   bt_done
bt_new:
    cmp   ebx, 127
    jae   bt_done
    mov   eax, ebx
    shl   eax, 4
    add   eax, ebp
    mov   dword ptr [eax+0], edi            ; ecx
    mov   dword ptr [eax+4], esi            ; arg0
    mov   dword ptr [eax+8], edx            ; arg1
    mov   dword ptr [eax+12], 1
    inc   ebx
    mov   dword ptr [g_tipCount], ebx
bt_done:
    popfd
    popad
    sub   esp, 8                            ; 83 EC 08   原指令重放
    push  esi                               ; 56
    mov   esi, ecx                          ; 8B F1
    jmp   dword ptr [g_retBuildTip]         ; = 6C5D96
BuildTipCaveStub ENDP

; ===========================================================================
; ScienceCaveStub <- base+0x4451F0
;   AOB 51568B74240C85F6578BF9 (11 字节；0x4451F0 / 0x68B140 两处同构，只装前者)
;   bool Science(Player* ecx, Template* [esp+8])  __thiscall, ret 4
;   返回 false 是「建造前置科技不满足」的唯一来源：
;       0x4451F0 内 if (0x43EA70(p,t)) → ret 0   ← 阵营/关系不匹配，战役锁走这条
;   而 0x445250(Buildability) 的判定是：Science 假 → 假；Science 真 → 看 status
;       （status 0/1 → true，2 → false，3 → 看 [Player+0x2C]）
;   所以本站点恒真即可让被剧情锁住的建筑重新可建（等价于旁路
;   0x43EA70 的阵营判定）。受 g_flags[1Ah]（FID_SCIALL）控制，默认关。
;   2026-10-22 补：0x445250 正是「建造页按钮是否画成灰」的唯一门（按钮 vtable slot13
;   → 0xABB6F0 → 0xABB812 调 0x445250），所以本开关对灰按钮同样有效 ——
;   但它是全局的（所有建筑都忽略前置），精准场景改用 BtnEnableCaveStub（g_flags[1Eh]）。
;   ⚠ 必须限定「只对本机玩家放行」：全局恒真会让 AI 也走未解锁的建筑分支，
;      2026-10-22 10:30 实测直接崩游戏（c0000005）。
; ===========================================================================
ScienceCaveStub PROC
    lea   edx, dword ptr [g_flags]
    cmp   byte ptr [edx+1Ah], 0
    je    sci_orig
    mov   edx, dword ptr [g_localPlayerObj]
    test  edx, edx
    jz    sci_orig
    cmp   ecx, edx                         ; ecx = Player，只放行本机玩家
    jne   sci_orig
    mov   al, 1                            ; 恒真：忽略科技前置
    ret   4                                ; 本函数只有 1 个栈参数
sci_orig:
    push  ecx                              ; 51          原指令重放
    push  esi                              ; 56
    mov   esi, dword ptr [esp+0Ch]         ; 8B 74 24 0C
    jmp   dword ptr [g_retScience]         ; = 4451F6
ScienceCaveStub ENDP

; ===========================================================================
; ClanGateCaveStub <- base+0x43EA70
;   AOB 8B 44 24 04 8B 90 C0 00 00 00 (10 字节；exe 内 2 处命中，本表按固定 RVA
;   安装并原地校验，只认 0x43EA70)
;   0x43EA70 = 「模板是否在玩家可用对象集合里」的判定：
;     __thiscall(ecx = Player*, [esp+4] = Template*) -> al, ret 4
;     返回 1 = 命中 Player+0x200 链表（= 该模板被锁 / 不可用，UI 画黑锁）
;     返回 0 = 不在链表里 = 可用
;     （2026-10-22 反汇编复核：命中 → eax≠哨兵 → setne dl = 1；未命中 → 0。
;       链表是「禁用集合」，不是白名单 —— 早先注释写反了，实测以 0=可用 为准。）
;   三个 UI 调用点 (0x6B7089 / 0x6BB228 / 0x6BB4E2) 直接调它、绕过 0x4451F0，
;   所以 Science 桩 (0x4451F0) 对它无效 —— 这才是建造页黑锁的门。
;   原指令边界：+0 = mov eax,[esp+4] (4B) / +4 = mov edx,[eax+0C0h] (6B)
;               → 边界在 +10，重放这两条后跳回 0x43EA7A
;   开 g_flags[1Bh] (FID_CLANGATE) 时只对本机玩家返回 0，AI / 其他玩家不受影响。
;
;   附带遥测：每次进入都记下 Template 指针（去重 + 计数）到 g_clanBuf。
;   战场外的用处 —— UI 每画一个建造按钮就会问一次，所以这份表就是
;   「建造页实际问过哪些模板」的快照：战役 / 扩展模式各跑一遍即可对比列表差异。
;   pushad(32) + pushfd(4) ⇒ 原 [esp+4] 在记录段里是 [esp+40]。
; ===========================================================================
ClanGateCaveStub PROC
    pushad
    pushfd
    mov   eax, dword ptr [g_localPlayerObj] ; 只记本机玩家的询问（敌方 AI 也会调本函数）
    test  eax, eax
    jz    cg_rec_done
    cmp   ecx, eax
    jne   cg_rec_done
    mov   esi, dword ptr [esp+40]          ; 原 [esp+4] = Template*
    test  esi, esi
    jz    cg_rec_done
    xor   ecx, ecx
    mov   edi, dword ptr [g_clanCount]
    lea   edx, dword ptr [g_clanBuf]
cg_rec_loop:
    cmp   ecx, edi
    jae   cg_rec_new
    cmp   dword ptr [edx], esi
    je    cg_rec_hit
    add   edx, 12
    inc   ecx
    jmp   cg_rec_loop
cg_rec_hit:
    inc   dword ptr [edx+8]
    jmp   cg_rec_done
cg_rec_new:
    cmp   edi, 128
    jae   cg_rec_done
    imul  ecx, edi, 12
    lea   edx, dword ptr [g_clanBuf]
    add   edx, ecx
    mov   dword ptr [edx], esi
    mov   dword ptr [edx+4], 0
    mov   dword ptr [edx+8], 1
    inc   edi
    mov   dword ptr [g_clanCount], edi
cg_rec_done:
    popfd
    popad
    lea   edx, dword ptr [g_flags]
    cmp   byte ptr [edx+1Bh], 0
    je    cg_orig
    mov   edx, dword ptr [g_localPlayerObj]
    test  edx, edx
    jz    cg_orig
    cmp   ecx, edx                         ; ecx = Player，只放行本机玩家
    jne   cg_orig
    xor   al, al                           ; 0 = 该模板对本机玩家可用
    ret   4                                ; 本函数 ret 4（1 个栈参数）
cg_orig:
    mov   eax, dword ptr [esp+4]           ; 8B 44 24 04       原指令重放
    mov   edx, dword ptr [eax+0C0h]        ; 8B 90 C0 00 00 00 原指令重放
    jmp   dword ptr [g_retClanGate]        ; = 43EA7A
ClanGateCaveStub ENDP

; ===========================================================================
; BuildBtnProbeCaveStub <- base+0xAB7070  AOB 83 EC 0C 53 56 8B F1 8B 4E 10 (10B)
;   探针：只记录 ecx（建造页按钮对象自身），不改变任何行为。
;   用途：拿到真实按钮对象地址后，反查「持有它们的数组」→ 定位建造页列表本体。
;   站点原指令：sub esp,0Ch(3B) / push ebx(1B) / push esi(1B) = 5B，边界 +5 → 0xAB7075
;   记录受 g_flags[1Ch] 控制（默认关）；去重上限 64 个地址。
BuildBtnProbeCaveStub PROC
    pushad
    pushfd
    lea   edx, dword ptr [g_flags]
    cmp   byte ptr [edx+1Ch], 0
    je    bp_done
    mov   esi, dword ptr [g_bpCount]
    cmp   esi, 64
    jae   bp_done
    xor   edi, edi
    lea   edx, dword ptr [g_bpBuf]
bp_scan:
    cmp   edi, esi
    jae   bp_new
    cmp   dword ptr [edx+edi*4], ecx
    je    bp_done
    inc   edi
    jmp   bp_scan
bp_new:
    mov   dword ptr [edx+esi*4], ecx
    inc   esi
    mov   dword ptr [g_bpCount], esi
bp_done:
    popfd
    popad
    sub   esp, 0Ch                         ; 83 EC 0C  原指令重放
    push  ebx                              ; 53        原指令重放
    push  esi                              ; 56        原指令重放
    jmp   dword ptr [g_retBtnProbe]        ; = 0xAB7075
BuildBtnProbeCaveStub ENDP

; ===========================================================================
; CommandSetProbeCaveStub <- base+0xACB352  AOB 8B D8 85 DB 89 5C 24 18 (8B)
;   探针：记录 ebx（= 从 [0xCD8D00] store 查出的 CommandSet 对象），不改变行为。
;   调用者 0xACB330 是「建造页按钮列表构建器」：vt+0 = 按钮数、vt+0xC(i) = 第 i 个按钮定义。
;   站点原指令：mov ebx,eax(2B) / test ebx,ebx(2B) / mov [esp+18h],ebx(4B) = 8B，边界 +8。
;   注意：桩内 esp 与站点处一致（pushad/popad 净零），所以 [esp+18h] 可原样重放。
;   g_flags[0x1D]（FID_BUILDEXT）打开时，先调 Ra3BuildExtFix(eax) 把战役版 cs 换成
;   扩展完整版 —— 此刻按钮还没 new，换完同一轮构建即按新 count 走。
CommandSetProbeCaveStub PROC
    pushad
    pushfd
    test  eax, eax
    jz    cs_done
    ; --- 1) 探针必须在 Fix 之前：Fix 返回后 eax 会变成新 inner，不再是 cs
    lea   edx, dword ptr [g_flags]
    cmp   byte ptr [edx+1Ch], 0
    je    cs_fix
    mov   esi, dword ptr [g_csCount]
    cmp   esi, 16
    jae   cs_fix
    xor   edi, edi
    lea   edx, dword ptr [g_csBuf]
cs_scan:
    cmp   edi, esi
    jae   cs_new
    cmp   dword ptr [edx+edi*4], eax
    je    cs_fix
    inc   edi
    jmp   cs_scan
cs_new:
    mov   dword ptr [edx+esi*4], eax
    inc   esi
    mov   dword ptr [g_csCount], esi
cs_fix:
    ; --- 2) 补全扩展建筑（eax = 构建器查表拿到的 cs）
    lea   edx, dword ptr [g_flags]
    cmp   byte ptr [edx+1Dh], 0
    je    cs_done
    push  eax                              ; cs（__cdecl，调用方清栈）
    call  Ra3BuildExtFix
    add   esp, 4
cs_done:
    popfd
    popad
    mov   ebx, eax                         ; 8B D8      原指令重放
    test  ebx, ebx                         ; 85 DB      原指令重放
    mov   dword ptr [esp+18h], ebx         ; 89 5C 24 18 原指令重放
    jmp   dword ptr [g_retCsProbe]         ; = 0xACB35A
CommandSetProbeCaveStub ENDP

; ===========================================================================
; BtnEnableCaveStub <- base+0xABB6F0  AOB 83EC08 56 8BF1 E865FCFFFF (11B，全 exe 唯一)
;   建造页按钮「可用性刷新」入口（按钮对象 vtable 0x00C88518 的 slot13，0xAB7070 尾部调它）。
;   原函数末尾决定 enable/disable：走 0xABB812 时调 0x845250(Buildability)，
;     返回 0 → 0xAB7040(setDisabled) → 绘制成灰；返回 1 → 0xAB7000(setEnabled)。
;   为什么 0x445250 会返回 0：它先调 0x8451F0(前置科技)，再调 0x7DF770(tmpl) 取状态；
;   扩展建筑在战役局的前置建筑 / 科技不满足，故恒灰（引擎「落点可建」判定却是通过的）。
;   桩：开关 g_flags[1Eh] 打开时，若按钮的 Template 是扩展建筑，直接 setEnabled(true) 并返回。
;   重放 6 字节 = sub esp,8 (3B) / push esi (1B) / mov esi,ecx (2B)，边界在 +6。
;   ⚠ 本桩无 pushad/pushfd，直接改 eax/ecx（两者都是调用者不保留的），ebx/esi/edi 不碰。

; ===========================================================================
; BuildableCaveStub —— 建造页「灰/亮」的唯一门：Buildability(Player*, Template*)
;   零售 0x845250 / Steam 0xA832C0（RVA 0x445250 / 0x4832C0），__thiscall，ret 4
;   原逻辑：Template==0 -> 0；Science(Player,Template)==0 -> 0；否则看 0x7DF770 的结果
; 只对「扩展建筑模板」提前返回 1（可建），其余走原逻辑 ——
; 这样游戏自己会调 setEnabled，我们完全不碰按钮对象。
; ⛔ 别再改回「手动调 setEnabled」：0x1E 旧实现在 Steam 上 17:25 实测把栈弹坏
;    （出错地址落在栈上、EBP=0x206），原因是 setEnabled 内部还有两层虚调用。
; ===========================================================================
; SetDisCaveStub —— 建造按钮 setDisabled 入口（零售 0xAB7040 / Steam RVA 0x64BCC0）
;   两版逐字节相同：80 79 3D 00 (cmp byte [ecx+0x3D],0) / 56 (push esi) / 8D 71 08 (lea esi,[ecx+8])
;   语义：把按钮 enabled 位（+0x3D）清 0 并通知 UI；setEnabled 是同址 +0x40 的对称函数。
; 做法：命中扩展建筑模板时直接 **jmp setEnabled**（不是 call）——
;   原调用者是 `call setDisabled`，栈上有返回地址；jmp 后 setEnabled 的 ret 正好回到同一处，
;   栈完全平衡，上下文全是游戏自己的。
; ⚠ 别再在 0x650320 入口手动 call setEnabled：那里栈上是 sub esp,8 + push esi，
;   17:25 实测会把栈弹坏（EIP 落到栈上、EBP=0x206）。
SetDisCaveStub PROC
    pushad
    pushfd
    mov   eax, dword ptr [esp+36]          ; 原返回地址（pushad 32B + pushfd 4B）
    mov   dword ptr [g_setDisRet], eax
    mov   eax, dword ptr [esp+40]          ; 原 [esp+4]（第一个栈参数）
    mov   dword ptr [g_setDisArg0], eax
    lea   edx, dword ptr [g_flags]
    cmp   byte ptr [edx+1Eh], 0
    je    sd_orig
    mov   eax, dword ptr [ecx+18h]         ; 按钮的 Template*
    test  eax, eax
    jz    sd_orig
    mov   eax, dword ptr [eax+8]           ; Template+0x08 = 名字哈希
    test  eax, eax
    jz    sd_orig
    mov   dword ptr [g_btnLastObj], ecx    ; 只读取证：记下按钮对象（+0x18 模板 / +0x3D 启用位）
    mov   dword ptr [g_btnLastHash], eax   ; 只读取证：记下模板哈希
    push  ecx
    push  eax
    call  Ra3IsExtTpl
    add   esp, 4
    pop   ecx
    test  eax, eax
    jz    sd_orig
    inc   dword ptr [g_btnForceCount]
    ; ⚠ popad 会把 EAX 恢复成入口时的原值。所以这里**不能**先 mov eax,[目标] 再 popad，
    ;    否则 jmp eax 跳的是原 EAX（2026-10-26 18:16 实测 EIP=1 / EAX=1 就是这么崩的）。
    ;    必须先查就绪，再 popad，最后用「内存间接跳」直接取绝对地址。
    cmp   dword ptr [g_btnSetEnabled], 0
    je    sd_orig                           ; 尚未就绪就走原逻辑，绝不 jmp 0
    popfd
    popad
    jmp   dword ptr [g_btnSetEnabled]       ; setEnabled 的 ret 回到原调用者
sd_orig:
    popfd
    popad
    cmp   byte ptr [ecx+3Dh], 0            ; 80 79 3D 00   原指令重放
    push  esi                              ; 56
    jmp   dword ptr [g_retSetDis]          ; = 站 + 5（lea esi,[ecx+8]）
SetDisCaveStub ENDP

BuildableCaveStub PROC
    pushad
    pushfd
    lea   edx, dword ptr [g_flags]
    cmp   byte ptr [edx+1Eh], 0
    je    bl_orig
    mov   eax, dword ptr [esp+28h]         ; 入口 [esp+4] = Template*（36 字节 + 4）
    test  eax, eax
    jz    bl_orig
    mov   eax, dword ptr [eax+8]           ; Template+0x08 = 名字哈希
    test  eax, eax
    jz    bl_orig
    push  eax
    call  Ra3IsExtTpl
    add   esp, 4
    test  eax, eax
    jz    bl_orig
    inc   dword ptr [g_btnForceCount]
    popfd
    popad
    mov   al, 1                            ; 可建 → UI 走亮/可点分支
    ret   4
bl_orig:
    popfd
    popad
    push  esi                              ; 56          原指令重放（8 字节）
    mov   esi, dword ptr [esp+8]           ; 8B 74 24 08
    test  esi, esi                         ; 85 F6
    push  edi                              ; 57
    jmp   dword ptr [g_retBuildability]    ; = 站 + 8（mov edi, ecx）
BuildableCaveStub ENDP

; ===========================================================================
BtnEnableCaveStub PROC
    ; 入口全保护。原版只存 ecx —— 但 Steam 版是重新编译的，setEnabled / C 函数
    ; 会破坏哪些寄存器没有保证；调用方（建造页构建循环）用 edi 存按钮定义指针，
    ; 一旦被改就是 `[edi+4]` 读到垃圾 → 跳到堆上执行（四次崩溃 EDI=0x2B 即此类）。
    pushad
    pushfd
    lea   edx, dword ptr [g_flags]
    cmp   byte ptr [edx+1Eh], 0
    je    be_orig
    mov   eax, dword ptr [ecx+18h]         ; 按钮的 Template*
    test  eax, eax
    jz    be_orig
    mov   eax, dword ptr [eax+8]           ; Template+0x08 = 名字哈希
    test  eax, eax
    jz    be_orig
    push  ecx                              ; cdecl 参数：先压按钮对象（高地址）
    push  eax                              ; 再压哈希（栈顶 = 第一个参数）
    call  Ra3IsExtTpl                      ; 四阵营扩展建筑统一判定
    add   esp, 4
    pop   ecx                              ; ★ 必须弹回：C 调用把 ecx 当 caller-saved 毁掉了。
                                           ;   漏这一步 → setEnabled 的 this 变成垃圾，
                                           ;   它内部 `mov ecx,[ecx+1C]` 再虚调用 → 跳到堆上执行。
                                           ;   （2026-10-26 实测：漏掉时 g_btnLastObj 记到的是
                                           ;     0x2D270F77，即模板哈希的残留值）
    test  eax, eax
    jz    be_orig
    inc   dword ptr [g_btnForceCount]      ; 记账：命中一次（演练也记）
    mov   dword ptr [g_btnLastObj], ecx    ; 记下按钮对象，供只读取证它的 +0x18/+0x1C
    cmp   dword ptr [g_btnForceReal], 0
    je    be_orig                          ; 演练模式：原样执行，不动按钮状态
    mov   eax, dword ptr [g_btnSetEnabled] ; setEnabled(true)，ecx 仍是按钮对象
    call  eax
    popfd
    popad
    mov   al, 1                            ; 跳过原刷新，保留刚设的状态
    ret
be_orig:
    popfd
    popad
    sub   esp, 8                           ; 83 EC 08   原指令重放
    push  esi                              ; 56
    mov   esi, ecx                         ; 8B F1
    jmp   dword ptr [g_retBtnEnable]       ; = 0xABB6F6
BtnEnableCaveStub ENDP

; ===========================================================================
; BuildExtCsStub <- 零售 base+0x6CB352 / Steam base+0x662422  AOB 8BD885DB895C2418 (8B)
;   建造页构建器拿到 CommandSet 句柄后（此时 eax = cs），立刻换成「同族扩展完整版」的句柄。
;   原指令 8 字节：mov ebx,eax / test ebx,ebx / mov [esp+18h],ebx
;
;   ★ 为什么不用「改 cs+4」那条路：cs 对象只有 0x10 字节（+0 外层vtable / +4 引用计数 /
;     +8 内层vtable / +0xC inner），构造时还会把 handle 注册进 manager+0x40 的 map
;     （key = *(inner+0)+4）并调整引用计数。直接改 inner 指针会绕过这两条约束，
;     点开建造列表时在容器路径上炸（StackHash，栈毁，dump 无法反查）。
;     本 stub 只换「调用方拿到的句柄」，不动任何对象 —— 引用计数与 map key 全部保持自洽。
;
;   pushad 布局：+0 EDI / +4 ESI / +8 EBP / +12 原ESP / +16 EBX / +20 EDX / +24 ECX / +28 EAX
BuildExtCsStub PROC
    pushad
    push  eax
    call  dword ptr [g_extReplaceFn]
    add   esp, 4
    mov   dword ptr [esp+28], eax        ; 覆盖 pushad 保存的 eax（EAX 在最高位）
    popad
    mov   ebx, eax                       ; ↓ 原指令重放（8 字节）
    test  ebx, ebx
    mov   dword ptr [esp+18h], ebx
    jmp   dword ptr [g_retBuildExtCs]    ; = site + 8
BuildExtCsStub ENDP

; ===========================================================================
; CsKeyCaveStub <- 零售 base+0x6CB344 / Steam base+0x662414
;   建造页构建器「算出 cs key、还没查表」那一刻，把 key 就地换成同名 Enhanced 版。
;   原指令 9 字节：
;     8B 08              mov  ecx,[eax]        ; eax = &key（0x2FCCE0 的输出槽）
;     51                 push ecx
;     8B 0D <abs>        mov  ecx,[<CommandSet 注册表槽>]
;   （紧随其后 site+9 是 call <store.find>，不重放、直接跳过去）
;
;   ★ 与 BuildExtCs（换句柄）的区别：这条在**查表前**改 key，游戏拿到的就是 Enhanced 版
;     句柄本身，不涉及任何句柄替换；也不碰 cs/inner/引用计数。覆盖 24 组配对（含 8 处
;     生产建筑），不需要靠内容猜同族。
;
;   ★ 重放里的绝对地址两构建不同（零售 0x00CD8D00 / Steam 0x00CDDEA0），所以不写死：
;     先取 g_csStoreSlot 里的槽地址，再按原语义 mov ecx,[槽]。
;
;   pushad 布局：+0 EDI / +4 ESI / +8 EBP / +12 原ESP / +16 EBX / +20 EDX / +24 ECX / +28 EAX
;   出口只有一处；pushad 只 pop 一次；净栈位移 = 0（引擎的 call find 是 ret 4，自清 push 的 key）
CsKeyCaveStub PROC
    pushad
    mov   dword ptr [g_csKeyPtr], eax    ; 先存 &key：pushad 保存的 EAX 在 +0x1C，不靠栈偏移取
    mov   ecx, dword ptr [esi+10h]       ; ecx = 建造者/生产者对象（原 0x6CB337 读的那个）
    push  dword ptr [eax]                ; 参数2：原 key
    push  ecx                            ; 参数1：生产者
    call  dword ptr [g_mapCsKeyFn]       ; uint32_t MapCsKey(producer, key)
    add   esp, 8
    test  eax, eax
    jz    short cs_key_replay
    mov   edx, dword ptr [g_csKeyPtr]    ; &key
    mov   dword ptr [edx], eax           ; 就地换掉 key
cs_key_replay:
    popad
    mov   ecx, dword ptr [eax]           ; ↓ 原指令重放（9 字节，到 call 之前）
    push  ecx
    mov   edx, dword ptr [g_csStoreSlot]
    mov   ecx, dword ptr [edx]
    jmp   dword ptr [g_retCsKey]         ; = site + 9（下一条即 call <store.find>）
CsKeyCaveStub ENDP


; ===========================================================================
; CanBuildProbeCaveStub <- Steam base+0x650470（零售未定位，站点=0）
;   建造按钮刷新里「第二道门」之后：mov edi,eax / test edi,edi / je 0x65049F
;   原语义：edi(门返回值)==0 → 跳 站+0x2F（不置灰）；否则顺序执行到 setDisabled。
;
;   ★ 为什么重放要自己模拟这条 je：je 是 rel8，搬进 cave 后相对地址会算到 cave 里。
;     所以只重放 mov/test，然后按原语义手写两条跳转（两条目标都在补丁区之外）。
;   ebp = Template 参数、esi = 门看到的对象（+0x470/+0x471 两个标志字节、决定它的分支）。
CanBuildProbeCaveStub PROC
    pushad
    pushfd
    cmp   dword ptr [g_cbProbeOn], 0       ; 诊断开关（默认 1；出问题可关掉）
    je    cb_skip
    test  ebp, ebp
    jz    cb_skip
    ; pushad(32)+pushfd(4) 后的栈布局：+00 EFLAGS +04 EDI +08 ESI +0C EBP
    ;   +10 旧ESP +14 EBX +18 EDX +1C ECX +20 EAX +24 返回地址 +28 arg0 +2C arg1
    mov   eax, dword ptr [esp+20h]         ; 门的返回值（原 EAX）
    mov   ecx, dword ptr [esp+28h]         ; arg0 = 判定主体（门内部 esi）
    mov   edx, dword ptr [esp+2Ch]         ; arg1
    push  edx
    push  ecx
    push  eax
    push  ebp                              ; (template, ret, arg0, arg1)
    call  dword ptr [g_canProbeFn]
    add   esp, 16
cb_skip:
    popfd
    popad
    mov   edi, eax                         ; 原指令重放 1（8B F8）
    test  edi, edi                         ; 原指令重放 2（85 FF）
    jnz   cb_nonzero
    mov   edx, dword ptr [g_retCanProbe]   ; 门说 0 → 原 je 目标
    add   edx, 29h                         ; rel8 就藏在原 AOB 里（74 29），落点 = 站+6+0x29 = 0x65049F
                                           ; ⚠ 必须 29h。写成 2Fh（多 6）会跳过 pop ebp/edi/ebx/
                                           ;   xor al,al/pop esi 五条，直接从 add esp,8; ret 返回 →
                                           ;   栈上还压着 4 个寄存器，EIP 取自 ESI 的残留值 →
                                           ;   DEP 拦下，WER 报 BEX + StackHash（崩点无法反查）。
    jmp   edx
cb_nonzero:
    jmp   dword ptr [g_retCanProbe]        ; = 站 + 6（0x650476 → setDisabled）
CanBuildProbeCaveStub ENDP

END

; ===========================================================================
; ProtoAllCaveStub <- base+0x69A9B3  AOB 8846188B118B (6B)
;   直接把控件状态字节 [esi+0x18] 写成 0 = 未锁定。
;   原函数 ret 4（一个栈参数），ecx = SCPoint。
