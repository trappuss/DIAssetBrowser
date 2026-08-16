#!/usr/bin/env python3
# DI FX binding cracker - process-dump scanner (v2, fast + progress).
#
# The FX-attach FORMAT is cracked and byte-verified (FX_RESEARCH.md):
#   "EffectCom/<ParticleSystem>:<AttachBone>:<StartTime>:<flags8>[:r..][:p..][:s..]"
# 48,163 such records live in .graph files (skills, cutscenes, NPC auras). A
# WORN cosmetic's effect is applied from the encrypted client item table at
# equip time, so no .graph names it, and - measured on a real 3.97 GB dump -
# the runtime does NOT keep the serialized string form for worn cosmetics in
# memory either; it binds via handles. So this scan reliably recovers:
#   * every effect NAME resident in the dump         (--grep)
#   * effect refs / bones sitting near a token        (--near)
#   * every serialized attach record present          (always)
# which is exactly the confirmed binding for graph-bound effects, and residency
# proof + candidate hardpoints for worn cosmetics.
#
# Nothing is executed and nothing is written back; this only reads bytes.
# Usage:  python di_fx_dumpscan.py <dump> [--near TOKEN ...] [--grep REGEX ...]
#                                         [--window N] [--max N]
# The .bat wrapper fills in the target tokens for you.
#
# v2 speed: marks (effect refs / bones / attach records) are located ONCE per
# chunk, then near-token windows are resolved by binary-searching those mark
# positions instead of re-running regexes inside every window. On the 3.97 GB
# reference dump this drops an ~1 hour scan to a couple of minutes. A live
# percentage prints to stderr so you can see it working.

import argparse, re, sys, os, bisect, time

# Strict attach record (the graph/serialized form).
ATTACH = re.compile(
    rb'EffectCom/([A-Za-z0-9_]{2,64}):'
    rb'([ A-Za-z0-9_]{1,40}):'
    rb'(-?[0-9]+(?:\.[0-9]+)?):'
    rb'([0-9]{8})'
    rb'((?::[rpsRPS][-0-9.,]+){0,3})')

# Any effect-ish asset reference. Bounded quantifiers only (no lazy/backtracking
# blowups on multi-GB data).
EFFECTREF = re.compile(rb'(?:EffectCom|EffectModel)/[A-Za-z0-9_]{2,80}|fx_[A-Za-z0-9_]{2,80}')
# Bone-name-ish (for confirming an attach target near a hit).
BONE = re.compile(rb'Bip001[ A-Za-z0-9_]{0,24}|HP_[A-Za-z0-9_]{1,40}|Scene Root|WorldOrigin|Bone[0-9]{1,4}')

CHUNK = 128 * 1024 * 1024
OVERLAP = 16384


def human(n):
    return f'{n/1e9:.2f} GB' if n >= 1e9 else f'{n/1e6:.0f} MB'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--near', action='append', default=[])
    ap.add_argument('--grep', action='append', default=[],
                    help='report every distinct byte-string matching this regex '
                         '(repeatable; e.g. --grep "fx_sz11_008[A-Za-z0-9_]*")')
    ap.add_argument('--window', type=int, default=3072)
    ap.add_argument('--max', type=int, default=400, help='cap reported items per section')
    ap.add_argument('--maxwins', type=int, default=300, help='cap token windows kept per token')
    args = ap.parse_args()
    near = [t.encode() for t in args.near]
    greps = [re.compile(g.encode()) for g in args.grep]
    grep_hits = {g.pattern: set() for g in greps}

    strict = {}                 # rec -> set(tokens matched nearby)
    token_windows = []          # (token, [interesting strings])
    win_count = {t: 0 for t in near}
    seenw = set()

    try:
        total_size = os.path.getsize(args.dump)
    except OSError:
        total_size = 0

    sys.stderr.write(f'Scanning {args.dump}\n')
    sys.stderr.write(f'  size {human(total_size)} - streaming, progress below.\n\n')
    t0 = time.time()
    read_bytes = 0
    tail = b''
    base = 0

    with open(args.dump, 'rb') as f:
        while True:
            buf = f.read(CHUNK)
            if not buf:
                break
            read_bytes += len(buf)
            data = tail + buf

            # --- locate marks ONCE for this chunk ---------------------------
            mark_pos = []
            mark_str = []
            for m in EFFECTREF.finditer(data):
                mark_pos.append(m.start()); mark_str.append(m.group(0))
            for m in BONE.finditer(data):
                mark_pos.append(m.start()); mark_str.append(m.group(0))
            # sort marks by position for bisect
            order = sorted(range(len(mark_pos)), key=mark_pos.__getitem__)
            mark_pos = [mark_pos[i] for i in order]
            mark_str = [mark_str[i] for i in order]

            # --- strict attach records anywhere -----------------------------
            for m in ATTACH.finditer(data):
                rec = m.group(0)
                s = max(0, m.start() - args.window)
                e = min(len(data), m.end() + args.window)
                ctx = data[s:e]
                hits = {t for t in near if t in ctx}
                strict.setdefault(rec, set()).update(hits)

            # --- near-token windows via bisect over marks -------------------
            for t in near:
                if win_count[t] >= args.maxwins:
                    continue
                start = 0
                while True:
                    i = data.find(t, start)
                    if i < 0:
                        break
                    start = i + len(t)
                    lo = bisect.bisect_left(mark_pos, i - args.window)
                    hi = bisect.bisect_right(mark_pos, i + args.window)
                    uniq = []
                    for k in range(lo, hi):
                        xs = mark_str[k].decode('latin1', 'replace')
                        if xs not in uniq:
                            uniq.append(xs)
                        if len(uniq) >= 12:
                            break
                    if uniq:
                        key = (t, tuple(uniq))
                        if key not in seenw:
                            seenw.add(key)
                            token_windows.append((t.decode('latin1', 'replace'), uniq))
                            win_count[t] += 1
                            if win_count[t] >= args.maxwins:
                                break

            # --- global greps ----------------------------------------------
            for g in greps:
                for gm in g.finditer(data):
                    grep_hits[g.pattern].add(gm.group(0))

            # --- progress ---------------------------------------------------
            tail = data[-OVERLAP:]
            base += len(buf)
            el = max(1e-6, time.time() - t0)
            mbps = read_bytes / 1e6 / el
            if total_size:
                pct = 100.0 * read_bytes / total_size
                eta = (total_size - read_bytes) / max(1.0, read_bytes / el)
                sys.stderr.write(
                    f'\r[{pct:5.1f}%] {human(read_bytes)}/{human(total_size)}  '
                    f'{mbps:5.1f} MB/s  ETA {eta:4.0f}s  strict={len(strict)}  wins={len(token_windows)}   ')
            else:
                sys.stderr.write(
                    f'\r{human(read_bytes)}  {mbps:5.1f} MB/s  strict={len(strict)}  wins={len(token_windows)}   ')
            sys.stderr.flush()

    sys.stderr.write(f'\n\nDone in {time.time()-t0:.0f}s.\n\n')

    def hdr(x):
        print('=' * 72)
        print(x)
        print('=' * 72)

    if greps:
        hdr('EVERY EFFECT NAME RESIDENT IN THE DUMP (matches your --grep patterns)')
        for pat, names in grep_hits.items():
            print(f'\n  -- /{pat.decode("latin1","replace")}/  ({len(names)} distinct) --')
            for nm in sorted(n.decode('latin1', 'replace') for n in names)[:args.max]:
                print('  ' + nm)
        print()

    if near:
        hdr('EFFECT REFERENCES CO-LOCATED WITH YOUR TOKENS (the binding, if present)')
        if not token_windows:
            print('  (none - the effect may not be resident, or was unloaded before the '
                  'dump; recapture with the set VISIBLE on the character.)')
        shown = 0
        for tok, items in token_windows:
            print(f'\n  near "{tok}":')
            for it in items:
                print('     ' + it)
            shown += 1
            if shown >= args.max:
                print('  ... (truncated)')
                break
        print()

    hdr('ALL STRICT ATTACH RECORDS  (EffectCom/<ps>:<bone>:<time>:<flags>:<xf>)')
    withtok = [(r, s) for r, s in strict.items() if s]
    other = [(r, s) for r, s in strict.items() if not s]
    for label, group in (('matched your tokens', withtok), ('other', other)):
        if not group:
            continue
        print(f'\n  -- {label} ({len(group)}) --')
        for rec, toks in list(group)[:args.max]:
            line = rec.decode('latin1', 'replace')
            if toks:
                line += '     [near: ' + ', '.join(sorted(t.decode('latin1', 'replace') for t in toks)) + ']'
            print('  ' + line)
    if not strict:
        print('  (no strict-format records found)')
    print()
    print('Scanned %s total.' % human(read_bytes))
    print('The PS name(s) + bone in the co-located section are the confirmed')
    print('binding for graph-bound effects; for worn cosmetics they are residency')
    print('proof + candidate hardpoints (the exact tuple is not kept as text).')


if __name__ == '__main__':
    main()
