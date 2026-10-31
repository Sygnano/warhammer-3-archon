"""Offline signature test for the Archon Mode native patches .

Reproduces the EXACT locator logic that archon_native.dll (v1.0.0) runs at load, but over the bytes of the
exe COPY on disk (never a running game). Confirms that each patch target resolves to exactly one address and
matches the reference VAs. For Patch C it additionally decodes the
involved instructions with capstone. For Patch D (partner leave) it follows the whole chain: player_left ->
sender of CCQ_FACTION_SWITCH_HUMAN_TO_AI -> command id slot == the slot registered for that CCQ name ->
its executor -> world switch -> set_idle_human writing IDLE_HUMAN (1) to faction+0x1bc4. For Patch E (resumed-lobby
seats) it follows the host join handler -> seating wrapper -> saved-key picker (exact bytes, layout decoded with
capstone), cross-checked against EMPIRE_MP::set_faction. Static, read-only.

Usage: python -I offline_scan_test.py <path-to-Warhammer3.exe copy>
Exit code 0 on full success, 1 on any mismatch.
"""
import sys, struct, hashlib, re, os
import capstone

# ---- the tested build ----
PIN_SHA256 = "9bd58c63bd7fc5c9909b3d3d9b6b7a6ae4eac784369bf91a9e53c3e9f42be496"
PIN_TS     = 0x6ACEC6F4
PIN_SOI    = 0x0E029000
PIN_SIZE   = 232531920
# reference VAs
REF_CANSET   = 0x141f2cc8c
REF_DUPSTR   = 0x143818bd8
REF_LEA      = 0x141f41208
REF_BT       = 0x141f411e6
DUP_KEY      = b"mp_ready_error_faction_duplicates"
CANSET_SIG   = "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 48 8B FA 48 8B E9 B3 01 48 85 D2"
LEA_TO_BT    = 0x22
BT_EXPECT    = bytes([0x48, 0x0F, 0xBA, 0xE0, 0x1C])
# Patch C (ready-mask producer)
REF_PROD_JNE  = 0x1421b59d5
REF_PROD_DUP  = 0x1421b5b70
REF_PROD_CONT = 0x1421b5b89
REF_PROD_HEAD = 0x1421b5947
PRODUCER_SIG = ("4D 8B C7 48 8D 55 70 48 8D 4C 24 50 E8 ?? ?? ?? ?? 48 8D 4C 24 58 48 39 08 "
                "0F 85 ?? ?? ?? ?? 4D 8B C7 48 8D 54 24 38 48 8D 4C 24 50 E8 ?? ?? ?? ?? E9 ?? ?? ?? ??")
PSIG_JNE_OFF, PSIG_JMP_OFF = 25, 49
# Patch D (partner leave)
REF_LEAVE_SEND    = 0x1421d1f5c
REF_LEAVE_HANDLER = 0x1428b2ea0
REF_LEAVE_SETIDLE = 0x1426cf528
REF_LEAVE_GETKEY  = 0x1421b36c0
REF_LEAVE_SLOT    = 0x143f8a178
BTS_RAX_1C      = bytes([0x48, 0x0F, 0xBA, 0xE8, 0x1C])
LOOPHEAD_EXPECT = bytes.fromhex("488b06488bceff90680100003bd80f83")
INACTIVE_EXPECT = bytes.fromhex("24033c010f85")

def sig_regex(sig):
    return b"".join((b"." if t == "??" else re.escape(bytes([int(t, 16)]))) for t in sig.split())

def fail(msg):
    print("FAIL:", msg); sys.exit(1)

def main():
    path = sys.argv[1]
    data = open(path, "rb").read()

    # identity
    sha = hashlib.sha256(data).hexdigest()
    if len(data) != PIN_SIZE: fail(f"file size {len(data)} != pinned {PIN_SIZE}")
    if sha != PIN_SHA256:      fail(f"sha256 {sha} != pinned")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    ts  = struct.unpack_from("<I", data, e_lfanew + 8)[0]
    opt = e_lfanew + 24
    soi = struct.unpack_from("<I", data, opt + 56)[0]
    imgbase = struct.unpack_from("<Q", data, opt + 24)[0]
    if ts  != PIN_TS:  fail(f"PE timestamp 0x{ts:08x} != 0x{PIN_TS:08x}")
    if soi != PIN_SOI: fail(f"SizeOfImage 0x{soi:x} != 0x{PIN_SOI:x}")

    # sections
    nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    optsize = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    sh = e_lfanew + 24 + optsize
    secs = {}
    allsecs = []
    for i in range(nsec):
        o = sh + 40 * i
        name = data[o:o+8].rstrip(b"\0").decode("latin1")
        vsz  = struct.unpack_from("<I", data, o + 8)[0]
        va   = struct.unpack_from("<I", data, o + 12)[0]
        praw = struct.unpack_from("<I", data, o + 20)[0]
        sraw = struct.unpack_from("<I", data, o + 16)[0]
        chars= struct.unpack_from("<I", data, o + 36)[0]
        secs[name] = (va, vsz, praw, sraw, chars)
        allsecs.append((name, va, vsz, praw, sraw, chars))
    MEM_EXECUTE, MEM_READ, MEM_WRITE = 0x20000000, 0x40000000, 0x80000000

    if ".udata" not in secs: fail("no .udata")
    if ".xcode" not in secs: fail("no .xcode")
    uva, uvsz, upraw, usraw, uchars = secs[".udata"]
    xva, xvsz, xpraw, xsraw, xchars = secs[".xcode"]
    if not (uchars & MEM_EXECUTE) or (uchars & MEM_WRITE): fail(".udata not non-writable code")
    if (xchars & MEM_WRITE): fail(".xcode is writable")
    code  = data[upraw:upraw + min(uvsz, usraw)]
    xdata = data[xpraw:xpraw + min(xvsz, xsraw)]

    # Patch A: unique canset signature in .udata
    toks = CANSET_SIG.split()
    pat = b"".join((b"." if t == "??" else re.escape(bytes([int(t, 16)]))) for t in toks)
    hits = [m.start() for m in re.finditer(pat, code, re.DOTALL)]
    if len(hits) != 1: fail(f"canset signature count = {len(hits)} (want 1)")
    canset_va = imgbase + uva + hits[0]

    # Patch B: find string in .xcode, then unique lea->string in .udata, then bt site
    si = xdata.find(DUP_KEY)
    if si < 0 or xdata[si + len(DUP_KEY)] != 0: fail("dup loc key not found as C-string in .xcode")
    dupstr_va = imgbase + xva + si
    lea_hits = []
    for m in re.finditer(rb"\x48\x8d\x15", code):
        i = m.start()
        disp = struct.unpack_from("<i", code, i + 3)[0]
        insn = imgbase + uva + i
        if insn + 7 + disp == dupstr_va:
            lea_hits.append(insn)
    if len(lea_hits) != 1: fail(f"lea->dupstring count = {len(lea_hits)} (want 1)")
    lea_va = lea_hits[0]
    bt_va  = lea_va - LEA_TO_BT
    bt_off = bt_va - imgbase - uva
    if code[bt_off:bt_off + 5] != BT_EXPECT: fail("bytes at bt site are not `bt rax,0x1c`")

    # Patch C: producer signature, unique across ALL execute-and-not-writable sections
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    rx = sig_regex(PRODUCER_SIG)
    prod_hits, scanned = [], []
    for (name, va, vsz, praw, sraw, chars) in allsecs:
        if not (chars & MEM_EXECUTE) or (chars & MEM_WRITE): continue
        scanned.append(name)
        sdata = data[praw:praw + min(vsz, sraw)]
        for m in re.finditer(rx, sdata, re.DOTALL):
            prod_hits.append((name, imgbase + va + m.start()))
    print("producer sig    : %d hit(s) across non-writable code sections %s" % (len(prod_hits), scanned))
    if len(prod_hits) != 1: fail(f"producer signature count = {len(prod_hits)} (want 1)")
    if prod_hits[0][0] != ".udata": fail("producer hit not in .udata")
    psig_va = prod_hits[0][1]
    def rd(va, n):
        off = va - imgbase - uva
        if off < 0 or off + n > len(code): fail(f"read 0x{va:x}+{n} outside .udata")
        return code[off:off + n]
    def rel32(insn_va, rel_off, insn_len):
        return insn_va + insn_len + struct.unpack("<i", rd(insn_va + rel_off, 4))[0]
    def dis1(va):
        return next(md.disasm(rd(va, 15), va, count=1))
    jne_va = psig_va + PSIG_JNE_OFF
    orig = rd(jne_va, 6)
    if orig[:2] != b"\x0f\x85": fail("no jne at producer signature offset")
    dup_va = rel32(jne_va, 2, 6)
    bts_at = next((i for i in range(17) if rd(dup_va + i, 5) == BTS_RAX_1C), None)
    if bts_at is None: fail("jne target has no bts rax,0x1c within 16 bytes")
    jmp_va = psig_va + PSIG_JMP_OFF
    if rd(jmp_va, 1) != b"\xe9": fail("no jmp at producer signature end")
    cont_va = rel32(jmp_va, 1, 5)
    cb = rd(cont_va, 7)
    if cb[0:2] != b"\xff\xc3" or cb[2] != 0xE9: fail("continue target is not inc ebx; jmp")
    if not (dup_va < cont_va <= dup_va + 0x40) or rd(cont_va - 5, 1) != b"\xe8":
        fail("duplicate block does not fall through into the continue path")
    head_va = rel32(cont_va + 2, 1, 5)
    if rd(head_va, len(LOOPHEAD_EXPECT)) != LOOPHEAD_EXPECT: fail("loop head bytes mismatch")
    if not (head_va < jne_va and jne_va - head_va < 0x200): fail("loop head not shortly before jne")
    inactive = None
    for p in range(head_va, jne_va - 9):
        if rd(p, 6) == INACTIVE_EXPECT and rel32(p + 4, 2, 6) == cont_va: inactive = p + 4; break
    if inactive is None: fail("inactive-slot jne to the continue path not found")
    newrel = cont_va - (jne_va + 6)
    new = b"\x0f\x85" + struct.pack("<i", newrel)

    # capstone confirmation of every instruction the patch relies on
    ins_jne = dis1(jne_va); ins_bts = dis1(dup_va + bts_at)
    ins_inc = dis1(cont_va); ins_jmp = dis1(cont_va + 2); ins_ina = dis1(inactive)
    ins_new = next(md.disasm(new, jne_va, count=1))
    checks = [
        (ins_jne.mnemonic == "jne" and int(ins_jne.op_str, 16) == dup_va, f"{ins_jne.mnemonic} {ins_jne.op_str}"),
        (ins_bts.mnemonic == "bts" and ins_bts.op_str == "rax, 0x1c",       f"{ins_bts.mnemonic} {ins_bts.op_str}"),
        (ins_inc.mnemonic == "inc" and ins_inc.op_str == "ebx",             f"{ins_inc.mnemonic} {ins_inc.op_str}"),
        (ins_jmp.mnemonic == "jmp" and int(ins_jmp.op_str, 16) == head_va,  f"{ins_jmp.mnemonic} {ins_jmp.op_str}"),
        (ins_ina.mnemonic == "jne" and int(ins_ina.op_str, 16) == cont_va,  f"{ins_ina.mnemonic} {ins_ina.op_str}"),
        (ins_new.mnemonic == "jne" and int(ins_new.op_str, 16) == cont_va and ins_new.size == 6,
                                                                            f"{ins_new.mnemonic} {ins_new.op_str}"),
    ]
    for ok, txt in checks:
        if not ok: fail("capstone check failed: " + txt)
    print("producer jne    : 0x%x  %s  (%s)" % (jne_va, orig.hex(" "), f"{ins_jne.mnemonic} {ins_jne.op_str}"))
    print("  dup block     : 0x%x  bts rax,0x1c at +%d" % (dup_va, bts_at))
    print("  continue      : 0x%x  inc ebx; jmp 0x%x (loop head)" % (cont_va, head_va))
    print("  inactive jne  : 0x%x  -> 0x%x (same continue path)" % (inactive, cont_va))
    print("  patched bytes : %s  (jne 0x%x)" % (new.hex(" "), cont_va))

    # cross-check vs reference VAs
    print("exe sha256      :", sha, "(match)")
    print("PE timestamp    : 0x%08x (match)" % ts)
    print("SizeOfImage     : 0x%x (match)" % soi)
    print("--- resolved targets (resolved vs reference) ---")
    def chk(name, got, ref):
        ok = got == ref
        print("  %-14s 0x%x   ref 0x%x   %s" % (name, got, ref, "OK" if ok else "MISMATCH"))
        return ok
    allok = True
    allok &= chk("CanSetFaction", canset_va, REF_CANSET)
    allok &= chk("dup loc key",   dupstr_va, REF_DUPSTR)
    allok &= chk("lea->dupkey",   lea_va,    REF_LEA)
    allok &= chk("bt rax,0x1c",   bt_va,     REF_BT)
    allok &= chk("producer jne",  jne_va,    REF_PROD_JNE)
    allok &= chk("producer dup",  dup_va,    REF_PROD_DUP)
    allok &= chk("producer cont", cont_va,   REF_PROD_CONT)
    allok &= chk("loop head",     head_va,   REF_PROD_HEAD)
    if not allok: fail("a resolved address does not match the reference VAs")
    print("ALL THREE TARGETS (A canset, B readyerr, C producer) RESOLVED UNIQUELY AND MATCH THE REFERENCE VAs")

    # ---------------------------------------------------------------------------------------------
    # Patch D (partner leave): same locator chain as resolve_leave() in archon_native.c
    print("--- patch D (partner leave) ---")
    code_secs = [(name, va, vsz, praw, sraw) for (name, va, vsz, praw, sraw, chars) in allsecs
                 if (chars & MEM_EXECUTE) and not (chars & MEM_WRITE)]
    def wstr_va(s):
        needle = s.encode("utf-16le") + b"\0\0"
        i = 0
        while True:
            i = xdata.find(needle, i)
            if i < 0: return None
            if i % 2 == 0: return imgbase + xva + i
            i += 1
    def riplea_hits(target):
        hits = []
        for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", code):
            i = m.start()
            if i + 7 > len(code): continue
            if imgbase + uva + i + 7 + struct.unpack_from("<i", code, i + 3)[0] == target: hits.append(imgbase + uva + i)
        return hits
    def sig_hits(sig, lo, hi):
        rx = sig_regex(sig)
        a, b = lo - imgbase - uva, hi - imgbase - uva
        return [imgbase + uva + a + m.start() for m in re.finditer(rx, code[a:b], re.DOTALL)
                if a + m.start() + len(sig.split()) <= b]
    def sig_at(sig, va):
        return re.fullmatch(sig_regex(sig), rd(va, len(sig.split())), re.DOTALL) is not None
    def u32(va): return struct.unpack("<I", rd(va, 4))[0]
    def i32(va): return struct.unpack("<i", rd(va, 4))[0]
    def ins(va): i = dis1(va); return f"{i.mnemonic} {i.op_str}"

    PL_CALL_SIG = ("48 8B 97 ?? ?? ?? ?? 48 85 D2 74 ?? 48 8B 52 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B D0 48 8D 4F F8 "
                   "E8 ?? ?? ?? ?? 40 B6 01")
    pl_str = wstr_va("EMPIRE_MP::player_left [player_id %u] [is_local_player %u] [reason: %s]")
    if not pl_str: fail("D: player_left string not found")
    h = riplea_hits(pl_str)
    if len(h) != 1: fail(f"D: lea->player_left string count {len(h)}")
    pl_lea = h[0]
    h = sig_hits(PL_CALL_SIG, pl_lea, pl_lea + 0x200)
    if len(h) != 1: fail(f"D: send call signature in player_left count {len(h)}")
    call = h[0]
    g = 0
    for (name, va, vsz, praw, sraw) in code_secs:
        g += len(list(re.finditer(sig_regex(PL_CALL_SIG), data[praw:praw + min(vsz, sraw)], re.DOTALL)))
    if g != 1: fail(f"D: send call signature global count {g}")
    off_setup, off_setup_sub = u32(call + 3) + 8, rd(call + 15, 1)[0]
    getkey, send = rel32(call + 19, 1, 5), rel32(call + 31, 1, 5)
    h = sig_hits("48 8B 8F ?? ?? ?? ?? 49 8B D7 40 32 F6 E8 ?? ?? ?? ??", pl_lea, call)
    if len(h) != 1: fail("D: list load in player_left")
    off_list, lookup = u32(h[0] + 3) + 8, rel32(h[0] + 13, 1, 5)
    if len(sig_hits("8B 80 38 01 00 00 8B C8 80 E1 11 80 F9 01 75 ?? 83 E0 25 3C 25 74 ??", h[0], call)) != 1:
        fail("D: flag predicate")
    if not sig_at("48 83 EC 28 83 3A 20 74 ?? 8B 02 8B 94 81 ?? ?? ?? ?? E8 ?? ?? ?? ??", lookup): fail("D: lookup bytes")
    off_map, recat = u32(lookup + 14), rel32(lookup + 18, 1, 5)
    if rd(recat, 33) != bytes.fromhex("39910028000076168bc2488d048048c1e0064803c1f680380100000175 0233c0c3".replace(" ", "")):
        fail("D: rec_at bytes")
    if not sig_at("40 53 48 83 EC 20 48 8B D9 48 8B CA E8 ?? ?? ?? ?? 8B 93 90 00 00 00 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 06 48 8B 40 08", getkey):
        fail("D: getkey bytes")
    hpl = wstr_va("called handle_player_left() [player %llu] [player-id %d] [reason %d]")
    h = riplea_hits(hpl) if hpl else []
    if len(h) != 1: fail("D: lea->handle_player_left string")
    hl = sig_hits("48 8B 8B ?? ?? ?? ?? 4C 8B D8 48 8B 51 ?? 8B 8A ?? ?? ?? ??", h[0], h[0] + 0x100)
    hlist = sig_hits("4C 8B AB ?? ?? ?? ?? 49 8D 57 08 49 8B CD E8", h[0], h[0] + 0x100)
    if len(hl) != 1 or len(hlist) != 1: fail("D: handle_player_left local chain / list")
    loc = (u32(hl[0] + 3), rd(hl[0] + 13, 1)[0], u32(hl[0] + 16))
    if u32(hlist[0] + 3) != off_list: fail("D: list offset differs between player_left and handle_player_left")
    SEND_SIG = ("48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 8B FA 48 83 C1 10 48 8B 01 FF 50 20 84 C0 74 22 "
                "48 8B 8B ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8D 54 24 20 48 89 44 24 20 48 89 7C 24 28 "
                "E8 ?? ?? ?? ?? 48 8B 5C 24 40 48 83 C4 30 5F C3")
    if not sig_at(SEND_SIG, send): fail("D: sender bytes")
    prologue = rd(send, 16)
    if prologue != bytes.fromhex("48895c24085748 83ec30488bd9488bfa".replace(" ", "")): fail("D: sender prologue")
    for o in (0, 5, 6, 10, 13):   # the 16 relocated bytes are position independent (no rip / rel operands)
        i = dis1(send + o)
        if "rip" in i.op_str or i.mnemonic.startswith("j") or i.mnemonic == "call": fail("D: prologue not relocatable")
    inner = rel32(send + 59, 1, 5)
    if not sig_at("48 89 5C 24 18 57 48 83 EC 40 8B 05 ?? ?? ?? ?? 48 8B D9 48 8D 4C 24 50 89 44 24 50 48 8B FA "
                  "E8 ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ??", inner): fail("D: command builder bytes")
    slot1, slot2 = inner + 16 + i32(inner + 12), inner + 43 + i32(inner + 39)
    if slot1 != slot2: fail("D: builder id slots differ")
    ccq = xdata.find(b"CCQ_FACTION_SWITCH_HUMAN_TO_AI\0")
    if ccq < 0: fail("D: CCQ string")
    ccq_va = imgbase + xva + ccq
    ccq_lea = [x for x in riplea_hits(ccq_va) if rd(x, 3) == b"\x48\x8d\x15"]
    if len(ccq_lea) != 1: fail("D: lea rdx->CCQ string")
    cl = ccq_lea[0]
    if rd(cl - 10, 3) != b"\x4c\x8d\x0d" or rd(cl - 3, 3) != b"\x45\x33\xc0" or rd(cl + 7, 3) != b"\x48\x8d\x0d":
        fail("D: CCQ registration shape")
    if cl + 14 + i32(cl + 10) != slot1: fail("D: sender does not send CCQ_FACTION_SWITCH_HUMAN_TO_AI")
    handler = cl - 3 + i32(cl - 7)
    h = sig_hits("48 8B D0 48 8B CF E8 ?? ?? ?? ??", handler, handler + 0x6e)
    if len(h) != 1: fail("D: executor -> world switch")
    world = rel32(h[0] + 6, 1, 5)
    h = sig_hits("48 8B CA E8 ?? ?? ?? ??", world, world + 0x20)
    if len(h) != 1: fail("D: world switch -> set_idle")
    setidle = rel32(h[0] + 3, 1, 5)
    h = sig_hits("C6 86 C4 1B 00 00 01", setidle, setidle + 0x100)
    if len(h) != 1: fail("D: set_idle does not write IDLE_HUMAN")
    idle_write = h[0]
    # the faction player-type byte +0x1bc4: CcoCampaignFaction IsIdleHuman getter compares it with 1
    getter = sig_hits("48 8B 41 68 80 B8 C4 1B 00 00 01 0F 94 C0 C3", imgbase + uva, imgbase + uva + len(code))
    pvt = send + 37 + 7 + i32(send + 40)
    if not (xva <= pvt - imgbase < xva + len(xdata)): fail("D: payload vtable not in .xcode")
    vt0 = struct.unpack_from("<Q", xdata, pvt - imgbase - xva)[0]
    if not sig_at("48 83 EC 28 48 8B C2 48 8B 51 08 48 8B C8 E8 ?? ?? ?? ?? B0 01 48 83 C4 28 C3", vt0): fail("D: vt0")
    ser = rel32(vt0 + 14, 1, 5)
    if not sig_at("48 89 5C 24 08 57 48 81 EC 30 01 00 00 48 8B D9 48 8B FA 48 8B CA E8 ?? ?? ?? ??", ser): fail("D: serialiser")
    slen = rel32(ser + 22, 1, 5)
    if rd(ser + 0x3f, 4) != b"\x48\x8b\xcf\xe8": fail("D: c_str call")
    cstr = rel32(ser + 0x42, 1, 5)
    if rd(slen, 46) != bytes.fromhex("488b410849b8000000000000 00f0488bd04923d049b8 0000000000000080493bd07403 8b01c348c1e83883e00fc3".replace(" ", "")):
        fail("D: strlen bytes")
    if rd(cstr, 40) != bytes.fromhex("488b410849b800000000000000f0488bd04923d049b80000000000000080493bd07401c3488bc1c3"):
        fail("D: c_str bytes")
    newb = b"\xff\x25\x00\x00\x00\x00" + b"<hook>" + b"\xcc\xcc"
    print("player_left lea  : 0x%x  %s" % (pl_lea, ins(pl_lea)))
    print("  switch call    : 0x%x  %s  (getkey 0x%x)" % (call + 31, ins(call + 31), getkey))
    print("  offsets        : list=+0x%x setup=[+0x%x]+0x%x id map=+0x%x local id=[[+0x%x]+0x%x]+0x%x" %
          (off_list, off_setup, off_setup_sub, off_map, *loc))
    print("sender           : 0x%x  first 16 bytes %s" % (send, prologue.hex(" ")))
    print("  patched to     : FF 25 00 00 00 00 <abs64 hook> CC CC  (16 bytes, original kept in a trampoline)")
    print("  builder        : 0x%x  command id slot 0x%x" % (inner, slot1))
    print("CCQ registration : 0x%x  CCQ_FACTION_SWITCH_HUMAN_TO_AI -> slot 0x%x, executor 0x%x" % (cl, slot1, handler))
    print("  -> world switch 0x%x -> set_idle_human 0x%x: %s at 0x%x" % (world, setidle, ins(idle_write), idle_write))
    print("  IsIdleHuman getter(s) comparing +0x1bc4 with 1: %s" % ", ".join("0x%x" % x for x in getter))
    print("string accessors : strlen 0x%x  c_str 0x%x  (via payload vtable 0x%x -> 0x%x -> 0x%x)" % (slen, cstr, pvt, vt0, ser))
    okD = True
    okD &= chk("D sender", send, REF_LEAVE_SEND)
    okD &= chk("D executor", handler, REF_LEAVE_HANDLER)
    okD &= chk("D set_idle", setidle, REF_LEAVE_SETIDLE)
    okD &= chk("D getkey", getkey, REF_LEAVE_GETKEY)
    okD &= chk("D id slot", slot1, REF_LEAVE_SLOT)
    if not okD or (off_list, off_setup, off_setup_sub, off_map, loc) != (0xe0b10, 0x750, 0x20, 0x281c, (0x74fe8, 0x58, 0x41c0)):
        fail("D: a resolved address/offset does not match the reference")
    print("PATCH D (leave) RESOLVED UNIQUELY AND MATCHES THE REFERENCE VAs")

    # ---------------------------------------------------------------------------------------------
    # Patch E (resumed-lobby seat picker): same locator chain as resolve_resume()
    print("--- patch E (resumed-lobby seats) ---")
    def global_count(sig):
        g = 0
        for (name, va, vsz, praw, sraw) in code_secs:
            g += len(list(re.finditer(sig_regex(sig), data[praw:praw + min(vsz, sraw)], re.DOTALL)))
        return g
    JOIN_SIG = ("49 8D BD ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8D 55 E8 49 8B CD E8 ?? ?? ?? ?? "
                "48 8B D0 48 8B CF E8 ?? ?? ?? ??")
    WRAP_CALL_SIG = "48 8D 8F ?? ?? ?? ?? 49 8B D6 4C 8D 45 E0 E8 ?? ?? ?? ??"
    PICK_SIG = ("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 "
                "44 0F B6 39 33 FF 4D 8B F0 48 8B F1 45 85 FF 74 3D 4C 8B 61 10 4C 8B 2A 8B EF 4D 39 2C FC 75 27 "
                "49 8B 4E 08 41 8B 5E 04 48 C1 E3 04 48 03 D9 48 C1 E5 04 48 03 6E 20 48 8B D3 4C 8B C5 "
                "E8 ?? ?? ?? ?? 48 3B C3 74 67 FF C7 41 3B FF 72 CA 48 8B 5E 20 8B 7E 1C 48 C1 E7 04 48 03 FB "
                "48 3B DF 74 2B 41 8B 6E 04 49 8B 76 08 48 C1 E5 04 48 03 EE 4C 8B C3 48 8B D5 48 8B CE "
                "E8 ?? ?? ?? ?? 48 3B C5 74 30 48 83 C3 10 48 3B DF 75 E4 E8 ?? ?? ?? ?? "
                "48 8B 5C 24 50 48 8B 6C 24 58 48 8B 74 24 60 48 83 C4 20 41 5F 41 5E 41 5D 41 5C 5F C3 "
                "48 8B C5 EB DE 48 8B C3 EB D9")
    RESUMED_SIG = ("40 53 48 83 EC 20 4C 8B C9 48 83 C1 78 E8 ?? ?? ?? ?? 33 DB 85 C0 75 10 49 8D 89 88 00 00 00 "
                   "E8 ?? ?? ?? ?? 84 C0 75 02 B3 01 8A C3 48 83 C4 20 5B C3")
    INDEX_SIG = ("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 59 1C 48 8B F9 48 8B 71 20 4C 8B C2 48 C1 E3 04 "
                 "48 8B CE 48 03 DE 48 8B D3 E8 ?? ?? ?? ?? 48 3B C3 74 17 48 2B C6 48 C1 F8 04 3B 47 2C 73 0B "
                 "8B C8 48 8B 47 30 8B 04 88 EB 03 83 C8 FF")
    SETF_RES_SIG = "48 8D 9D ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 ?? 49 8B D7 48 8B CB E8 ?? ?? ?? ??"
    js = wstr_va("recv MP_NETWORK_LOBBY_ID_ADD_FILLED_PLAYER_SLOT about %d")
    if not js: fail("E: ADD_FILLED_PLAYER_SLOT string not found")
    h = riplea_hits(js)
    if len(h) != 1: fail(f"E: lea->ADD_FILLED_PLAYER_SLOT string count {len(h)}")
    join_lea = h[0]
    h = sig_hits(JOIN_SIG, join_lea, join_lea + 0x200)
    if len(h) != 1: fail(f"E: resumed seating sequence in the join handler count {len(h)}")
    if global_count(JOIN_SIG) != 1: fail("E: resumed seating sequence not unique across non-writable code")
    j = h[0]
    r_off, resumed, wrapper, index_of = u32(j + 3), rel32(j + 10, 1, 5), rel32(j + 26, 1, 5), rel32(j + 37, 1, 5)
    h = sig_hits(WRAP_CALL_SIG, wrapper, wrapper + 0x110)
    if len(h) != 1 or u32(h[0] + 3) != r_off: fail("E: wrapper -> picker call / R offset")
    pick = rel32(h[0] + 14, 1, 5)
    if not sig_at(PICK_SIG, pick): fail("E: picker bytes")
    if global_count(PICK_SIG) != 1: fail("E: picker signature not unique")
    pro = rd(pick, 16)
    if pro != bytes.fromhex("48895c2408" "48896c2410" "4889742418" "57"): fail("E: picker prologue")
    for o in (0, 5, 10, 15):      # the 16 relocated bytes are position independent
        i = dis1(pick + o)
        if "rip" in i.op_str or i.mnemonic.startswith("j") or i.mnemonic == "call": fail("E: prologue not relocatable")
    if ins(pick + 15) != "push rdi" or dis1(pick + 15).size != 1: fail("E: prologue does not end with a 1-byte push rdi")
    find1, find2, empty = rel32(pick + 89, 1, 5), rel32(pick + 149, 1, 5), rel32(pick + 168, 1, 5)
    if find1 != find2: fail("E: picker uses two find helpers")
    # the layout the hook reads, decoded from the picker itself
    lay = [ins(pick + 0x1c), ins(pick + 0x2d), ins(pick + 0x40), ins(pick + 0x4f), ins(pick + 0x6e)]
    want = ["movzx r15d, byte ptr [rcx]", "mov r12, qword ptr [rcx + 0x10]", "mov ebx, dword ptr [r14 + 4]",
            "add rbp, qword ptr [rsi + 0x20]", "mov edi, dword ptr [rsi + 0x1c]"]
    if lay != want: fail("E: picker layout decode %s" % lay)
    if not sig_at(RESUMED_SIG, resumed): fail("E: is_resumed bytes")
    slen_e = rel32(resumed + 13, 1, 5)
    if not sig_at(INDEX_SIG, index_of): fail("E: index_of bytes")
    ss = wstr_va("faction not set: another player already has this faction (%s)")
    h = riplea_hits(ss) if ss else []
    if len(h) != 1: fail("E: lea->set_faction log string")
    sl = h[0]
    if rd(sl - 0xd, 4) != b"\x49\x8b\xcf\xe8": fail("E: c_str call before the set_faction log")
    cstr_e = rel32(sl - 0xd + 3, 1, 5)
    if (slen_e, cstr_e) != (slen, cstr): fail("E: string accessors differ from patch D's")
    h = sig_hits(SETF_RES_SIG, sl - 0x80, sl)
    if len(h) != 1 or u32(h[0] + 3) != r_off or rel32(h[0] + 10, 1, 5) != resumed or rel32(h[0] + 25, 1, 5) != index_of:
        fail("E: set_faction and the join handler disagree")
    print("join handler     : lea 0x%x, resumed seating at 0x%x (R = EMPIRE_MP+0x%x)" % (join_lea, j, r_off))
    print("  is_resumed 0x%x  wrapper 0x%x  index_of 0x%x" % (resumed, wrapper, index_of))
    print("picker           : 0x%x  first 16 bytes %s" % (pick, pro.hex(" ")))
    print("  layout         : %s" % "; ".join(lay))
    print("  find 0x%x  empty-string 0x%x  strlen 0x%x  c_str 0x%x (same as D)" % (find1, empty, slen_e, cstr_e))
    print("  patched to     : FF 25 00 00 00 00 <abs64 hook> CC CC  (16 bytes, original kept in a trampoline)")
    print("set_faction log  : lea 0x%x (cross-check of R offset, is_resumed, index_of OK)" % sl)
    okE = True
    okE &= chk("E picker", pick, 0x1421b36f8)
    okE &= chk("E wrapper", wrapper, 0x1421b3d7c)
    okE &= chk("E join seq", j, 0x1421cb508)
    okE &= chk("E is_resumed", resumed, 0x1421bc8e0)
    okE &= chk("E index_of", index_of, 0x1421b6c18)
    if not okE or r_off != 0xe0ba8:
        fail("E: a resolved address/offset does not match the reference")
    print("PATCH E (resume seats) RESOLVED UNIQUELY AND MATCHES THE REFERENCE VAs")

    # ---------------------------------------------------------------------------------------------
    # Patch F (end-turn consensus): same locator chain as resolve_endturn()
    print("--- patch F (end-turn consensus) ---")
    EXEC_SIG = ("40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 71 48 8B 83 60 2F 00 00 4C 8B 90 90 00 00 00 "
                "4C 8B 88 C8 01 00 00 49 8B 82 B0 5B 00 00 48 8B 88 A8 00 00 00 48 8B 81 70 01 00 00 8B 89 6C 01 00 00 "
                "4C 8D 04 C8 49 3B C0 74 1C 48 8B 08 48 8B 11 80 7A 10 00 75 06 48 83 C0 08 EB E9 41 80 B9 C4 1B 00 00 02 "
                "72 1C 45 33 C0 49 8B D1 49 8B CA E8 ?? ?? ?? ?? 84 C0 74 0A 33 D2 48 8B CB E8 ?? ?? ?? ?? 48 83 C4 20 5B C3")
    ISSUER_SIG = ("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 48 8B D9 40 8A F2 48 8D 4C 24 38 E8 ?? ?? ?? ?? "
                  "4C 8B 93 60 2F 00 00 49 8B BA 90 00 00 00 4D 8B 9A C8 01 00 00 49 8B D3 48 8B 8F A0 6C 00 00 "
                  "48 8B 89 10 03 00 00 E8 ?? ?? ?? ?? 84 C0")
    CANCEL_SIG = ("48 83 EC 38 4C 8B 89 60 2F 00 00 49 8B 81 E0 00 00 00 80 78 4C 00 74 5C 49 8B 81 90 00 00 00 "
                  "4D 8B 91 C8 01 00 00 4C 89 54 24 40 48 8B 90 A0 6C 00 00 48 8B 82 10 03 00 00 48 8D 54 24 40 "
                  "48 8B 88 20 01 00 00 48 83 C1 10 E8 ?? ?? ?? ?? 84 C0 74 22 48 8D 05 ?? ?? ?? ?? 4C 89 54 24 28 "
                  "48 89 44 24 20 48 8D 54 24 20 49 8B 89 98 00 00 00 E8 ?? ?? ?? ?? 48 83 C4 38 C3")
    FIND_SIG = ("4C 8B 41 08 8B 41 04 49 8D 0C C0 4C 3B C1 74 14 48 8B 02 49 39 00 74 09 49 83 C0 08 4C 3B C1 75 F2 "
                "4C 3B C1 0F 95 C0 C3")
    SITE_RAX = "48 8B 88 20 01 00 00 48 83 C1 10 E8 ?? ?? ?? ??"
    SITE_RCX = "48 8B 89 20 01 00 00 48 83 C1 10 E8 ?? ?? ?? ??"
    SITE_R11 = ("4C 8B 99 10 03 00 00 49 8B CB E8 ?? ?? ?? ?? 84 C0 74 ?? 49 8B 8B 20 01 00 00 48 8D 54 24 ?? "
                "48 83 C1 10 4C 89 54 24 ?? E8 ?? ?? ?? ??")
    CCQ_CALL = "45 33 C0 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 44 8A 44 24 30 48 8B D3 48 8B CE E8 ?? ?? ?? ??"
    def cstr_va(s):
        hits = [m.start() for m in re.finditer(re.escape(s) + b"\0", xdata)]
        if not hits: fail("F: string %r not found" % s)
        return imgbase + xva + hits[0]
    def lea_rdx(target):
        h = [x for x in riplea_hits(target) if rd(x, 3) == b"\x48\x8d\x15"]
        if len(h) != 1: fail("F: lea rdx->0x%x count %d" % (target, len(h)))
        return h[0]
    if global_count(EXEC_SIG) != 1: fail("F: executor signature not unique")
    exec_ = sig_hits(EXEC_SIG, imgbase + uva, imgbase + uva + len(code))[0]
    can_end, issuer = rel32(exec_ + 112, 1, 5), rel32(exec_ + 126, 1, 5)
    if not sig_at(ISSUER_SIG, issuer): fail("F: issuer bytes")
    iss_pro = rd(issuer, 18)
    if iss_pro != bytes.fromhex("48895c2408" "4889742410" "57" "4883ec60" "488bd9"): fail("F: issuer prologue")
    is_turn = rel32(issuer + 69, 1, 5)
    cl = lea_rdx(cstr_va(b"CCQ_END_TURN"))
    if rd(cl - 10, 3) != b"\x4c\x8d\x0d" or rd(cl - 3, 3) != b"\x45\x33\xc0": fail("F: CCQ registration shape")
    ccq_exec = cl - 3 + i32(cl - 7)
    h = sig_hits(CCQ_CALL, ccq_exec, ccq_exec + 0xb0)
    if len(h) != 1 or rel32(h[0] + 9, 1, 5) != can_end: fail("F: executors disagree on can_end_turn")
    rl = lea_rdx(cstr_va(b"IsFactionReadyToEndTurn"))
    if not sig_at("48 8D 05 ?? ?? ?? ?? 45 33 F6", rl - 10): fail("F: getter registration shape")
    getter = rl - 3 + i32(rl - 7)
    h = sig_hits(SITE_RCX, getter, getter + 0x60)
    if len(h) != 1: fail("F: reader in the getter")
    site_get = h[0] + 11
    h = [x for x in riplea_hits(cstr_va(b"EndTurnButton")) if rd(x, 3) == b"\x48\x8d\x15"
         and sig_at("48 8B C8 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03", x + 7)]
    if len(h) != 1: fail("F: EndTurnButton factory count %d" % len(h))
    bl = h[0]
    vt = bl + 15 + 7 + i32(bl + 15 + 3)
    upd = struct.unpack_from("<Q", xdata, vt + 4 * 8 - imgbase - xva)[0]
    clk = struct.unpack_from("<Q", xdata, vt + 13 * 8 - imgbase - xva)[0]
    h = sig_hits(SITE_RAX, clk, clk + 0x100)
    if len(h) != 1: fail("F: reader in the click handler")
    site_clk = h[0] + 11
    if not sig_at("84 C0 74 ?? 48 8B CB E8 ?? ?? ?? ??", site_clk + 5): fail("F: cancel call after the click reader")
    cancel = rel32(site_clk + 5 + 7, 1, 5)
    h = sig_hits(SITE_RAX, upd, upd + 0x110)
    if len(h) != 1: fail("F: reader in the state updater")
    site_upd = h[0] + 11
    if not sig_at(CANCEL_SIG, cancel): fail("F: cancel issuer bytes")
    can_pro = rd(cancel, 18)
    if can_pro != bytes.fromhex("4883ec38" "4c8b89602f0000" "498b81e0000000"): fail("F: cancel prologue")
    find = rel32(cancel + 73, 1, 5)
    if not sig_at(FIND_SIG, find): fail("F: find bytes")
    h = sig_hits(SITE_R11, imgbase + uva, imgbase + uva + len(code))
    if len(h) != 2 or global_count(SITE_R11) != 2: fail("F: turn-gated readers count %d" % len(h))
    sites = [site_get, site_clk, site_upd, h[0] + 40, h[1] + 40]
    for s in sites:
        if rd(s, 1) != b"\xe8" or rel32(s, 1, 5) != find: fail("F: site 0x%x is not call find" % s)
    # every call of find whose 40-byte window reads +0x120 is either a site, the cancel issuer or model-side
    other = []
    for m in re.finditer(rb"\xe8", code):
        i = m.start()
        if i + 5 > len(code) or i < 40: continue
        va = imgbase + uva + i
        if va + 5 + struct.unpack_from("<i", code, i + 1)[0] == find and b"\x20\x01\x00\x00" in code[i - 40:i]:
            other.append(va)
    unknown = [x for x in other if x not in sites + [cancel + 73, 0x141f91dc6, 0x142462d0e, 0x1426a6979, 0x1426a7cc0, 0x1426ba884]]
    if unknown: fail("F: unclassified ready reader(s): %s" % [hex(x) for x in unknown])
    for s, cover in ((issuer, (0, 5, 10, 11, 15)), (cancel, (0, 4, 11))):   # the 18 relocated bytes are position independent
        for o in cover:
            i = dis1(s + o)
            if "rip" in i.op_str or i.mnemonic.startswith("j") or i.mnemonic == "call": fail("F: prologue not relocatable")
    # the thunk's contract: find must not clobber r9-r11 (its callers keep them live)
    for i in md.disasm(rd(find, len(FIND_SIG.split())), find):
        if re.match(r"r(9|10|11)", i.op_str): fail("F: find uses r9-r11: " + i.op_str)
    thunk = (b"\x48\xb8" + b"\x11" * 8 + bytes.fromhex("488b00 4885c0 7408 483b02 7503 b001 c3 ff2500000000".replace(" ", "")) + b"\x22" * 8)
    dec = [(i.address, i.mnemonic, i.op_str) for i in md.disasm(thunk[:32], 0)]
    if [d[1] for d in dec] != ["movabs", "mov", "test", "je", "cmp", "jne", "mov", "ret", "jmp"]: fail("F: thunk decode %s" % dec)
    if dec[3][2] != hex(dec[8][0]) or dec[5][2] != hex(dec[8][0]): fail("F: thunk branches do not land on the jmp")
    print("executor         : 0x%x  can_end_turn 0x%x  issuer 0x%x  is_factions_turn 0x%x" % (exec_, can_end, issuer, is_turn))
    print("cancel issuer    : 0x%x  find 0x%x  CCQ_END_TURN executor 0x%x" % (cancel, find, ccq_exec))
    print("reader sites     : %s" % " ".join("0x%x" % s for s in sites))
    print("  thunk decodes  : %s" % "; ".join("%s %s" % (d[1], d[2]) for d in dec))
    okF = True
    okF &= chk("F executor", exec_, 0x141fd8dac)
    okF &= chk("F issuer", issuer, 0x141e6ce14)
    okF &= chk("F cancel", cancel, 0x141e65e50)
    okF &= chk("F find", find, 0x14052b770)
    okF &= chk("F CCQ executor", ccq_exec, 0x1428b2760)
    if not okF or sorted(sites) != sorted([0x142059640, 0x1421540ce, 0x14215a124, 0x14216a578, 0x141e652b2]):
        fail("F: a resolved address does not match the reference")
    print("PATCH F (end-turn consensus) RESOLVED UNIQUELY AND MATCHES THE REFERENCE VAs")

    # ---------------------------------------------------------------------------------------------
    # Patch G (battle gift system): same locator chain as resolve_gift()
    print("--- patch G (battle gift system) ---")
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native", "archon_native.c"), encoding="utf-8").read()
    def csig(name):                                   # the DLL's own signature text, so the two can never drift
        m = re.search(r"#define %s\s+((?:\"[^\"]*\"\s*\\?\s*)+)" % name, src)
        if not m: fail("G: %s not found in archon_native.c" % name)
        return "".join(re.findall(r"\"([^\"]*)\"", m.group(1)))
    def cval(name):
        return int(re.search(r"#define %s\s+(0x[0-9A-Fa-f]+)" % name, src).group(1), 16)
    lo, hi = imgbase + uva, imgbase + uva + len(code)
    hit = {}
    for n in ("ORIG", "OWNED", "GIFT", "RECALL", "SEND", "LIST"):
        sig = csig("GF_%s_SIG" % n)
        h = sig_hits(sig, lo, hi)
        if len(h) != 1 or global_count(sig) != 1: fail("G: %s signature not unique" % n)
        hit[n] = h[0]
    env_global = hit["ORIG"] + 7 + i32(hit["ORIG"] + 3)
    s = {"orig_army": hit["ORIG"] + cval("GF_ORIG_ARMY_E8"), "orig_first": hit["ORIG"] + cval("GF_ORIG_FIRST_E8"),
         "owned_army": hit["OWNED"] + cval("GF_OWNED_ARMY_E8"), "owned_first": hit["OWNED"] + cval("GF_OWNED_FIRST_E8"),
         "gift": hit["GIFT"] + cval("GF_GIFT_E8"), "recall": hit["RECALL"] + cval("GF_RECALL_E8"),
         "send": hit["SEND"] + cval("GF_SEND_E8"), "list": hit["LIST"] + cval("GF_LIST_E8")}
    for k, v in s.items():
        if rd(v, 1) != b"\xe8": fail("G: site %s is not a call" % k)
    t = {k: rel32(v, 1, 5) for k, v in s.items()}
    if t["orig_army"] != t["owned_army"] or t["orig_first"] != t["owned_first"]: fail("G: the owner functions disagree")
    if rel32(t["gift"] + cval("GF_SEL_E8"), 1, 5) != t["recall"]: fail("G: GiftUnits does not reach gift_units")
    if not 0 <= s["send"] - t["recall"] < cval("GF_UNITS_SPAN"): fail("G: the send is not in gift_units")
    if rd(s["list"] + 0x1B, 5) != bytes.fromhex("453bf77453"): fail("G: the popup walk no longer skips r15d")
    # no reader of a volatile register after any retargeted call: C stands in for the callee
    for k, v in s.items():
        i = dis1(v + 5)
        src_op = i.op_str.split(",")[-1]
        if re.search(r"\b(rcx|ecx|rdx|edx|r8|r8d|r9|r9d|r10|r11)\b", src_op): fail("G: code after %s reads a volatile register: %s %s" % (k, i.mnemonic, i.op_str))
        if k in ("gift", "recall", "send", "list") and re.search(r"\b(rax|eax|al)\b", src_op): fail("G: code after %s reads a result: %s %s" % (k, i.mnemonic, i.op_str))
    want = {"orig_army": (0x141e8f9e8, 0x141e61024), "orig_first": (0x141e8fa36, 0x1421b518c), "owned_army": (0x141e84232, 0x141e61024),
            "owned_first": (0x141e84296, 0x1421b518c), "gift": (0x141bf2b32, 0x141bf281c), "recall": (0x141e7f42c, 0x141bf2854),
            "send": (0x141bf29cd, 0x1421b72cc), "list": (0x141e7c405, 0x1421b4ab8)}
    okG = chk("G env", env_global, 0x144480d10)
    for k, (site, target) in want.items():
        okG &= chk("G %s site" % k, s[k], site)
        okG &= chk("G %s target" % k, t[k], target)
    if not okG: fail("G: a resolved address does not match the reference")
    # the near-page stubs, decoded
    fn = 0x7000000000
    list_stub = bytes.fromhex("4883ec28" "ff1514000000" "4883c428" "85c0" "740b" "4183cfff" "c74548ffffffff" "c3")
    d = [(x.mnemonic, x.op_str) for x in md.disasm(list_stub, 0x1000)]
    if d != [("sub", "rsp, 0x28"), ("call", "qword ptr [rip + 0x14]"), ("add", "rsp, 0x28"), ("test", "eax, eax"), ("je", "0x101d"),
             ("or", "r15d, 0xffffffff"), ("mov", "dword ptr [rbp + 0x48], 0xffffffff"), ("ret", "")]: fail("G: list stub: %s" % d)
    if len(list_stub) != 30 or 0x1000 + 10 + 0x14 != 0x1000 + 30: fail("G: list stub layout")
    cap = bytes.fromhex("49ba1122334455667788" "49890a")
    if [(x.mnemonic, x.op_str) for x in md.disasm(cap, 0)] != [("movabs", "r10, 0x8877665544332211"), ("mov", "qword ptr [r10], rcx")]: fail("G: capture stub")
    print("PATCH G (battle gift system) RESOLVED UNIQUELY AND MATCHES THE REFERENCE VAs")
    sys.exit(0)

if __name__ == "__main__":
    main()
