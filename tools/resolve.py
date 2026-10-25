#!/usr/bin/env python3
"""Offline twin of patch/src/resolver.c: finds every address stwpatch needs in a Stormworks exe
(client or dedicated server) from RTTI names and byte patterns. Used to validate the resolver
against Ghidra and to check a new game build before playing.

    tools/resolve.py path/to/stormworks64.exe [path/to/server64.exe ...]
"""
import re
import struct
import sys


class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.d, 0x3c)[0]
        n = struct.unpack_from("<H", self.d, pe + 6)[0]
        opt = struct.unpack_from("<H", self.d, pe + 20)[0]
        self.base = struct.unpack_from("<Q", self.d, pe + 24 + 24)[0]
        self.secs = []
        for i in range(n):
            name, vs, va, rs, rp = struct.unpack_from("<8sIIII", self.d, pe + 24 + opt + i * 40)
            self.secs.append((name.rstrip(b"\0").decode(), va, max(vs, rs), rp, rs))

    def sec(self, name):
        for s in self.secs:
            if s[0] == name:
                return s
        raise KeyError(name)

    def off(self, rva):
        for _, va, size, rp, rs in self.secs:
            if va <= rva < va + size and rva - va < rs:
                return rp + rva - va
        return None

    def rva(self, off):
        for _, va, size, rp, rs in self.secs:
            if rp <= off < rp + rs:
                return va + off - rp
        return None

    def bytes_at(self, rva, n):
        o = self.off(rva)
        return self.d[o:o + n]

    def u32(self, rva):
        return struct.unpack("<I", self.bytes_at(rva, 4))[0]

    def i32(self, rva):
        return struct.unpack("<i", self.bytes_at(rva, 4))[0]

    def u64(self, rva):
        return struct.unpack("<Q", self.bytes_at(rva, 8))[0]

    def find(self, sec, pattern):
        """pattern: hex string with ?? wildcards. Returns list of RVAs."""
        _, va, size, rp, rs = self.sec(sec)
        rx = b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in pattern.split())
        return [va + m.start() for m in re.finditer(rx, self.d[rp:rp + rs], re.S)]


def vtable(pe, type_name):
    """MSVC x64 RTTI: TypeDescriptor{vftable, spare, name}, COL{sig=1, off, cd, td, chd, self}."""
    _, dva, dsize, drp, drs = pe.sec(".data")
    i = pe.d.find(type_name.encode() + b"\0", drp, drp + drs)
    if i < 0:
        return None
    td = pe.rva(i) - 0x10
    _, rva_, rsize, rrp, rrs = pe.sec(".rdata")
    blob = pe.d[rrp:rrp + rrs]
    for m in re.finditer(re.escape(struct.pack("<I", td)), blob):
        col = rva_ + m.start() - 12
        if col % 4:
            continue
        sig, offset, _, tdr, _, selfr = struct.unpack_from("<IIIIII", blob, m.start() - 12)
        if sig == 1 and offset == 0 and tdr == td and selfr == col:
            ref = struct.pack("<Q", pe.base + col)
            j = blob.find(ref)
            if j >= 0:
                return rva_ + j + 8
    return None


def rel_target(pe, insn_rva, opcode_len=1):
    return insn_rva + opcode_len + 4 + pe.i32(insn_rva + opcode_len)


def calls_in(pe, start, length):
    """(rva, kind, target) for every E8/E9 rel32 in [start, start+length)."""
    code = pe.bytes_at(start, length)
    out = []
    for i in range(len(code) - 4):
        if code[i] in (0xE8, 0xE9):
            t = start + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
            if pe.sec(".text")[1] <= t < pe.sec(".text")[1] + pe.sec(".text")[2]:
                out.append((start + i, "call" if code[i] == 0xE8 else "jmp", t))
    return out


def follow_thunks(pe, rva):
    """A function that is a bare 'jmp rel32' (incremental-link style thunk) resolves to its target."""
    for _ in range(4):
        if pe.bytes_at(rva, 1) != b"\xE9":
            break
        rva = rel_target(pe, rva)
    return rva


def lea_refs(pe, target):
    """RVAs of 'lea r64,[rip+disp32]' (48/4C 8D modrm) pointing at target."""
    out = []
    for p in ("48 8D 0D", "48 8D 15", "48 8D 05", "4C 8D 05", "4C 8D 0D", "48 8D 1D", "48 8D 35", "48 8D 3D"):
        for r in pe.find(".text", p + " ?? ?? ?? ??"):
            if r + 7 + pe.i32(r + 3) == target:
                out.append(r)
    return sorted(out)


def string_rva(pe, s):
    _, va, size, rp, rs = pe.sec(".rdata")
    i = pe.d.find(b"\0" + s.encode() + b"\0", rp, rp + rs)
    return None if i < 0 else pe.rva(i + 1)


def resolve(path):
    pe = PE(path)
    r = {}
    for key, name in (("vt_dispatcher", ".?AVbtCollisionDispatcher@@"),
                      ("vt_hashed_paircache", ".?AVbtHashedOverlappingPairCache@@"),
                      ("vt_softrigid_world", ".?AVbtSoftRigidDynamicsWorld@@"),
                      ("vt_discrete_world", ".?AVbtDiscreteDynamicsWorld@@"),
                      ("vt_vehicle_tick", ".?AUthreaded_tick_task@c_scene_vehicle_body@@"),
                      ("vt_scene_update", ".?AUupdate_thread_task@c_scene@@")):
        r[key] = vtable(pe, name)
    base = pe.base
    if r["vt_softrigid_world"]:
        r["step_simulation"] = pe.u64(r["vt_softrigid_world"] + 12 * 8) - base
    if r["vt_dispatcher"]:
        for slot, key in ((2, "disp_new_manifold"), (3, "disp_release_manifold"), (7, "disp_dispatch_all"),
                          (13, "disp_alloc_algo"), (14, "disp_free_algo")):
            r[key] = pe.u64(r["vt_dispatcher"] + slot * 8) - base
        # constructor: lea rax,[vtable] ... lea rax,[defaultNearCallback]; mov [reg+30h],rax
        for ref in lea_refs(pe, r["vt_dispatcher"]):
            for m in pe.find(".text", "48 8D 05 ?? ?? ?? ?? 48 89 ?? 30"):
                if ref < m < ref + 0x80:
                    r["default_near"] = m + 7 + pe.i32(m + 3)
                    break
            if "default_near" in r:
                break

    # mm_thread_object::enqueue: sets task->pending (byte [rdx+0B0h]) = 1 first
    enq = pe.find(".text", "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 EC 40 48 8B EA 48 8B F1 C6 82 B0 00 00 00 01")
    if len(enq) == 1:
        e = r["thread_enqueue"] = enq[0]
        cs = calls_in(pe, e, 0xe0)
        r["mtx_lock"] = cs[0][2]
        r["mtx_unlock"] = next(t for a, k, t in cs if k == "jmp")
        code = pe.bytes_at(e, 0xe0)
        i = code.find(bytes.fromhex("48 8B CD E8"))             # mov rcx,rbp ; call set_state
        r["task_set_state"] = rel_target(pe, e + i + 3)
        i = code.find(bytes.fromhex("48 8D 8E 90 00 00 00 E8"))  # lea rcx,[rsi+90h] ; call _Cnd_broadcast
        r["cnd_broadcast"] = rel_target(pe, e + i + 7)
    # mm_thread_task::wait: lock [rcx+10h], loop on state [rdi+8] with cond [rdi+68h]
    w = pe.find(".text", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F9 48 8D 59 10 33 F6")
    w = [x for x in w if bytes.fromhex("48 8D 4F 68 E8") in pe.bytes_at(x, 0x80)]
    if len(w) == 1:
        r["task_wait"] = w[0]

    # Bullet profiler: Start_Profile is called right after lea rcx,"internalSingleStepSimulation"
    s = string_rva(pe, "internalSingleStepSimulation")
    for ref in lea_refs(pe, s) if s else []:
        cs = [c for c in calls_in(pe, ref + 7, 24) if c[1] == "call"]
        if cs:
            r["profile_start"] = follow_thunks(pe, cs[0][2])
            break
    s = string_rva(pe, "dispatchAllCollisionPairs")
    for ref in lea_refs(pe, s) if s else []:
        cs = calls_in(pe, ref, 0x80)
        for (a1, k1, t1), (a2, k2, t2) in zip(cs, cs[1:]):
            if k1 == "call" and a2 - a1 <= 0x30 and t1 == t2:    # Stop_Profile(); Stop_Profile(); (2nd may be a tail jmp)
                r["profile_stop"] = follow_thunks(pe, t1)
                break
        if "profile_stop" in r:
            break
    # Server build: CProfileSample ctor/dtor call through bts_enterFunc / bts_leaveFunc pointers
    #   ctor: push rbx; sub rsp,20h; mov rbx,rcx; mov rcx,rdx; call [rip+enter_ptr]
    #   dtor: sub rsp,28h; call [rip+leave_ptr]
    for key, pat in (("profile_start", "40 53 48 83 EC 20 48 8B D9 48 8B CA FF 15"), ("profile_stop", "48 83 EC 28 FF 15")):
        if key in r and pe.bytes_at(r[key], len(pat.split())) == bytes.fromhex(pat):
            at = r[key] + len(pat.split())
            ptr = at + 4 + pe.i32(at)
            r[key + "_ptr"] = ptr
            r[key] = pe.u64(ptr) - base        # static initial value; read at run time in the DLL
    if "profile_start" in r:
        code = pe.bytes_at(r["profile_start"], 0x40)
        i = code.find(b"\x41\xB8")                    # mov r8d, <offset of the thread index in the TLS block>
        if i >= 0:
            r["profile_tls_offset"] = struct.unpack_from("<I", code, i + 2)[0]
    if "profile_start" in r:
        # first rip-relative lea in Start_Profile = CProfileManager::CurrentNode[64], initialised to &gRoots[i]
        code = pe.bytes_at(r["profile_start"], 0x80)
        _, dva, dsize, _, _ = pe.sec(".data")
        for i in range(len(code) - 7):
            if code[i] in (0x48, 0x4C) and code[i + 1] == 0x8D and (code[i + 2] & 0xC7) == 0x05:
                cur = r["profile_start"] + i + 7 + struct.unpack_from("<i", code, i + 3)[0]
                if dva <= cur < dva + dsize:
                    r["profile_current_nodes"] = cur
                    r["profile_roots"] = pe.u64(cur) - base
                    break
    return pe, r


EXPECTED_CLIENT = {   # stormworks64.exe PE ts 0x6a9a9e20, from Ghidra
    "vt_dispatcher": 0xae0a30, "vt_hashed_paircache": 0xae0810, "vt_softrigid_world": 0xae3760,
    "vt_discrete_world": 0xae30a8, "vt_vehicle_tick": 0xb3ff28, "vt_scene_update": 0xb80fd0,
    "step_simulation": 0x133ea0, "disp_new_manifold": 0xd7f40, "disp_release_manifold": 0xd81e0,
    "disp_dispatch_all": 0xd8450, "disp_alloc_algo": 0xd8630, "disp_free_algo": 0xd8660,
    "default_near": 0xd84b0, "thread_enqueue": 0x17d720, "mtx_lock": 0x95ab30, "mtx_unlock": 0x95ab54,
    "task_set_state": 0x17d1b0, "cnd_broadcast": 0x95ae24, "task_wait": 0x17d2c0,
    "profile_start": 0x163440, "profile_stop": 0x163360, "profile_roots": 0xcbf730,
}

if __name__ == "__main__":
    for path in sys.argv[1:]:
        pe, r = resolve(path)
        print(f"== {path}")
        ok = True
        for k, v in r.items():
            exp = EXPECTED_CLIENT.get(k) if path.lower().endswith("stormworks64.exe") else None
            mark = "" if exp is None else ("  ok" if exp == v else f"  MISMATCH expected 0x{exp:x}")
            ok &= not mark.strip().startswith("MISMATCH")
            print(f"  {k:24s} {'None' if v is None else hex(v)}{mark}")
        missing = [k for k in EXPECTED_CLIENT if r.get(k) is None]
        print("  missing:", missing or "none")
