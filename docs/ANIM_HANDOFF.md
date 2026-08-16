# Animation work — handoff / instructions

Single source of truth for finishing the .anim work. Pair with
`DI_ANIM_V3_NOTES.md` (the byte/codec detail). Two-environment rule still
applies: edit + build-verify in the Linux sandbox, deliver via SendUserFile,
user saves into `src\...` and runs `rebuild.bat`.

## STATE — what already works (shipped, rebuild.bat to see)
- v1 container (49 files): full decode. 48-bit smallest-three quat.
- v2 classic container: one/two key arrays (mask bit 0 @ off 25), per-track
  backtracking, rc==0 = no tag byte. ~16,300 clips play.
- 16-byte-preamble container, inner v1/v2: section directory parsed; these were
  the 9,025 "track count mismatch" refusals — now decode.
- Overall ~43% of 37,769 clips play + export to .glb. Filters + Clips column +
  animated/player facets: shipped.
- Flood fixed: info panel is bounded + de-duplicated; unsupported clips greyed
  in the dropdown.

## STATE — v3 (the remaining ~12,300 clips, incl. serrat's 4)
CRACKED from the game binary (see DI_ANIM_V3_NOTES.md "CODEC CRACKED"):
v3 rotation = smallest-three quaternion, SAME law as v1/v2, per-block bit width.
Decoder VA 0x7ff7c0ce6b78, sampler 0x7ff7c0ce40e0, dequant 0x7ff7c0ce5450.
NOT yet implemented — do not ship a partial decoder.

## TO FINISH v3 — do these in order

1. (USER, optional, ~2 min) Confirm the two dequant constants are exactly
   2.0f and 1.0f. Run:
       crack-anim-dump.bat
   after editing the trailing VAs in it to these two .rdata addresses:
       0x7ff7c2e285b8  0x7ff7c2df6e27
   Send the new carve_*_fn_*.bin back. (Algebra already says 2.0/1.0; this is
   belt-and-suspenders. Skippable — I can proceed without it.)

2. (ME) Read the integer bit-reader inside the sampler carve (field order +
   how each channel's width is chosen from ctx[0x70..0x7c]). Already have the
   carve; no user action.

3. (ME) Implement the v3 path in src/model/AnimParser.cpp:
   - parse block header (marker/version/FNV checksum already known),
   - walk the 3-bit/track/frame presence bitstream,
   - smallest-three decode per key at the block's width,
   - fill pos/rot the same AnimClip the rest of the app consumes.

4. (ME) VERIFY before shipping — the self-validating oracle, no ground-truth
   clip needed:
   - every decoded quaternion unit-norm (|q|-1 < 1e-3),
   - frame-to-frame continuity (no >25 deg jumps),
   - block-boundary stitch continuous,
   - frame count == header, duration == (frames-1)/30,
   - cross-check frame-0 vs the Z reference table, and vs serrat's v2 clips on
     the same rig.
   Ship only if ALL pass. A wrong width/order renders a scrambled skeleton —
   worse than the current honest refusal.

5. (USER) rebuild.bat; open Char/mon/battlepet_serrat, confirm a_idle plays;
   spot-check a boss and an NPC; export one to .glb.

## IF YOU MAKE A FRESH DUMP
ASLR moves every VA. Re-run crack-anim-dump.bat (string-anchored pass finds the
decoder regardless), tell me the new decode-loop base, I recompute the explicit
VAs. The dump you have now is fine and all current VAs match it.

## TOOLING (all in the repo, build via the .bat files)
- di_probe            --anim-verify / --anim-forensics / --asset (REAL parser
                      over all clips; per-folder pass %, failure families)
- di_dumpcarve        pulls the unpacked decoder out of a .DMP by string xref +
                      marker; trailing 0x.. args carve explicit function VAs
- crack-anim-dump.bat one-run: build di_dumpcarve, carve the decoder + sampler
- probe-anim-all.bat  one-run: every anim probe mode over the archive
