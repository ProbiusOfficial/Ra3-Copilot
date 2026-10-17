#!/usr/bin/env python3
"""scan_hooks.py - 用真实游戏 exe 校验全部 hook AOB（移植前的前提检查）
用法: python scan_hooks.py [exe_path]
输出: 每个 hook 在文件中的位置（RVA）、是否与预期 RVA 吻合、匹配次数
"""
import os, sys

# exe 路径：命令行参数 > RA3_GAME_EXE 环境变量 > 兜底默认值
EXE = sys.argv[1] if len(sys.argv) > 1 else \
    os.environ.get("RA3_GAME_EXE") or r'D:\GAME\Game_RTS\Red Alert 3\Data\ra3_1.12.game'

# (名称, 预期RVA或None, AOB hex)  —— RVA 相对模块基址
HOOKS = [
    # ---- 已有（工作正常）----
    ('PlayerID',      0x0FF95B, '8b50288b4220'),
    ('Money',         0x6CFDFE, '0378048b11'),
    # ---- 待移植：玩家属性 ----
    ('Power',         0x6CFD0D, '8b40048b8eb0030000'),
    ('SCPoint',       0x6CFE6C, '8b78348b4e3c'),
    ('HaveAllSC',     0x6CFEB5, 'f30f10472c'),
    # ---- 待移植：建造 ----
    ('FastBuild1',    0x30F42E, 'f30f2c461c'),
    ('FastBuild2',    0x30F38C, 'd9461c89465c'),
    ('FastBuild3',    0x2F6CFF, 'd986bc010000'),
    ('EnemyCantBuild',0x30F530, '0350043bd7'),
    # ---- 待移植：战斗/地图 ----
    ('SuperPower',    0x2EBB69, '8b9818040000'),
    ('SuperPower2',   0x43EC19, '8b70508b01'),
    ('DisableAllSP',  0x43ECA6, '8b51503b500c'),
    ('DisableAllSP2', 0x30A5F6, '8b48503b4e20'),
    ('Map',           0x3C226C, 'f30f118560020000'),
    ('UnitAmmo',      0x128735, '8b0cba85c9'),
    ('DangerLevel',   0x438ED0, '8b8178120000'),
    ('OreMine',       0x2D4933, '8b40082b4114'),
    ('GodMode',       0x12EEDF, '8b503c8bce'),
    ('KillIt',        0x3651AE, 'f30f114604'),
    ('Zoom',          0x1EC7CD, 'f30f114644'),
]


def main():
    if not os.path.exists(EXE):
        print('[x] exe not found:', EXE)
        return 1
    data = open(EXE, 'rb').read()
    print(f'[*] exe={EXE} size={len(data)}')
    ok = bad = 0
    for name, rva, aob in HOOKS:
        pat = bytes.fromhex(aob)
        hits = []
        i = data.find(pat)
        while i != -1:
            hits.append(i)
            i = data.find(pat, i + 1)
        # 校验预期 RVA
        mark = ' '
        if rva is not None:
            if rva in hits:
                mark = 'V'          # 与预期 RVA 吻合
                ok += 1
            else:
                mark = '!'
                bad += 1
        rvas = [h for h in hits]
        print(f'[{mark}] {name:14} exp_rva=0x{rva:06X} matches={len(hits)} '
              f'at={["0x%X" % h for h in rvas[:5]]}')
    print(f'--- verified={ok} mismatch={bad} ---')
    return 0


if __name__ == '__main__':
    sys.exit(main())
