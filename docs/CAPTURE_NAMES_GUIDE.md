# Capturing real cosmetic names (Route 1)

The PC client has **no item-name text on disk** — the marketing names
("Dishonored Legionnaire") come from the server when you open the in-game
**Codex / Collection / Wardrobe**. This guide gets that data off the wire (or
out of client memory) so `di_names_from_capture.py` can turn it into
`di_item_names.csv` rows.

You don't need to know the exact format — the extractor scans any file for
readable strings and correlates them with the 165 asset set keys the tool
already decodes. Just capture *something* while the cosmetics are on screen,
run `capture-names.bat` on it, and send back the result (or a sample) so the
mapping can be confirmed.

Try the methods in order — the first that produces names wins.

---

## Method A — network proxy with TLS interception (best, if it's HTTP)

If the Codex data travels over HTTPS, a proxy captures it as clean JSON.

1. Install **Fiddler Classic** (free) or **mitmproxy**.
2. Enable HTTPS decryption:
   - Fiddler: Tools > Options > HTTPS > *Decrypt HTTPS traffic*, install the
     root cert when prompted.
   - mitmproxy: run `mitmproxy`, browse to `http://mitm.it` once and install
     its cert.
3. With the proxy running (system proxy on), launch Diablo Immortal and open
   the **Codex / Collection / Wardrobe** — scroll through the cosmetic sets so
   the client requests their data. Open a few set detail pages.
4. Export the session:
   - Fiddler: File > Export Sessions > All Sessions > **HTTPArchive (HAR)**.
   - mitmproxy: `:w capture.har` (or save the flow file).
5. Run `capture-names.bat "capture.har"`.

If the traffic is empty or unreadable, the game may pin certificates or use a
non-HTTP game protocol — go to Method B.

---

## Method B — client memory dump (works even without network capture)

When a cosmetic name is drawn on screen, its text is in the client's memory.

1. Open the **Codex / Collection** in-game and leave a set with a known name
   visible (note which set it is).
2. Open **Task Manager > Details**, right-click `DiabloImmortal.exe` >
   **Create dump file**. (This writes a big `.DMP` in your temp folder — the
   dialog shows the path.)
3. Run `capture-names.bat "C:\...\DiabloImmortal.DMP"`.

The dump is large; the extractor handles it. Names may be UTF-16 (Windows) —
the tool reads both encodings.

---

## Method C — a web collection / API (if one exists)

If you use any Diablo Immortal companion site or web collection that shows
your cosmetics, open it in a browser, open DevTools > Network, reload the
collection, and **Save all as HAR**. Run `capture-names.bat` on that HAR.

---

## What comes out

`name_candidates.csv` next to your capture, e.g.:

```
setKey,class,name,in_template,found_in,nearby_name_candidates
sz08_004,,,yes,"...f_barbarian_yifu_sz08_004...","Dishonored Legionnaire | ..."
```

- Rows where the asset stem (e.g. `sz08_004`) sits right next to a readable
  name are near-certain — copy the name into the `name` column.
- `in_template = NEW` means a set key the tool didn't know about (worth adding).
- If the capture has names but the extractor found **no set keys**, the server
  used its own numeric item IDs instead of asset stems. That's still solvable —
  send `capture_strings.txt` plus a note of which set was on screen, and the
  id -> asset-key join can be worked out.

Keep the good rows, save the file as `data\di_item_names.csv`, and use
**Names > Reload** in the tool. The Meaning column, info panels and Wardrobe
set picker then show the real names.
