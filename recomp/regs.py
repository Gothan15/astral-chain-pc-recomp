"""Function-local general registers for recomp/gen.py.

The lifter reads and writes the guest's X0-X30 and SP in the Cpu (c->x[n], c->sp), so every guest instruction loads
and stores memory, and the C compiler cannot keep a register in a host register across a guest store: guest stores go
through pointers that may alias the Cpu. A generated function instead copies the LOCALS registers it uses most into
locals at its entry (xN, sp_), works on those, and syncs with the Cpu only where something else reads or writes them:
before a call, an SVC, a trap, a return or a tail exit it stores the registers it changed since the last sync (spill),
and after an SVC it loads them all again (reload). The other registers stay in the Cpu.

After a call it reloads only X0-X18 and X30: AAPCS64 has the callee preserve X19-X29 and SP, so their locals still
hold what the Cpu holds. What would break that does not occur in Astral Chain: main and subsdk0 import neither longjmp nor
the C++ unwinder, sdk's longjmp, __cxa_throw and _Unwind_* have no direct callers, and the runtime writes no register
above X3 of a running thread's Cpu except X30, which rt_call restores (runtime/hostfn.c). Guest fibers each run on a host fiber of their own with their own Cpu (runtime/kernel.c),
and ExitThread leaves by longjmp without returning. gen.py --no-callee-saved-trust reloads those registers too.

Only plain assignments write registers in the lifted C (`c->x[n] = ...`, `c->sp = ...`), and no code takes a
register's address, so the rewrite is textual. A register counts as dirty from a write until the next spill on some
path: plan() runs that forward over the function's control flow, so a call spills only what changed since the
previous call or the entry. Spilling a register that is not dirty is harmless (the local equals the Cpu), so where a
case is unclear the plan errs on spilling more.
"""
import re
from collections import Counter

import ipa

REG = re.compile(r'c->x\[(\d+)\]|c->sp\b')
WRITE = re.compile(r'c->(?:x\[(\d+)\]|(sp))\s*=(?!=)')
# Lifted instructions that hand the Cpu to the runtime: SVCs return, BRK and undefined words do not.
ESCAPE = re.compile(r'\brt_(?:svc|brk|udf)\(c\b')
SP = 31   # index of SP in the register sets
# Markers gen.py puts where the Cpu must be current, resolved by Plan.resolve(): spill before; reload all, or reload
# what a call may change, after.
SPILL, RELOAD, RELOAD_CALL = '\x00S', '\x00R', '\x00C'
CALLEE_SAVED = frozenset(range(19, 30)) | {SP}
NEVER_LOCAL = frozenset({18})   # the platform register stays in the Cpu
# gen.py --abi-poison: the value a register left out of a sync gets in its place (ORed with the register's number).
# No guest address: a load or store through it faults.
POISON = 0xdead0000dead0000
# Locals per function: the most used registers. More cost more than they save, as clang then spills them to the stack
# and the syncs stay (300 hot Astral Chain functions: fewest instructions and memory operands at 4-6).
LOCALS = 6


def name(r):
    return 'sp_' if r == SP else f'x{r}'


def cpu(r):
    return 'c->sp' if r == SP else f'c->x[{r}]'


def refs(text):
    """Registers text reads or writes, once per occurrence."""
    return [SP if m.group(0) == 'c->sp' else int(m.group(1)) for m in REG.finditer(text)]


def written(text):
    return {SP if m.group(2) else int(m.group(1)) for m in WRITE.finditer(text)}


def read(text):
    """Registers text reads: those with more occurrences than assignments. An instruction that assigns a register and
    reads it only afterwards counts as reading it, which errs on keeping the register alive."""
    occ = Counter(refs(text))
    wr = Counter(SP if m.group(2) else int(m.group(1)) for m in WRITE.finditer(text))
    return {r for r, k in occ.items() if k > wr.get(r, 0)}


class Plan:
    def __init__(self, regs, spill, kept, reloads=None, poison=None):
        self.regs = regs     # sorted registers the function keeps in locals
        self.local = set(regs)
        self.spill = spill   # per instruction: registers to store at its sync points
        self.kept = kept     # registers a call leaves as they were (CALLEE_SAVED, or none)
        self.reloads = reloads     # --abi-trust: {instruction: registers loaded again after its call} (plan_ipa())
        self.poison = poison       # --abi-poison: {instruction: (registers whose Cpu copy, registers whose local
                                   # gets a trap value at its sync)}

    def decl(self):
        """Locals at the top of the function, loaded from the Cpu."""
        return 'uint64_t ' + ', '.join(f'{name(r)} = {cpu(r)}' for r in self.regs) + ';' if self.regs else ''

    def resolve(self, i, text):
        """text of instruction i with its registers on the locals and its sync markers expanded."""
        def local(m):
            r = SP if m.group(0) == 'c->sp' else int(m.group(1))
            return name(r) if r in self.local else m.group(0)

        text = REG.sub(local, text)
        spill = ''.join(f'{cpu(r)} = {name(r)}; ' for r in sorted(self.spill[i]))
        reload = ''.join(f' {name(r)} = {cpu(r)};' for r in self.regs)
        if self.reloads is not None and i in self.reloads:
            reload_call = ''.join(f' {name(r)} = {cpu(r)};' for r in self.regs if r in self.reloads[i])
        else:
            reload_call = ''.join(f' {name(r)} = {cpu(r)};' for r in self.regs if r not in self.kept)
        if self.poison and i in self.poison:
            in_cpu, in_local = self.poison[i]
            spill += ''.join(f'{cpu(r)} = {POISON | r:#x}ull; ' for r in sorted(in_cpu))
            reload_call += ''.join(f' {name(r)} = {POISON | r:#x}ull;' for r in sorted(in_local))
        return text.replace(SPILL, spill).replace(RELOAD, reload).replace(RELOAD_CALL, reload_call)


def plan(texts, succ, clean, n_locals=LOCALS, kept=CALLEE_SAVED):
    """texts[i]: instruction i's C with markers; succ[i]: successor indices inside the function; clean: instructions
    after which every local equals the Cpu (a call or an SVC that spills and reloads on every path through it);
    n_locals: how many registers to keep in locals; kept: registers a call does not reload."""
    n = len(texts)
    uses = Counter(r for t in texts for r in refs(t))
    regs = {r for r, _ in sorted(uses.items(), key=lambda kv: (-kv[1], kv[0]))[:n_locals]}
    writes = [written(t) & regs for t in texts]
    dirty = [set() for _ in range(n)]
    seen = [False] * n
    work = [0] if n else []
    if n:
        seen[0] = True
    while work:
        i = work.pop()
        out = set() if i in clean else dirty[i] | writes[i]
        for s in succ[i]:
            if not seen[s] or not out <= dirty[s]:
                seen[s] = True
                dirty[s] |= out
                work.append(s)
    every = set().union(*writes) if writes else set()
    for i in range(n):
        if not seen[i]:   # not reached from the entry in the plan's flow: anything may be dirty
            dirty[i] = every
    # A sync point stores what was dirty on entry to its instruction and what the instruction wrote before it
    # (c->x[30] of a call): all the instruction's writes, which is at most a little more.
    return Plan(sorted(regs), [dirty[i] | writes[i] if SPILL in texts[i] else set() for i in range(n)], kept)



def plan_ipa(texts, succ, S, real_out, T, poison=False):
    """The plan of gen.py --abi-trust: every register the function uses is a local (X18 excepted), and a sync point
    stores what the other side reads, from recomp/ipa.py: S is the function's summary, real_out the registers known
    code reads after each instruction or a caller reads after the return (ipa.live()), T the tables of every
    function. What must be current in the Cpu at a sync point (need):

    - a direct call: the registers the callee reads (T.inn) and the ones read after the call, unless the callee
      always writes them;
    - an indirect call or jump: the registers an unknown target is taken to read (ipa.IND_USE), and what is read
      after it;
    - a return or a jump to another function: the registers a caller reads (T.out), the callee-saved ones, which
      leave as they came, and what the target reads;
    - an SVC and a patched site: every register (all the locals are loaded again afterwards).

    A sync point stores the changed registers it needs. An indirect call, a jump out and a trap also store the other
    changed registers, so an unknown target finds what the function wrote, as long as their locals hold them on
    every path (see stale below).

    After a call the function loads again the registers its own code reads before writing them. The others a call
    may change are stale: the local no longer holds the register, the Cpu does. A stale local is never stored, or a
    later sync point would put an old value over the Cpu's: a store of a needed register counts as a read, so a
    register written on one path to such a sync point and left alone on another is loaded again after the call, and
    a register that is not needed is left out of the store where it may be stale. A changed register a call does not
    store stays changed only if the callee preserves it (T.kept), and a later sync point that needs it stores it.

    poison: at each call and return, the Cpu copy of every changed register that is not stored, and after a call the
    local of every register that is not loaded again, get POISON, so a read the analysis missed faults or shows
    instead of using a stale value."""
    n = len(texts)
    regs = {r for t in texts for r in refs(t)} - NEVER_LOCAL
    rmask = sum(1 << r for r in regs)
    writes = [sum(1 << r for r in written(t) & regs) for t in texts]
    kept, ev, patched = T.kept, S.ev, S.patched
    out_f = T.out(S.entry) | kept
    # a call to a known function, to an unknown one; a return; a jump out; an SVC or a patched site; a trap
    DIRECT, INDIRECT, RET, EXIT, FULL, TRAP = range(1, 7)
    kind, need = [0] * n, [0] * n
    for i in range(n):
        evs = ev.get(i)
        if not evs:
            if SPILL in texts[i]:
                kind[i], need[i] = FULL, ipa.ALL
            continue
        k, t, how = evs[0]
        if i in patched or k == ipa.SVC:
            kind[i], need[i] = FULL, ipa.ALL
        elif k == ipa.CALL:
            if t is None:
                kind[i], need[i] = INDIRECT, ipa.IND_USE | real_out[i]
            else:
                kind[i] = DIRECT
                need[i] = T.inn(t) | (0 if T.NORET[t] else real_out[i] & ~kept & ~T.MUST[t])
        elif k == ipa.RET:
            kind[i], need[i] = RET, out_f
        elif k == ipa.TAIL:
            kind[i] = EXIT
            need[i] = out_f | (ipa.IND_USE if t is None or how == ipa.PLT else 0) | (T.inn(t) if t is not None else 0)
        else:
            kind[i] = TRAP

    def after(i, d):
        """The changed registers after instruction i, entered with d (its own writes included)."""
        k = kind[i]
        if k == DIRECT:
            return d & ~need[i] & kept
        if k == INDIRECT or (k == FULL and i not in patched):
            return 0
        return d   # the fall-through of a conditional jump out, a patched site (as plan()), a trap

    dirty = [0] * n
    seen = [False] * n
    work = [0] if n else []
    if n:
        seen[0] = True
    while work:
        i = work.pop()
        out = after(i, dirty[i] | writes[i])
        for s in succ[i]:
            if not seen[s] or out & ~dirty[s]:
                seen[s] = True
                dirty[s] |= out
                work.append(s)
    every = 0
    for w in writes:
        every |= w
    entered = [dirty[i] if seen[i] else every for i in range(n)]   # changed before the instruction
    dirty = [entered[i] | writes[i] for i in range(n)]               # and at its sync point
    # The locals the function reads: by its code, and by the stores of the registers a sync point needs. A register
    # the instruction writes itself before its sync point (X30 of a call) is not read from before it; at a patched
    # site it may be, the write being in one alternative only.
    pred = [[] for _ in range(n)]
    for i in range(n):
        for s in succ[i]:
            if s < n:
                pred[s].append(i)
    calls = [k in (DIRECT, INDIRECT) for k in kind]
    use = [(S.use[i] | ((dirty[i] if i in patched else entered[i] & ~writes[i]) & need[i])) & rmask for i in range(n)]
    kill = [rmask & ~kept | S.dfn[i] if calls[i] else rmask if kind[i] == FULL and i not in patched else S.dfn[i]
            for i in range(n)]
    lin, lout = [0] * n, [0] * n
    work = list(range(n))
    while work:
        i = work.pop()
        o = 0
        for s in succ[i]:
            if s < n:
                o |= lin[s]
        lout[i] = o
        b = use[i] | (o & ~kill[i])
        if b != lin[i]:
            lin[i] = b
            work.extend(pred[i])
    reload = [lout[i] & ~kept & rmask if calls[i] else 0 for i in range(n)]
    # stale: registers whose local may not hold them, on some path
    stale = [0] * n
    work = list(range(n))
    while work:
        i = work.pop()
        if calls[i]:
            out = rmask & ~kept & ~reload[i]
        elif kind[i] == FULL and i not in patched:
            out = 0
        else:
            out = stale[i] & ~writes[i]
        for s in succ[i]:
            if s < n and out & ~stale[s]:
                stale[s] |= out
                work.append(s)
    spill, reloads, traps = [], {}, {}
    for i in range(n):
        d, k = dirty[i], kind[i]
        stored = d & need[i]
        if k in (INDIRECT, EXIT, TRAP):
            stored |= d & ~(stale[i] & ~writes[i])
        spill.append({r for r in regs if stored >> r & 1})
        if calls[i]:
            reloads[i] = {r for r in regs if reload[i] >> r & 1}
            if poison:
                traps[i] = ({r for r in regs if (d & ~stored & ~kept) >> r & 1},
                            {r for r in regs if (rmask & ~kept & ~reload[i]) >> r & 1})
        elif poison and k == RET:
            traps[i] = ({r for r in regs if (d & ~stored) >> r & 1}, set())
    return Plan(sorted(regs), spill, {r for r in range(32) if kept >> r & 1}, reloads, traps)
