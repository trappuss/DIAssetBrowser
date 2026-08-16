#!/usr/bin/env python3
# di_names_from_capture.py
import os
import re
import sys
import csv
import json
import bisect

STREAM_THRESHOLD = 256 * 1024 * 1024
CHUNK = 16 * 1024 * 1024
OVERLAP = 8 * 1024
MAX_DISTINCT_STRINGS = 4_000_000

_FAST_ASCII  = re.compile(rb'[\x20-\x7e]{4,}')
_FAST_U8CJK  = re.compile(rb'(?:[\xe4-\xe9][\x80-\xbf][\x80-\xbf]){2,}')
_FAST_U16    = re.compile(rb'(?:[\x20-\x7e]\x00){4,}')

def runs_fast(buf):
    out = []
    for m in _FAST_ASCII.finditer(buf):
        out.append((m.start(), m.group().decode('ascii')))
    for m in _FAST_U8CJK.finditer(buf):
        try:
            out.append((m.start(), m.group().decode('utf-8')))
        except UnicodeDecodeError:
            pass
    for m in _FAST_U16.finditer(buf):
        try:
            out.append((m.start(), m.group().decode('utf-16le')))
        except UnicodeDecodeError:
            pass
    out.sort()
    return out

SETKEY_RE = re.compile(r'(?:^|[^A-Za-z0-9])((?:sz|ty|s|t)\d{2,3}_\d{2,3})(?![0-9])', re.I)

def name_like(s):
    s = s.strip()
    if len(s) < 3 or len(s) > 60: return False
    if '/' in s or '\\' in s: return False
    if re.fullmatch(r'[0-9a-fA-F]{8,}', s): return False
    if re.fullmatch(r'[\x20-\x2f\x3a-\x40\[\]{}()<>_.+=-]+', s): return False
    return bool(re.search(r'[A-Za-z]', s))

def load_template_keys(path):
    keys = set()
    if os.path.isfile(path):
        with open(path, encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                k = line.split(',', 1)[0].strip().lower()
                if k: keys.add(k)
    return keys

def stream_extract(cap, strings_path):
    size = os.path.getsize(cap)
    rows = []
    seen_keys = set()
    seen_hit_off = set()
    distinct = set()
    truncated = False
    total_runs = 0
    total_namelike = 0
    sf = open(strings_path, 'w', encoding='utf-8')
    try:
        with open(cap, 'rb') as fh:
            tail = b''
            file_pos = 0
            read_done = 0
            while True:
                chunk = fh.read(CHUNK)
                if not chunk:
                    break
                buf = tail + chunk
                buf_start = file_pos - len(tail)
                strings = runs_fast(buf)
                total_runs += len(strings)
                for _, s in strings:
                    if name_like(s):
                        total_namelike += 1
                        t = s.strip()
                        if not truncated and t not in distinct:
                            distinct.add(t)
                            sf.write(t + "\n")
                            if len(distinct) >= MAX_DISTINCT_STRINGS:
                                truncated = True
                named = [(o, s) for (o, s) in strings if name_like(s)]
                named.sort()
                offs = [o for o, _ in named]
                for off, s in strings:
                    for m in SETKEY_RE.finditer(s):
                        key = m.group(1).lower()
                        g = buf_start + off
                        if g in seen_hit_off:
                            continue
                        seen_hit_off.add(g)
                        lo = bisect.bisect_left(offs, off - 2048)
                        hi = bisect.bisect_right(offs, off + 2048)
                        near = [s2 for _o, s2 in sorted(
                            (named[i] for i in range(lo, hi)),
                            key=lambda p: abs(p[0] - off))[:6]]
                        ctx = " | ".join(dict.fromkeys(near))
                        rows.append((key, s.strip()[:60], ctx))
                        seen_keys.add(key)
                read_done += len(chunk)
                pct = (read_done / size * 100.0) if size else 100.0
                sys.stdout.write("\r  scanned %d/%d bytes (%.1f%%)   " % (read_done, size, pct))
                sys.stdout.flush()
                tail = buf[-OVERLAP:]
                file_pos += len(chunk)
        sys.stdout.write("\n")
    finally:
        sf.close()
    print("streamed %d runs, %d name-like (%d distinct%s)" % (
        total_runs, total_namelike, len(distinct), ", capped" if truncated else ""))
    return rows, seen_keys

def write_candidates(cand_path, rows, keys):
    with open(cand_path, 'w', encoding='utf-8', newline='') as f:
        w = csv.writer(f)
        w.writerow(['setKey', 'class', 'name', 'in_template', 'found_in', 'nearby_name_candidates'])
        for key, src, ctx in sorted(set(rows)):
            w.writerow([key, '', '', 'yes' if key in keys else 'NEW', src, ctx])

def main():
    args = sys.argv[1:]
    if not args:
        print("usage: python dn.py <capture-file> [template.csv]")
        return 2
    cap = args[0]
    if not os.path.isfile(cap):
        print("capture file not found: " + cap)
        return 2
    here = os.path.dirname(os.path.abspath(cap))
    tmpl = args[1] if len(args) > 1 else os.path.join(here, 'di_item_names_template.csv')
    keys = load_template_keys(tmpl)
    print("loaded %d known set keys" % len(keys))
    size = os.path.getsize(cap)
    cand_path = os.path.join(here, 'name_candidates.csv')
    strings_path = os.path.join(here, 'capture_strings.txt')
    print("scanning %s (%d bytes) in %d MB chunks..." % (cap, size, CHUNK // (1024 * 1024)))
    rows, seen_keys = stream_extract(cap, strings_path)
    write_candidates(cand_path, rows, keys)
    print("\nset keys seen in capture: %d (%d of %d known)" % (
        len(seen_keys), len(seen_keys & keys), len(keys)))
    print("wrote " + cand_path)
    print("wrote " + strings_path)
    try:
        import subprocess
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Get-Content -LiteralPath '%s' -Raw | Set-Clipboard" % cand_path],
                       check=False)
        print("(name_candidates.csv also copied to clipboard)")
    except Exception:
        pass
    print("\n==== DONE ====")

if __name__ == '__main__':
    sys.exit(main() or 0)
