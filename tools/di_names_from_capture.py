#!/usr/bin/env python3
# di_names_from_capture.py — turn a Codex/Collection capture into candidate
# di_item_names.csv rows.
#
# Route 1 (the only path to REAL marketing names): the PC client receives
# cosmetic display names from the server when you open the in-game Codex /
# Collection. Capture that traffic (or a client memory dump), point this
# script at the file, and it extracts every readable string and correlates
# it with the 165 asset set keys the tool already knows about.
#
# It is FORMAT-AGNOSTIC on purpose — a HAR (.har/.json), a saved proxy
# session, a raw packet payload export, a process .dmp, or any binary all
# work, because it just scans bytes for UTF-8 and UTF-16LE strings. You do
# not have to know the wire format; neither do I until I see a sample.
#
# Outputs (next to the input file):
#   name_candidates.csv  — "setkey,,<candidate name>" rows for review, each
#                          with the context that suggested it. Fill/keep the
#                          good ones, drop the rest, save as di_item_names.csv.
#   capture_strings.txt  — every distinct readable string in the capture
#                          (so the mapping can be refined by eye / by me).
#
# Usage:  python di_names_from_capture.py <capture-file> [template.csv]
#         (template defaults to di_item_names_template.csv beside the repo)

import os
import re
import sys
import csv

# ── string extraction ───────────────────────────────────────────────────────

def utf8_runs(data, minlen=3):
    """(offset, text) for printable ASCII/UTF-8 runs (CJK passes through)."""
    out = []
    cur = bytearray(); start = 0
    i, n = 0, len(data)
    def flush(end):
        if len(cur) >= minlen:
            try:
                s = bytes(cur).decode('utf-8')
                if s.strip(): out.append((start, s))
            except UnicodeDecodeError:
                pass
    while i < n:
        b = data[i]
        if 0x20 <= b < 0x7f:
            if not cur: start = i
            cur.append(b); i += 1
        elif b >= 0xc0:
            ln = 2 if b < 0xe0 else 3 if b < 0xf0 else 4
            if i + ln <= n and all(0x80 <= data[i+k] < 0xc0 for k in range(1, ln)):
                if not cur: start = i
                cur.extend(data[i:i+ln]); i += ln
            else:
                flush(i); cur.clear(); i += 1
        else:
            flush(i); cur.clear(); i += 1
    flush(i)
    return out

def utf16le_runs(data, minlen=3, align=0):
    """(offset, text) for genuine UTF-16LE runs at a given byte alignment:
    printable ASCII (hi==0) or CJK Unified (U+4E00..U+9FFF -> hi 0x4e..0x9f).
    Scanned at both alignments by the caller; the ASCII-overlap post-filter
    removes odd-aligned ASCII misread as fake CJK."""
    out = []
    cur = bytearray(); start = align
    i, n = align, len(data) - 1
    def flush():
        if len(cur) >= minlen * 2:
            try:
                s = bytes(cur).decode('utf-16le')
                if s.strip(): out.append((start, s))
            except UnicodeDecodeError:
                pass
    while i < n:
        lo, hi = data[i], data[i+1]
        if (hi == 0x00 and 0x20 <= lo < 0x7f) or (0x4e <= hi <= 0x9f):
            if not cur: start = i
            cur += bytes([lo, hi]); i += 2
        else:
            flush(); cur.clear(); i += 2
    flush()
    return out

# ── set-key correlation ─────────────────────────────────────────────────────

# Asset stems put the set key right after '_' (e.g. yifu_sz08_004), so a \b
# lead anchor FAILS ('_' is a word char). Anchor on a non-alphanumeric-or-'_'
# start (or string start) and forbid a trailing digit so the item number is
# fully captured.
SETKEY_RE = re.compile(r'(?:^|[^A-Za-z0-9])((?:sz|ty|s|t)\d{2,3}_\d{2,3})(?![0-9])', re.I)
# a "name-like" string: has letters, some length, not an asset path / hex / guid
def name_like(s):
    s = s.strip()
    if len(s) < 3 or len(s) > 60: return False
    if '/' in s or '\\' in s: return False
    if re.fullmatch(r'[0-9a-fA-F]{8,}', s): return False
    if re.fullmatch(r'[\x20-\x2f\x3a-\x40\[\]{}()<>_.+=-]+', s): return False
    # must contain a letter (latin) or CJK
    return bool(re.search(r'[A-Za-z]', s) or re.search(r'[㐀-鿿]', s))

NAME_FIELDS = ('name', 'displayname', 'display_name', 'title', 'itemname',
               'appearancename', 'label', 'text')

def _find_setkey(v):
    if isinstance(v, str):
        m = SETKEY_RE.search(v)
        if m: return m.group(1).lower()
    return None

def json_pass(obj, out):
    """Walk parsed JSON; for each object that carries an asset stem with a set
    key, pair it with the object's name-ish field. Also unwraps HAR bodies
    (response.content.text is itself JSON) and any string that re-parses as
    JSON. out: list of (setkey, name, context)."""
    if isinstance(obj, dict):
        # set key anywhere in this object's own string values
        key = None
        for v in obj.values():
            key = key or _find_setkey(v)
        if key:
            name = None
            for k, v in obj.items():
                if isinstance(v, str) and k.lower() in NAME_FIELDS \
                        and name_like(v) and not SETKEY_RE.search(v):
                    name = v.strip(); break
            if not name:  # fall back to the most name-like non-asset value
                cands = [v.strip() for v in obj.values()
                         if isinstance(v, str) and name_like(v)
                         and not SETKEY_RE.search(v) and '_' not in v[:20]]
                name = cands[0] if cands else ''
            out.append((key, name, str(obj)[:120]))
        for v in obj.values():
            json_pass(v, out)
    elif isinstance(obj, list):
        for v in obj:
            json_pass(v, out)
    elif isinstance(obj, str) and len(obj) > 20 and obj.lstrip()[:1] in '{[':
        try:
            json_pass(json.loads(obj), out)   # HAR body / nested JSON string
        except Exception:
            pass

def try_json(data):
    """Return parsed JSON from the whole file, else None."""
    try:
        return json.loads(data.decode('utf-8', 'ignore'))
    except Exception:
        return None

import json  # noqa: E402  (kept next to json_pass for locality)

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

def merge_into_csv(csv_path, pairs):
    """Cumulatively write captured (setkey, name) pairs into di_item_names.csv.
    NEVER overwrites a name you already have — it only fills blank names and
    appends set keys the file doesn't list yet. So a capture after a patch just
    adds the new sets; existing names are untouched. Returns (filled, added)."""
    have = {}          # (key,class) -> existing name (may be '')
    lines = []         # original lines, preserved (comments + rows)
    if os.path.isfile(csv_path):
        with open(csv_path, encoding='utf-8') as f:
            lines = f.read().splitlines()
    for ln in lines:
        s = ln.strip()
        if not s or s.startswith('#'):
            continue
        parts = ln.split(',', 2)
        if len(parts) >= 1 and parts[0].strip():
            key = parts[0].strip().lower()
            cls = (parts[1].strip().lower() if len(parts) >= 2 else '')
            nm = (parts[2].strip() if len(parts) >= 3 else '')
            have[(key, cls)] = nm

    filled = added = 0
    # 1) fill blank names in existing rows, in place
    out = []
    for ln in lines:
        s = ln.strip()
        if not s or s.startswith('#'):
            out.append(ln); continue
        parts = ln.split(',', 2)
        key = parts[0].strip().lower()
        cls = (parts[1].strip().lower() if len(parts) >= 2 else '')
        nm = (parts[2].strip() if len(parts) >= 3 else '')
        if not nm and (key, cls) in pairs and pairs[(key, cls)]:
            out.append(f"{key},{cls},{pairs[(key, cls)]}")
            have[(key, cls)] = pairs[(key, cls)]
            filled += 1
        else:
            out.append(ln)

    # 2) append captured keys the file doesn't have named yet
    new_rows = []
    for (key, cls), nm in sorted(pairs.items()):
        if not nm:
            continue
        if have.get((key, cls)):     # already named — leave it
            continue
        if (key, cls) in have:       # existed but blank; step 1 handled it
            if have[(key, cls)]:
                continue
        else:
            new_rows.append(f"{key},{cls},{nm}")
            have[(key, cls)] = nm
            added += 1
    if not lines:
        out = ["# DIAssetBrowser cosmetic-set real-name overrides",
               "# setKey,class,name   (blank class = all classes)",
               "# rows below were captured from the in-game Codex"]
    if new_rows:
        out.append("# --- captured from the in-game Codex ---")
        out.extend(new_rows)
    with open(csv_path, 'w', encoding='utf-8') as f:
        f.write("\n".join(out) + "\n")
    return filled, added

def main():
    args = [a for a in sys.argv[1:]]
    do_merge = '--merge' in args
    out_csv = None
    if '--out' in args:
        i = args.index('--out')
        if i + 1 < len(args):
            out_csv = args[i + 1]
            del args[i:i + 2]
    args = [a for a in args if a != '--merge']
    if not args:
        print("usage: python di_names_from_capture.py <capture-file> "
              "[template.csv] [--merge] [--out <di_item_names.csv>]")
        return 2
    cap = args[0]
    here = os.path.dirname(os.path.abspath(cap))
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tmpl = args[1] if len(args) > 1 else \
        os.path.join(repo, 'di_item_names_template.csv')
    keys = load_template_keys(tmpl)
    print(f"loaded {len(keys)} known set keys from {tmpl}")

    data = open(cap, 'rb').read()
    print(f"read {len(data):,} bytes from {cap}")

    # ── JSON-aware pass (HAR / API responses) — direct setkey->name pairs ────
    json_pairs = []   # (setkey, name, in_template, context)
    parsed = try_json(data)
    if parsed is not None:
        raw = []
        json_pass(parsed, raw)
        for key, name, ctx in raw:
            json_pairs.append((key, name, key in keys, ctx))
        print(f"JSON pass: {len(json_pairs)} object(s) with a set key "
              f"({sum(1 for p in json_pairs if p[1])} with a paired name)")

    # UTF-8/ASCII first; record dense-ASCII byte ranges as intervals so the
    # UTF-16 pass (run at BOTH alignments) can drop any run that overlaps them
    # — that is where odd-aligned ASCII fakes CJK.
    strings = list(utf8_runs(data))          # [(offset, text)]
    ascii_iv = []                            # [(start, end)] of dense ASCII
    for off, s in strings:
        enc = s.encode('utf-8')
        if len(enc) >= 8 and all(0x20 <= b < 0x7f for b in enc):
            ascii_iv.append((off, off + len(enc)))
    ascii_iv.sort()

    def overlaps_ascii(a, b):
        import bisect as _b
        j = _b.bisect_right([iv[0] for iv in ascii_iv], b) - 1
        # check a couple of candidate intervals around j
        for k in range(max(0, j - 1), min(len(ascii_iv), j + 2)):
            s0, e0 = ascii_iv[k]
            if a < e0 and s0 < b:
                return True
        return False

    u16 = utf16le_runs(data, align=0) + utf16le_runs(data, align=1)
    for off, s in u16:
        end = off + len(s.encode('utf-16le'))
        # keep pure-CJK (real names) or ASCII-UTF16 that does NOT sit inside an
        # ASCII/UTF-8 region (that would be the odd-aligned mojibake)
        if not overlaps_ascii(off, end):
            strings.append((off, s))
    strings.sort()
    print(f"extracted {len(strings):,} readable strings "
          f"({sum(1 for _,s in strings if name_like(s)):,} name-like)")

    # candidates: for each set key occurrence, the nearest name-like strings
    outdir = here
    cand_path = os.path.join(outdir, 'name_candidates.csv')
    strings_path = os.path.join(outdir, 'capture_strings.txt')

    # index name-like strings by offset for nearest lookup
    named = [(o, s) for (o, s) in strings if o >= 0 and name_like(s)]
    named.sort()
    offs = [o for o, _ in named]
    import bisect

    rows = []
    seen_keys = set()
    for off, s in strings:
        for m in SETKEY_RE.finditer(s):
            key = m.group(1).lower()
            in_tmpl = key in keys
            # nearest name-like strings within +/- 2KB of this key's offset
            near = []
            if off >= 0:
                lo = bisect.bisect_left(offs, off - 2048)
                hi = bisect.bisect_right(offs, off + 2048)
                near = [named[i][1] for i in range(lo, hi)][:6]
            ctx = " | ".join(dict.fromkeys(near))  # dedup, keep order
            rows.append((key, in_tmpl, s.strip()[:60], ctx))
            seen_keys.add(key)

    with open(cand_path, 'w', encoding='utf-8', newline='') as f:
        w = csv.writer(f)
        w.writerow(['# fill/keep col "name", then save the good rows as di_item_names.csv'])
        w.writerow(['setKey', 'class', 'name', 'in_template', 'found_in', 'nearby_name_candidates'])
        # JSON-pass rows FIRST — these have a name already paired to the key.
        # Prefer the rows that actually carry a name; drop empty-name dupes for
        # a key once any named row exists (the HAR wrapper yields both).
        named_keys = {k for (k, nm, _, _) in json_pairs if nm}
        json_keys = set()
        emitted = set()
        for key, name, in_tmpl, ctx in sorted(set(json_pairs)):
            if not name and key in named_keys:
                continue
            rowkey = (key, name)
            if rowkey in emitted:
                continue
            emitted.add(rowkey)
            json_keys.add(key)
            w.writerow([key, '', name, 'yes' if in_tmpl else 'NEW',
                        'json:paired', ctx])
        # then the string-proximity rows for keys the JSON pass didn't pair
        for key, in_tmpl, src, ctx in sorted(set(rows)):
            if key in json_keys:
                continue
            w.writerow([key, '', '', 'yes' if in_tmpl else 'NEW', src, ctx])

    seen = set()
    with open(strings_path, 'w', encoding='utf-8') as f:
        for _, s in strings:
            t = s.strip()
            if name_like(s) and t not in seen:
                seen.add(t)
                f.write(t + "\n")

    print(f"\nset keys seen in capture: {len(seen_keys)} "
          f"({len(seen_keys & keys)} of the 165 known)")
    print(f"wrote {cand_path}")
    print(f"wrote {strings_path}")

    # ── cumulative auto-merge into data\di_item_names.csv ───────────────────
    if do_merge:
        pairs = {}
        for key, name, in_tmpl, ctx in json_pairs:
            if name:
                pairs.setdefault((key, ''), name)   # first paired name wins
        target = out_csv or os.path.join(repo, 'data', 'di_item_names.csv')
        os.makedirs(os.path.dirname(os.path.abspath(target)), exist_ok=True)
        if pairs:
            filled, added = merge_into_csv(target, pairs)
            print(f"\nmerged into {target}: {filled} filled, {added} added "
                  f"(existing names untouched)")
            print("-> use Names > Reload in the tool to apply.")
        else:
            print("\n--merge: no paired names in this capture (the JSON pass "
                  "found no name+stem pairs). Nothing merged; review "
                  "name_candidates.csv / capture_strings.txt.")
    if not seen_keys:
        print("\nNO asset set keys found in this capture. That means the server\n"
              "uses its own item IDs, not asset stems — send me capture_strings.txt\n"
              "and a note of which set was on screen, and I'll find the join.")

if __name__ == '__main__':
    sys.exit(main() or 0)
