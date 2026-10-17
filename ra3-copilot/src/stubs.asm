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
EXTERNDEF g_zoomHits     : DWORD    ; 遥测：Zoom cave 进入次数
EXTERNDEF g_zoomMax      : DWORD    ; 已废用：ZF 机制不写任何上限字段（保留占位，待清理）
EXTERNDEF g_zoomMaxOff   : DWORD    ; 已废用：同上
EXTERNDEF g_zoomCam34    : DWORD    ; 遥测：ZoomPostStub 采样的 [+0x34] 相机系数
EXTERNDEF g_zoomPitch    : DWORD    ; 遥测：ZoomPostStub 采样的 [+0x74] 俯仰角
EXTERNDEF g_zoomObj      : DWORD    ; 遥测：最近一次 this 指针
EXTERNDEF g_zoomVal      : DWORD    ; 遥测：进入时 [esi+0x44]
EXTERNDEF g_zoomPost     : DWORD    ; 遥测：运算后 [esi+0x44]
EXTERNDEF g_zoomState    : BYTE     ; 已废用：旧护栏语义，现无写入方（保留占位，待清理）
EXTERNDEF g_zoomEntry    : DWORD    ; 遥测：缩放函数入口命中次数（含被早退的）

; ---- 各 hook 返回地址（运行时由 C++ 填充 = site + 重放长度）----
EXTERNDEF g_retMoney        : DWORD
EXTERNDEF g_retPlayerId     : DWORD
EXTERNDEF g_retPower        : DWORD
EXTERNDEF g_retSCPoint      : DWORD
EXTERNDEF g_retHaveAllSC    : DWORD
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
    mov   eax, dword ptr [0CDB750h]     ; A1 50 B7 CD 00  原指令重放
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
;   需存在选中单位（[ebp+0x3ac]!=0）；[ecx+0x1c]=0x7FFFFFFF
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
    cmp   byte ptr [g_flags+12h], 0
    je    ua_skip
    cmp   dword ptr [ebp+000003ACh], 0
    je    ua_skip
    cmp   ecx, 00010000h            ; 护栏：不像堆指针就别写（防闪退）
    jb    ua_skip
    mov   dword ptr [ecx+01Ch], 07FFFFFFFh
ua_skip:
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

END
