"""Check the V-locals syncs of lifted chunks (recomp/vlocals.py) on the generator's real output.

Usage: vlocals_check.py [--gen generated/v101] [--trust]

For every function of a chunk built with RT_VLOCALS whose VMASK is not 0:
- every call that passes c and may reach c->v (a guest function, rt_dispatch, rt_svc/udf/brk/spin_gate, and the host
  helpers of the code patches, `@fn` / `@fnz` in recomp/patches-v101.txt) is either inside a return block that stores
  the written locals first, or wrapped as `{ <stores> <call> <reloads> vd_ = 0; }`: the stores are the function's
  written local registers, the reloads every local register (V8-V15 excepted with --trust);
- the stores are the same set at every call, return and the end of the function, and hold every local register the
  function writes;
- no other call passes c, except the VCTX helpers and the value helpers of recomp/vlocals.py (SAFE).
The patch helpers in runtime/frame_rate.c and runtime/resolution.c read and write c->v (rt_fps_cam_s0 reads s0 and s10
and writes s8, rt_fov_s0 rewrites s0, rt_wide_anchor runs guest code), and tests/diff/func.py never emits a patched
site, so this is the check that covers them. Prints the counts and every violation; exit status 1 on a violation.
"""
import argparse
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
import vlocals  # noqa: E402

FN = re.compile(r'^(?:static )?void (?:f|fb)_[0-9a-f]+\(Cpu \*c\) \{$', re.M)
HEAD = re.compile(r'\s*const unsigned VMASK = (0x[0-9a-f]+|0)u?;')
STORES = re.compile(r'(?:VSYNC_OUT_D\((\d+)\); )*$')
SYNC_OUT = re.compile(r'VSYNC_OUT_D\((\d+)\);')
RELOADS = re.compile(r' ((?:VSYNC_IN\(\d+\); )*)vd_ = 0; \}')


def functions(text):
    """(name line, body) of each function of a chunk"""
    for m in FN.finditer(text):
        d, i = 1, m.end()
        while d:
            d += {'{': 1, '}': -1}.get(text[i], 0)
            i += 1
        yield m.group(0), text[m.end():i - 1]


def check(body, trust):
    """(sync points checked, list of problems) of one function body"""
    h = HEAD.match(body)
    if not h or int(h.group(1), 16) == 0:
        return 0, []
    mask = int(h.group(1), 16)
    local = {r for r in range(32) if mask >> r & 1}
    _, written = vlocals.registers(body)
    want_store = written & local
    want_reload = local - (vlocals.CALLEE_SAVED if trust else set())
    problems, n = [], 0
    rets = [m.span() for m in vlocals.RET.finditer(body)]
    stores_seen = set()

    def stores_before(pos):
        """registers stored right before pos, after the '{ ' that opens the sync block"""
        head = body[max(0, pos - 800):pos]   # 32 stores at most
        m = STORES.search(head)
        if not head[:m.start()].endswith('{ '):
            return None
        return frozenset(int(x) for x in SYNC_OUT.findall(m.group(0)))

    for s, e in rets:
        n += 1
        st = stores_before(s)
        if st is None:
            problems.append(f'return without stores: {body[s:e]!r}')
            continue
        stores_seen.add(st)
    for m in vlocals.PASSES_C.finditer(body):
        name = m.group(1)
        if name in vlocals.HELPERS or vlocals.SAFE.fullmatch(name):
            continue
        if any(s <= m.start() < e for s, e in rets):
            continue
        # a patch helper writing flags (`@fnz`): the sync block holds its assignment too
        a = re.search(r'(?:F\.nz|c->nzcv) = $', body[max(0, m.start() - 16):m.start()])
        start = m.start() - (len(a.group(0)) if a else 0)
        call = vlocals.CALL.match(body, start)
        if not call:
            problems.append(f'call of {name} that is no sync point')
            continue
        n += 1
        st = stores_before(start)
        r = RELOADS.match(body, call.end())
        if st is None or not r:
            problems.append(f'call without its syncs: {body[max(0, start - 60):call.end() + 60]!r}')
            continue
        stores_seen.add(st)
        reloads = {int(x) for x in re.findall(r'VSYNC_IN\((\d+)\);', r.group(1))}
        if reloads != want_reload:
            problems.append(f'{name} reloads {sorted(reloads)}, not {sorted(want_reload)}')
    tail = re.search(r'((?:VSYNC_OUT_D\(\d+\); ?)*)\s*$', body)
    if want_store:
        stores_seen.add(frozenset(int(x) for x in SYNC_OUT.findall(tail.group(1))))
    for st in stores_seen:
        if st != want_store:
            problems.append(f'stores {sorted(st)}, not the written locals {sorted(want_store)}')
    return n, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--gen', type=Path, default=ROOT / 'generated' / 'v101')
    ap.add_argument('--trust', action='store_true', help='the output was lifted with gen.py --v-trust')
    args = ap.parse_args()
    counts, bad = Counter(), []
    for path in sorted(args.gen.glob('*/*_[0-9][0-9][0-9][0-9].c')):
        text = path.read_text(encoding='utf-8')
        if '#define RT_VLOCALS 1' not in text:
            counts['chunks without RT_VLOCALS'] += 1
            continue
        counts['chunks'] += 1
        for name, body in functions(text):
            counts['functions'] += 1
            n, problems = check(body, args.trust)
            counts['local functions'] += bool(n) or bool(problems)
            counts['sync points'] += n
            counts['patch helper calls'] += sum(1 for m in vlocals.HOST_CALL.finditer(body)) if n else 0
            bad += [f'{path.name} {name[:-2]}: {p}' for p in problems]
    print(', '.join(f'{v} {k}' for k, v in counts.items()))
    for b in bad[:50]:
        print(b)
    print(f'{len(bad)} violations')
    return 1 if bad or not counts['chunks'] else 0


if __name__ == '__main__':
    sys.exit(main())
