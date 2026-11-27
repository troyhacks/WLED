#!/usr/bin/env python3
"""Decode an ESP-IDF coredump that was read out of the board's flash.

The IDF writes its coredump to the 'coredump' partition as an ELF32 core
file, behind a 12 byte core_dump_header_t.  It is a *subset* of ELF: the
program headers are PT_NOTE (the registers, per task) and PT_LOAD (the task
control block and the task stack).  There are no DWARF sections, so gdb
needs the application ELF as well; this script reads the application ELF's
symbol table itself and does the symbolising, so it works on a board with
no toolchain installed.

    python tools/coredump_decode.py coredump.bin --elf firmware.elf
    python tools/coredump_decode.py coredump.bin --elf firmware.elf --dump 4ff56140:32
    python tools/coredump_decode.py coredump.bin --elf firmware.elf --stack AudioTask

The core dump header (components/espcoredump/include_core_dump/
esp_core_dump_types.h):

    uint32_t data_len;    // total length of the dump
    uint32_t version;     // core dump format version
    uint32_t chip_rev;    // chip revision

The notes (src/core_dump_elf.c):

    "CORE"              type 1      the register set of one task
    "ESP_CORE_DUMP_INFO" type 8266  the version and the app's SHA256
    "ESP_EXTRA_INFO"    type 677    the fault: cause, address, crashed task

The register set is arch specific: on RISC-V it is a riscv_prstatus
(components/espcoredump/src/port/riscv/core_dump_port.c) with the
registers at offset 72, on Xtensa it is an xtensa_elf_reg_dump_t with the
gdb register set at offset 72 as well.
"""

import argparse
import os
import struct
import sys

# ---------------------------------------------------------------- ELF basics

PT_LOAD = 1
PT_NOTE = 4
SHT_SYMTAB = 2
SHT_NOBITS = 8


class Elf32:
    """Just enough ELF32 to read a symbol table and to map addresses."""

    def __init__(self, data, base=0):
        self.data = data
        self.base = base
        if data[base:base + 4] != b"\x7fELF":
            raise ValueError("not an ELF32 file")
        (self.e_type, self.e_machine, self.e_version, self.e_entry,
         self.e_phoff, self.e_shoff, self.e_flags, self.e_ehsize,
         self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, base + 16)
        self.machine = self.e_machine
        self.sections = []
        self.symbols = []
        self._read_sections()
        self._read_symbols()

    def _read_sections(self):
        raw = []
        for i in range(self.e_shnum):
            off = self.base + self.e_shoff + i * self.e_shentsize
            (name, stype, flags, addr, offset, size, link, info,
             align, entsize) = struct.unpack_from("<IIIIIIIIII", self.data, off)
            raw.append((name, stype, flags, addr, offset, size, link, entsize))
        if not raw:
            return
        strtab_off = raw[self.e_shstrndx][4]
        for name, stype, flags, addr, offset, size, link, entsize in raw:
            s = self.data.index(b"\0", strtab_off + name)
            self.sections.append({
                "name": self.data[strtab_off + name:s].decode("latin-1"),
                "type": stype, "flags": flags, "addr": addr,
                "offset": offset, "size": size, "link": link,
                "entsize": entsize,
            })

    def _read_symbols(self):
        for sec in self.sections:
            if sec["type"] != SHT_SYMTAB:
                continue
            strtab = self.sections[sec["link"]]
            n = sec["size"] // 16
            for i in range(n):
                off = sec["offset"] + i * 16
                st_name, st_value, st_size, st_info, st_other, st_shndx = \
                    struct.unpack_from("<IIIBBH", self.data, off)
                end = self.data.index(b"\0", strtab["offset"] + st_name)
                name = self.data[strtab["offset"] + st_name:end].decode("latin-1")
                if not name or st_value == 0:
                    continue
                self.symbols.append((st_value, st_size, name))
        self.symbols.sort()

    def section_of(self, addr):
        for sec in self.sections:
            if sec["type"] == SHT_NOBITS:
                continue
            if sec["addr"] <= addr < sec["addr"] + sec["size"]:
                return sec
        return None

    def symbol_of(self, addr):
        """The symbol whose address is the highest one below addr."""
        lo, hi = 0, len(self.symbols)
        best = None
        while lo < hi:
            mid = (lo + hi) // 2
            if self.symbols[mid][0] <= addr:
                best = self.symbols[mid]
                lo = mid + 1
            else:
                hi = mid
        return best

    def describe(self, addr):
        sec = self.section_of(addr)
        sym = self.symbol_of(addr)
        text = ""
        if sym:
            text = sym[2]
            if sym[1]:
                text += "+%d" % (addr - sym[0])
        where = sec["name"] if sec else "?"
        return "0x%08x %-40s (%s)" % (addr, text, where)


# ---------------------------------------------------------------- the coredump

CORE_NOTE_TYPE = 1
EXTRA_INFO_TYPE = 677
DUMP_INFO_TYPE = 8266


def load_core(path):
    data = open(path, "rb").read()
    data_len, version, chip_rev = struct.unpack_from("<III", data, 0)
    core = Elf32(data, base=12)
    return data, core, (data_len, version, chip_rev)


def notes_of(data, core):
    """Every note in the file, as (name, type, desc)."""
    out = []
    for ph in range(core.e_phnum):
        off = core.base + core.e_phoff + ph * core.e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, _ = \
            struct.unpack_from("<IIIIIIII", data, off)
        if p_type != PT_NOTE:
            continue
        # the offsets in the program headers are relative to the ELF start,
        # which is not the start of the file: the core dump header is in front
        p = core.base + p_offset
        end = core.base + p_offset + p_filesz
        while p + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from("<III", data, p)
            p += 12
            name = data[p:p + namesz].split(b"\0")[0].decode("latin-1")
            p += (namesz + 3) & ~3
            desc = data[p:p + descsz]
            p += (descsz + 3) & ~3
            out.append((name, ntype, desc))
    return out


RISCV_REGS = ["pc", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1",
               "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
               "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11",
               "t3", "t4", "t5", "t6"]


def riscv_registers(desc):
    """The register set of one task, from a riscv_prstatus note."""
    if len(desc) < 200:
        return None
    signal, = struct.unpack_from("<H", desc, 12)
    pid, = struct.unpack_from("<I", desc, 24)
    regs = struct.unpack_from("<32I", desc, 72)
    return {"arch": "riscv", "tcb": pid, "signal": signal,
            "regs": dict(zip(RISCV_REGS, regs))}


XTENSA_REGS = ["pc", "ps", "lbeg", "lend", "lcount", "sar", "windowstart",
               "windowbase"]


def xtensa_registers(desc):
    """The register set of one task, from an xtensa_elf_reg_dump_t note."""
    if len(desc) < 400:
        return None
    regs = {}
    for i, name in enumerate(XTENSA_REGS):
        regs[name] = struct.unpack_from("<I", desc, 72 + 4 * i)[0]
    for i in range(16):
        regs["a%d" % i] = struct.unpack_from("<I", desc, 72 + 32 + 224 + 4 * i)[0]
    pid = struct.unpack_from("<I", desc, 72 - 48)[0]
    return {"arch": "xtensa", "tcb": pid, "signal": 0, "regs": regs}


def parse_extra_info(desc, arch):
    """The fault: what went wrong, and which task it went wrong in."""
    info = {}
    if arch == "riscv":
        # riscv_extra_info_t: crashed_task_tcb, isr_context.  The RISC-V
        # coredump carries no mcause/mtval: the crash cause is only in the
        # panic message, the registers here are the general ones.
        (tcb,) = struct.unpack_from("<I", desc, 0)
        info["crashed_task"] = tcb
        info["isr_context"] = struct.unpack_from("<I", desc, 4)[0]
    else:
        # xtensa_extra_info_t: crashed_task_tcb, exccause pair, excvaddr pair
        (tcb,) = struct.unpack_from("<I", desc, 0)
        info["crashed_task"] = tcb
        info["cause"] = struct.unpack_from("<I", desc, 8)[0]
        info["vaddr"] = struct.unpack_from("<I", desc, 16)[0]
    return info


def segments(core):
    """The PT_LOAD segments: address, size and where they are in the file."""
    out = []
    base = core.base
    for ph in range(core.e_phnum):
        off = base + core.e_phoff + ph * core.e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, _ = \
            struct.unpack_from("<IIIIIIII", core.data, off)
        if p_type != PT_LOAD:
            continue
        out.append({"vaddr": p_vaddr, "size": p_filesz,
                    "offset": core.base + p_offset})
    out.sort(key=lambda s: s["vaddr"])
    return out


def find_string(blob, minlen=3):
    """The first printable run in a blob - the task name in the TCB."""
    cur = b""
    for b in blob:
        if 32 <= b < 127:
            cur += bytes([b])
        else:
            if len(cur) >= minlen:
                return cur.decode("latin-1")
            cur = b""
    if len(cur) >= minlen:
        return cur.decode("latin-1")
    return None


def read_mem(data, segs, addr, length):
    """Bytes at a virtual address, or None if the dump does not hold them."""
    for s in segs:
        if s["vaddr"] <= addr and addr + length <= s["vaddr"] + s["size"]:
            off = s["offset"] + (addr - s["vaddr"])
            return data[off:off + length]
    return None


# ---------------------------------------------------------------- the report

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("coredump")
    ap.add_argument("--elf", help="the application ELF, for the symbols")
    ap.add_argument("--dump", action="append", default=[],
                    metavar="ADDR:LEN", help="dump a memory range (repeatable)")
    ap.add_argument("--stack", metavar="TASK", help="print a task's stack")
    ap.add_argument("--lines", type=int, default=40,
                    help="how many stack words to show")
    args = ap.parse_args()

    data, core, (data_len, version, chip_rev) = load_core(args.coredump)
    app = Elf32(open(args.elf, "rb").read()) if args.elf else None

    print("core dump: %d bytes, format v%d, chip rev 0x%x, %s ELF"
          % (data_len, version, chip_rev,
             {0x28: "ARM", 0x3e: "x86-64", 0x5e: "Xtensa", 0xf3: "RISC-V"}
             .get(core.machine, "machine %d" % core.machine)))

    notes = notes_of(data, core)
    tasks = []
    extra = {}
    for name, ntype, desc in notes:
        if name == "CORE":
            if core.machine == 0xf3:
                t = riscv_registers(desc)
            else:
                t = xtensa_registers(desc)
            if t:
                tasks.append(t)
        elif name == "ESP_EXTRA_INFO":
            arch = "riscv" if core.machine == 0xf3 else "xtensa"
            extra = parse_extra_info(desc, arch)
        elif name == "ESP_CORE_DUMP_INFO":
            if len(desc) >= 36:
                sha = desc[4:36].split(b"\0")[0].decode("latin-1")
                print("app SHA256: %s" % sha)

    segs = segments(core)

    # name the tasks: the TCB holds the name, and the TCB is dumped as a
    # PT_LOAD at the task's TCB address
    for t in tasks:
        t["name"] = None
        for s in segs:
            if s["vaddr"] == t["tcb"]:
                blob = data[s["offset"]:s["offset"] + s["size"]]
                t["name"] = find_string(blob)
                t["tcb_seg"] = s
                break

    crashed = None
    if "crashed_task" in extra:
        for t in tasks:
            if t["tcb"] == extra["crashed_task"]:
                crashed = t

    if extra:
        print("\nfault: cause 0x%x (%s), address 0x%08x"
              % (extra.get("cause", 0), cause_name(extra.get("cause", 0)),
                 extra.get("vaddr", 0)))

    print("\ntasks (%d):" % len(tasks))
    for t in tasks:
        mark = "  <-- crashed" if t is crashed else ""
        pc = t["regs"].get("pc", 0)
        if app:
            where = app.describe(pc)
        else:
            where = "0x%08x" % pc
        print("  %-16s tcb 0x%08x  pc %s%s"
              % (t["name"] or "?", t["tcb"], where, mark))

    if crashed:
        print("\nregisters of the crashed task (%s):" % (crashed["name"] or "?"))
        for name in RISCV_REGS:
            if name in crashed["regs"]:
                v = crashed["regs"][name]
                extra_txt = ""
                if app:
                    extra_txt = "   " + app.describe(v)
                print("  %-4s 0x%08x%s" % (name, v, extra_txt))
        if app:
            print("\nbacktrace (return addresses found on the stack):")
            for a in stack_returns(data, segs, crashed, app):
                print("  %s" % app.describe(a))

    for spec in args.dump:
        addr, _, length = spec.partition(":")
        addr = int(addr, 0)
        length = int(length, 0) if length else 32
        blob = read_mem(data, segs, addr, length)
        print("\nmemory at 0x%08x (%d bytes):" % (addr, length))
        if blob is None:
            print("  not in the dump")
            continue
        for i in range(0, len(blob), 16):
            chunk = blob[i:i + 16]
            words = struct.unpack("<%dI" % (len(chunk) // 4), chunk[:len(chunk) // 4 * 4])
            text = "  ".join("%08x" % w for w in words)
            line = "  0x%08x: %s" % (addr + i, text)
            if app:
                sym = app.symbol_of(addr + i)
                if sym:
                    line += "   ; %s" % sym[2]
            print(line)

    if args.stack:
        for t in tasks:
            if t["name"] == args.stack:
                print("\nstack of %s:" % t["name"])
                for s in segs:
                    if s["vaddr"] == t["tcb"]:
                        continue
                    blob = data[s["offset"]:s["offset"] + s["size"]]
                    for i in range(0, min(len(blob), args.lines * 4), 16):
                        chunk = blob[i:i + 16]
                        words = struct.unpack("<%dI" % (len(chunk) // 4),
                                              chunk[:len(chunk) // 4 * 4])
                        print("  0x%08x: %s" % (s["vaddr"] + i,
                                                "  ".join("%08x" % w for w in words)))


def cause_name(cause):
    """The RISC-V and Xtensa exception causes, by number."""
    riscv = {0: "instruction address misaligned", 1: "instruction access fault",
             2: "illegal instruction", 3: "breakpoint", 4: "load address misaligned",
             5: "load access fault", 6: "store address misaligned",
             7: "store access fault", 8: "ecall", 9: "reserved"}
    xtensa = {0: "illegal instruction", 2: "syscall", 3: "breakpoint",
              4: "load address misaligned", 5: "load access fault",
              6: "store address misaligned", 7: "store access fault",
              8: "divide by zero", 9: "privileged", 12: "instruction fetch fault",
              14: "data break", 16: "coprocessor"}
    return riscv.get(cause, xtensa.get(cause, "unknown"))


def stack_returns(data, segs, task, app):
    """Return addresses on the task's stack - the backtrace."""
    out = []
    for s in segs:
        if s["vaddr"] == task["tcb"]:
            continue
        blob = data[s["offset"]:s["offset"] + s["size"]]
        for i in range(0, len(blob) - 3, 4):
            word, = struct.unpack_from("<I", blob, i)
            sec = app.section_of(word)
            if sec and sec["flags"] & 0x4:  # SHF_EXECINSTR
                sym = app.symbol_of(word)
                name = sym[2] if sym else ""
                if name not in [n for _, n in out]:
                    out.append((word, name))
    return [w for w, _ in out]


if __name__ == "__main__":
    main()
