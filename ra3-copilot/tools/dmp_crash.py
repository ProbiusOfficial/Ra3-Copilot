#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dmp_crash.py <minidump> [--va 0xADDR --n N] [--stack N] [--all]
解析 minidump：异常记录 + 上下文寄存器 + EIP 处实际内存字节 + 栈上行。
用于"运行时代码被打补丁"的场景 —— 一切以转储内存为准。"""
import struct, sys

def u32(d, o): return struct.unpack_from("<I", d, o)[0]
def u64(d, o): return struct.unpack_from("<Q", d, o)[0]

FLD = ["Edi","Esi","Ebx","Edx","Ecx","Eax","Ebp","Eip","Esp"]
# i386 CONTEXT（WinNT.h）：6 个 Dr + FloatSave(0x70) → Edi@0x9C
OFF = {"Edi":0x9C,"Esi":0xA0,"Ebx":0xA4,"Edx":0xA8,"Ecx":0xAC,"Eax":0xB0,"Ebp":0xB4,"Eip":0xB8,"Esp":0xC4}

def parse(path):
    d = open(path, "rb").read()
    sig, ver, nstreams, dirrva = struct.unpack_from("<4sIII", d, 0)
    assert sig == b"MDMP", sig
    streams = {}
    for i in range(nstreams):
        t, sz, rva = struct.unpack_from("<III", d, dirrva + i * 12)
        streams.setdefault(t, []).append((sz, rva))
    return d, streams

def mem_regions(d, streams):
    regs = []
    for sz, rva in streams.get(5, []):
        n = u32(d, rva)
        for i in range(n):
            b = rva + 4 + i * 16
            start, msize, moff = struct.unpack_from("<QII", d, b)
            regs.append((start, msize, moff))
    for sz, rva in streams.get(9, []):
        n = u64(d, rva); base = u64(d, rva + 8)
        data0 = rva + 16 + n * 16
        cur = base
        for i in range(n):
            start, msize = struct.unpack_from("<QQ", d, rva + 16 + i * 16)
            regs.append((cur, msize, data0 + (cur - base)))
            cur += msize
    regs.sort()
    return regs

def read_mem(d, regs, va, n):
    out = bytearray(); cur = va
    while len(out) < n:
        hit = None
        for start, msize, moff in regs:
            if start <= cur < start + msize:
                hit = (start, msize, moff); break
        if not hit:
            out += b"\xCC" * min(n - len(out), 16); cur += min(n - len(out), 16) or 16; continue
        start, msize, moff = hit
        take = min(start + msize - cur, n - len(out))
        out += d[moff + (cur - start): moff + (cur - start) + take]
        cur += take
    return bytes(out)

def disas(d, regs, va, n=0x60, back=0x20, label=None):
    code = read_mem(d, regs, va - back, n)
    try:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        for ins in md.disasm(code, va - back):
            if ins.address > va + (n - back):
                break
            mark = "  <<<" if ins.address == label else ""
            print("  %08X  %-18s %s %s%s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, mark))
    except ImportError:
        print("  bytes:", code.hex())

def main():
    path = sys.argv[1]
    d, streams = parse(path)
    regs = mem_regions(d, streams)
    print("streams:", sorted(streams.keys()), "| mem_regions:", len(regs), "| dump %d bytes" % len(d))

    fault_tid = None
    if 6 in streams:
        sz, rva = streams[6][0]
        fault_tid = u32(d, rva)
        ec = u32(d, rva + 8); eflags = u32(d, rva + 12)
        eaddr = u64(d, rva + 0x18)
        nparam = u32(d, rva + 0x20)
        params = [u64(d, rva + 0x28 + i * 8) for i in range(15)]
        ctxsz, ctxrva = u32(d, rva + 0xA0), u32(d, rva + 0xA4)
        print("EXCEPTION tid=%d code=%08X flags=%08X addr=%08X nparam=%d" % (fault_tid, ec, eflags, eaddr, nparam))
        print("  params:", [hex(p) for p in params[:max(1, nparam)]])
        print("  exc ctx: size=%d rva=%X" % (ctxsz, ctxrva))
        if ctxrva and ctxsz >= 0xC8:
            R = {k: u32(d, ctxrva + OFF[k]) for k in FLD}
            print("  " + " ".join("%s=%08X" % (k, v) for k, v in R.items()))
            show_fault(d, regs, R)

    # 线程表：找同 tid 的栈 + 扫描各线程栈里的代码返回址
    for sz, rva in streams.get(3, []):
        n = u32(d, rva)
        print("\nthreads: %d (fault tid=%s)" % (n, fault_tid))
        for i in range(n):
            b = rva + 4 + i * 48
            th = u32(d, b)
            stacksz, stackrva = u32(d, b + 0x24), u32(d, b + 0x28)
            csz, crva = u32(d, b + 0x2C), u32(d, b + 0x30)
            star = " *FAULT*" if th == fault_tid else ""
            esp = u32(d, crva + OFF["Esp"]) if csz >= 0xC8 else 0
            print("  tid=%d stack(0x%X @rva%X) esp=%08X%s" % (th, stacksz, stackrva, esp, star))

def show_fault(d, regs, R):
    eip, esp = R["Eip"], R["Esp"]
    print("\n-- code @EIP=%08X (dump 内存，可能已被补丁) --" % eip)
    disas(d, regs, eip, 0x50, 0x20, label=eip)
    if esp:
        st = read_mem(d, regs, esp, 0x140)
        print("\n-- stack @ESP=%08X --" % esp)
        for i in range(0, len(st) - 3, 4):
            v = struct.unpack_from("<I", st, i)[0]
            tag = ""
            if 0x401000 <= v < 0x400000 + 0x93B000:
                tag = "  ->game +%X" % (v - 0x400000)
            print("  %08X: %08X%s" % (esp + i, v, tag))

if __name__ == "__main__":
    main()
