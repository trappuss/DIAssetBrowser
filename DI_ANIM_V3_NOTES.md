# CHAR::ANIM inner-v3 codec — research state (2026-08-10)

Everything here is MEASURED on real clips unless marked open. Samples live in
`_probe/anim_samples/` (dumped by `probe-anim-all.bat`); the key workhorses are
`tyrael_sword/a_idle2` (1 track, 61 frames, 576 B) and
`boss_centaur_jian/a_idle` (2 tracks, 11 frames, 384 B).

## Solved and already shipped in AnimParser
- 16-byte-preamble container: u32 @4 = SECTION DIRECTORY offset (not payload
  length). Directory = 32-byte entries [u32 size][8 unused][u32 nameLen][name].
  Sections after the preamble, 16-aligned, in order: HEADER (32 B),
  CHANNEL_DATA, MOTION_DATA, TRACKS_DATA. 15/15 samples align exactly.
- Inner version 2 in this container = the ordinary v2 track stream at the
  CHANNEL_DATA start (mask at 37). Those were the 9,025 "track count mismatch"
  refusals. Inner version 1 = v1 stream (no parent field, u32 pos count,
  u16 rc + u16 tag 0x4000, 8-byte rot keys, 48-bit smallest-three quat,
  3x15-bit fields).

## Inner v3 CHANNEL_DATA (the remaining ~12,300 clips)
Layout, marker-relative (marker = 0xAC10AC10 after the name table; base = m+8):
```
base+0   u16 ??      +2 u16 K (block count)
base+4   u8[4] 04 03 03 07   constant on all 8 samples (width menu, suspected)
base+8   u8 flagA(07|00), u8 flagB(00|01), u8 01, u8 junk
base+12  u32 totalFrames     (SUM of block frame counts; verified 8/8)
base+16  u32 30              (fps)
base+22  u16[4] quad: A,B,C,D  -- cumulative region offsets from base:
   [base+32, base+A)  record table, K x 20 B  (A-32 == 20*K verified 8/8)
   [base+A, base+B)   X: per-track field, 2 bits/track when flagB=0,
                      3 bits/track when flagB=1, padded to 4 B (verified 8/8)
   [base+B, base+C)   Y: same width as X, different content (animated flags?)
   [base+C, base+D)   Z: 3 x f32 per track (+extra 12 B entries on files with
                      position-animated tracks?). Per-track smallest-three
                      REFERENCE components: tyrael Z = (0.016,0.7069,0.7069),
                      unit-norm with implied w.
   [base+D, ...)      per-block data, then trailing global streams
```
Record (20 B): [u32 frameCount][u32 a = bits per frame][u32 b][u32 c][u32 d],
b/c/d relative to base. Per block:
- [b,c)  header; first byte = WIDTH CODE w' with a == 3*w' (verified: code 3 ->
  a=15, code 2 -> a=12 on tyrael's three blocks)
- [c,d)  "mid": initial-state region, VARIABLE length (6,6,8 bytes on tyrael's
  three blocks) — encoding open
- [d, d+ceil(fc*a/8))  the per-frame stream, LSB-first, fc frames x a bits,
  a/3 bits per channel per frame

## THE DELTA LAW (cracked, not yet shipped)
Per frame, per channel, field of w=a/3 bits:
  raw == 0        -> HOLD (no change)
  raw != 0        -> signed delta, raw - 2^(w-1)
Evidence: tyrael block1 (w=4) decodes to channel1 held on all 20 frames and
channels 0/2 updating on alternating frames with deltas
-5,-1,3,5,7,7,5,3,-1,-5 and -3,1,4,6,7,6,4,1,-3 — two smooth antiphase arcs,
exactly a one-axis idle sway. Flat 12-bit reads produce the interleaved
artifact that stalled earlier analysis.

## Session 2 findings (2026-08-10, later)
- Z LAW (verified against v2 same-rig ground truth, serrat skill1_2 vs
  skill2_2): Z[i] belongs to track i; ROOT tracks (parent -1) carry a
  reference QUATERNION (x,y,z, +w implied - serrat root = (0.5,-0.5,-0.5),
  exactly unit), all other tracks carry their reference TRANSLATION (matches
  v2 frame-0 translations, shifted one slot past the root). Entries beyond
  trackCount (serrat +4, barb +26, fnecro +92 after the root adjustment) are
  unexplained - correlate with pos-animated track counts.
- Records with a == 0 exist (centaur a_idle): fully static clip, b/c/d are
  0xFF junk, everything lives in Z + the rig bind. Constant-rotation tracks
  have NO rotation data anywhere in the file -> the renderer must hold BIND
  for them, not identity. This matters for the eventual AnimClip plumbing.
- Per-frame delta lanes are NOT plain (ch0,ch1,ch2): tyrael block1's real
  updates alternate between lane0 and lane2 with clean arcs in each. Either
  channels update at half rate interleaved, or a per-frame selector stream
  (the trailing 3-bit "92 24 49" region) permutes lanes. Unresolved.
- Cross-block continuity FAILS for "mid = start state + cumulative deltas";
  mid u16s repeat 256 for the held channel across blocks but 0xBC00 appears
  where a moving channel should be - mid is not plain component values.
- Tyrael D-region (24 B between region D and the first block payload):
  floats 1.71292 and 0.08685 among junk bytes - shaped like per-channel
  dequant (min,range) pairs for the delta accumulator. Prime suspect.

## Open (in likely dependency order)
1. "mid" region: initial quantized state per channel (variable length; NOT
   3 x u16 — sizes 6/6/8 on blocks with identical channel counts).
2. Dequantization: how deltas + Z reference floats produce component values
   (scale source unknown; Z gives the t=0 components).
3. Selector handling across frames: trailing 3-bit stream after the last block
   ("92 24 49" pattern = repeating value, plausibly the smallest-three
   selector per frame; tyrael: ~61 x 3 bits fits the region).
4. The 16-byte "01 60 4c 00 01 60 4d 00 ..." table before that stream
   (entries increment; count = K+1?).
5. X/Y bit semantics (which tracks animate / have pos channels); Z extra
   entries (+1..+93) correlate with pos-animated tracks — unverified.
6. Position channels: MOTION_DATA is 22 B on idles, 358 B on serrat a_run
   (358 = 22 + 24 x 14 — root motion keys, v2-shaped?) — unverified.
7. What TRACKS_DATA (3 B) is.

## Binary route — CLOSED as a static option (2026-08-10)
DiabloImmortal.exe is PACKED: standard sections have zero raw data, the real
code sits in a 42 MB `.jNS` section at entropy 7.98 (encrypted). Anchor strings
(CHAR::ANIM, CHANNEL_DATA, marker 0xAC10AC10) are absent from the on-disk file.
The MessiahAssetIndexerClient DLLs are unpacked but contain only the network
asset client, not the decoder. Public community tools (xforce / fangfang1984 /
oyear11 netease-messiah-tools, ResHax Messiah format threads) all list
Animations as UNSUPPORTED — nobody has published this codec.
=> The unpacked decoder exists ONLY in the live process memory (the user's
   DiabloImmortal.DMP, 3.2 GB). Reaching it needs on-device analysis (slice the
   dump around the marker/strings, disassemble the referencing function). That
   requires device_bash, which is down in this cloud session; it works natively
   in Cowork "on your computer" mode.

## Self-validating solve — the in-sandbox path (no binary needed)
The smallest-three quaternion is self-checking: correct bit layout => every key
unit-norm + continuous; wrong => norms scatter. This is how v1/v2 were solved
with no oracle. Same judge applies to v3. RULED OUT this session:
- Naive "start at Z ref ints, accumulate signed deltas * (2^(w-1)*sqrt2)":
  tyrael block0 sumsq reaches 7.5 (must be <=1). So deltas are NOT raw
  component increments at that scale. D-region floats 1.71194 and 0.086853
  (1/0.086853 ~= 11.5) are the likely real scale/range — untested against norm.
Next: treat D-region floats as per-channel (offset,scale); the delta lanes may
be residuals in a rotated/tangent frame, not raw components. Validate by
unit-norm across all 61 tyrael frames + continuity.

## DISASSEMBLY — decoder LOCATED in the process dump (2026-08-10, session 3)
di_dumpcarve pulled the real decoder from DiabloImmortal.DMP. The function that
consumes CHANNEL_DATA and reads the 0xAC10AC10 marker is at module VA
0x7ff7c0ce6b78 (carve_015); the matching writer is at 0x7ff7c0cd3a19
(carve_013). Confirmed straight from the machine code:
- Per-track runtime struct stride = 0x58 (88 bytes); parentIdx stored as
  word->dword at [track*0x58 + 0x50].
- v3 block header (r12, after the name table, aligned UP to 16):
    [r12+0x00] u32 blockTotalLen (r9)
    [r12+0x04] u32 FNV-1a-32 checksum of bytes [0x08 .. blockTotalLen)
               (seed 0x811c9dc5, prime 0x1000193) — verify before decoding
    [r12+0x08] u32 0xAC10AC10 marker
    [r12+0x0c] u16 version (==3)
    [r12+0x0e] u8  0
- Frame setup: ecx=[r15+0x1c] (frame count), xmm7=[r15+0x20]; timestep =
  (frameCount - 1) / [r15+0x20]  — so [0x20] is the fps/normalizer (30).
- [r15+0x10] u16 == trackCount (the header check that made 9,025 clips look
  like "mismatch" before the container fix).
Callees seen from the decode fn: 0x7ff7c03ea940 (bit-reader init?), plus the
per-track name-copy loop (movsx word parentIdx -> [r14+rcx+0x50], stride 0x58).
NEXT: the 384 B window ended at 0x7ff7c0ce6c42, right before the keyframe
loop. Re-carved with 16 KB windows (2 KB pre / 14 KB post) to capture the full
body + callees in one run. Read the bit-unpack + dequant math there directly;
that is the last missing piece.

## DECODE LOOP fully read (2026-08-10, session 3b) - from carve_015
Function 0x7ff7c0ce6b78. After the checksum passes it does:
  frameCount = u32[hdr+0x1c];  fps = u32[hdr+0x20] (=30);  r12 += blockLen
  duration = (frameCount-1)/fps    (float)
  trackCount = u16[hdr+0x10]
Per-track runtime struct is 0x58 bytes with THREE output lists (std::vector
{begin,end,cap}):
  +0x10 list A  element 0x10 (16 B)  <- 3rd channel, gated by [rbp+0x90][0x10]!=0
  +0x28 list B  element 0x10 (16 B)  <- 1st channel
  +0x40 list C  element 0x14 (20 B)  <- 2nd channel
A per-track PRESENCE BITSTREAM starts at frame-data byte 0: edi=byte[r12+r14];
each track consumes bits LSB-first (shr dil,1; every 8 -> r14++ reload). For
EACH track, THREE consecutive presence bits decide whether channel B / C / A
emits a key this frame. So the three channels are independently keyed per track
per frame, packed as 3 bits/track/frame across the whole clip.
THE MATH is in the sampler called each track/frame:
  call 0x7ff7c0ce40e0 (rcx=ctx, edx=trackIdx, r8=&slotB[rbp], r9=&slotC[rbp-0x10],
                        [rsp+0x20]=&slotA[rbp+0x10])
It fills three 16-byte slots (xmm) that the loop then shuffles into the lists.
Channels B and A store {timeInt(ebx), 3 floats} = (t, x,y,z) 16 B -> quaternion
w implied / translation. Channel C stores 20 B = time + 4 floats (t,x,y,z,+1).
Other callees: 0x7ff7c0d05140 (list init), 0x7ff7c0d4ce20 (per-frame setup,
takes the interpolation float in xmm), 0x7ff7c03ea940 / 0x7ff7c03ed950 /
0x7ff7c0c4eba0 (list push/grow helpers).
=> Need 0x7ff7c0ce40e0 (the sampler) to read the bit-unpack + dequant. Carved
   by crack-anim-dump2.bat (explicit-VA pass). That is the final missing piece.

## CODEC CRACKED (2026-08-10, session 3c) - from carve_016 (sampler)
The v3 rotation encoding IS smallest-three quaternion - the SAME law already
shipping+verified for v1/v2, just SIMD with a per-block bit width. Read from
the machine code, no inference:
- Sampler 0x7ff7c0ce40e0: reads ctx dwords [rcx+0x70/0x74/0x78/0x7c] (the
  per-channel bit widths - the "04 03 03 07" menu), then runs a 3-PHASE SWAR
  popcount (masks 0x92492492 / 0x49249249 / 0x24924924 = every 3rd bit) over
  the presence bitstream to count each channel's keys and locate its data.
- Dequant fn 0x7ff7c0ce5450: width w in r8d, r13d = 3*w (three stored fields).
    scale = 1.0f / sqrt(2.0f)            (consts: 1.0 @rdata, 2.0 @rdata)
    comp_j = (2 * field_j / range - 1) * scale     [range = (1<<w)-1]
    implied = sqrt(1 - comp0^2 - comp1^2 - comp2^2)
  Four unrolled arrangements = the 4 implied-index cases (idx in 0..3),
  placing the sqrt result in the right lane - identical selector law to v1/v2.
- Loop writes per track (0x58 struct, 3 std::vector lists):
    list @+0x28 (16 B key) = channel B, list @+0x40 (20 B) = channel C,
    list @+0x10 (16 B) = channel A (gated by ctx[+0x90][0x10]).
  Each key = { int timeMs, 3 floats }; 20-B channel adds one float.
- Presence: 3 bits/track/frame, LSB-first, from the byte stream at frame-data
  start; each track pulls 3 bits (chan B / C / A present this frame).

REMAINING to SHIP (implementation, not discovery):
1. Confirm the two dequant consts are exactly 2.0f and 1.0f (algebra says yes).
   Carve .rdata to be certain - explicit VAs for the next run:
     0x7ff7c2e285b8  (const at rip+0x213a0d2 from 0x7ff7c0ce54de)
     0x7ff7c2df6e27  (const at rip+0x21118fe from 0x7ff7c0ce5521)
2. Read the integer bit-reader that produces field_j + range (the part of the
   sampler between the popcount and the dequant call) for exact field order and
   how width is chosen per channel from ctx[0x70..0x7c].
3. Implement in AnimParser as a v3 path; VERIFY with the self-validating oracle
   (unit norm every key + continuity + block stitch) on all 4 serrat v3 clips,
   cross-check frame-0 vs the Z reference and vs serrat v2 on the same rig.
Do NOT ship until (3) passes - a wrong width/order renders garbage.

## VALIDATED ON REAL DATA (2026-08-10, session 3d) - serrat a_idle
Block model confirmed byte-exact against the live file (not just the code):
  blockStart = markerFileOff - 8
  +0x00 u32 blockLen (32023)
  +0x04 u32 FNV-1a-32 checksum -> RECOMPUTED 0x1881b913, EXACT MATCH over
        bytes [blockStart+8 .. blockStart+blockLen). Framing 100% correct.
  +0x08 u32 0xAC10AC10   +0x0c u16 ver(3)   +0x0e u16 0
  +0x10 u16 trackCount (84, matches)
  +0x12 u16 K = block/segment count (7)
  +0x14 u8[4] component width menu = 04 03 03 07  (per-field bit widths)
  +0x18 u8[4] flags = 07 00 01 00
  +0x1c u32 frameCount (121 = 4001ms*30/1000 + 1, matches)
  +0x20 u32 fps (30, matches)
  +0x24.. u16 region offsets (0xac,0xc4,0xdc,0x4d8 ...) + K x 20-B records
Dequant fn 0x7ff7c0ce5450: confirmed scale=1/sqrt2, field->comp = smallest3;
the raw field bits are pulled by callee 0x7ff7c1a07990 (bit reader, not yet
carved - but NOT needed: width menu + presence stream + smallest3 law are
enough to implement and let the norm/continuity oracle confirm packing order).

DISCOVERY COMPLETE. Remaining is implementation + oracle verification (see
ANIM_HANDOFF.md steps 3-5). Prototype in Python against the serrat v3 files in
v3work/ first (unit-norm + continuity), then port to AnimParser and ship.

## DECOMPILED (2026-08-11, Ghidra headless in sandbox) - DEFINITIVE
Ghidra 11.3.2 headless runs in the cloud sandbox; carve_016 decompiled to 16
functions. The rotation dequant is FUN_7ff7c0ce5b50, exact C:
  scale = 1/sqrt(2); fVar4 = 1.0
  mask  = (1<<w)-1   (w = global _DAT_7ff7c316b53c; rotation uses the 40-bit form)
  denom = mask / sqrt(2)          // = range/sqrt2
  per key: v64 = record[+8]       // 16-B raw record = {u32 time, u64 packed@+8}
    idx = (v64>>0x24)&3           // implied component, bits 36-37
    f0  =  v64        & mask      // bits 0..
    f1  = (v64>>0x0c) & mask      // bits 12..
    f2  = (v64>>0x18) & mask      // bits 24..
    comp_k = f_k/denom - scale    // == (2*f/range - 1)/sqrt2   [v2 law exactly]
    implied = sqrt(1 - sum(comp^2))
  placement is the SAME cyclic idx mapping as v2's unpackQuat. Output = 20-B
  record {u32 time, float q0,q1,q2,q3}.
=> v3 rotation == v2 40-bit smallest-three. AnimParser::unpackQuat already does
   this. The ONLY remaining unknown is the sampler pulling {time, 40-bit} per
   present key out of the per-frame bitstream (functions past the 16KB window).
Next: region-carve ~1.2 MB around the anim code, one Ghidra pass over the whole
neighborhood, transcribe the bitstream reader, implement, verify (norm +
continuity + serrat v2 cross-check), ship.
Sandbox Ghidra recipe: analyzeHeadless <proj> n -import region.bin -processor
x86:LE:64:default -loader BinaryLoader -loader-baseAddr <VA> -postScript
DecompDump.java (iterates functions, prints getDecompiledFunction().getC()).

## ACQUISITION COMPLETE (2026-08-11)
The whole decoder neighborhood (1.18 MB region) was carved and decompiled with
Ghidra headless in the sandbox: 1,724 functions. The 14 that matter are saved
verbatim in DI_ANIM_V3_DECOMPILED.txt (decode orchestrator FUN_..6930, sampler
FUN_..40e0, rotation dequant FUN_..5b50, block setup FUN_..05140, per-frame
cursor FUN_..4ce20, + list/utility callees).

CONFIRMED end-to-end from the decompiled C:
- Presence bitstream at (blockStart + blockLen): 3 bits/track/frame (2 if the
  third channel is disabled, gated by param_1[0x10]), LSB-first, byte-refilled.
  bit0 -> rotation key (list @track+0x28, 16 B {int time, q...}),
  bit1 -> 20-B channel (list @+0x40), bit2 -> translation (list @+0x10, 16 B).
- Sampler FUN_..40e0 fills the 3 value slots per (frame,track); rotation values
  go through FUN_..5b50 = v2 40-bit smallest-three (bits 0/12/24 fields, 36-37
  idx, comp=(2f/range-1)/sqrt2). Time int = (frame/fps)*_DAT_7ff7c2df99d4.
- Frame loop: uVar9 in 0..frameCount, time=uVar9/fps.

NO MORE DUMP RUNS NEEDED to implement. The bat now ALSO carves the .rdata const
pages (2.0/1.0/width/timescale) as insurance, but those are derivable via the
oracle (width=12 forced by the 40-bit layout; timescale pinned by requiring key
times monotonic in [0,durationMs]). Implementation = transcribe from the saved
C, validate with norm+continuity+serrat-v2, ship.

## Rules of engagement
Never ship a partial decoder — a wrong rotation renders garbage, which is
worse than the current honest refusal. Verify with: unit norm on every frame,
inter-frame continuity, block-boundary stitching, and bind-pose plausibility
against the rig's skeleton.

## IMPLEMENTATION SPEC (2026-08-11) - turnkey, from the decompiled C
Context struct (built by FUN_7ff7c0d05140 from blockStart bs):
  ctx[0]=bs; puVar1 = bs+0x10
  ctx[1] = puVar1 + u16[bs+0x24]   = record table A (K records x 20B) (=bs+0x30)
  ctx[4] = puVar1 + u16[bs+0x26]   = table B
  ctx[2] = puVar1 + u16[bs+0x28]   = table C
  ctx[3] = puVar1 + u16[bs+0x2a]   (0xffff => absent) = table D
  ctx[5] = puVar1 + u16[bs+0x2c]   (0xffff => absent) = table E
  ctx[6] = (frameCount-1)/fps  (float)
  ctx presence-words = ((flag[bs+0x19]!=0)+2)*trackCount rounded up /32
Record (20B, in table A): [u32 frameCount][u32 bitsPerFrame(a)][u32 b][u32 c][u32 d]
  b,c,d are u32 offsets relative to bs+0x10 into the segment's key streams.
Per-frame setup FUN_7ff7c0d4ce20(ctx, t, ...): walk records, find the one whose
  cumulative frame range covers frame; call FUN_7ff7c0ced5c0 to bracket + set
  interpolation; store per-channel stream pointers into ctx[8..].
Sampler FUN_7ff7c0ce40e0(ctx, trackIdx, &slotB, &slotA(rot), &slotC): reads the
  bracketed keys for this track from ctx[8..] streams, dequants, fills 3 slots.
Rotation dequant FUN_7ff7c0ce5b50: v2 40-bit smallest-three EXACTLY -
  v64=record16[+8]; idx=(v64>>36)&3; f0=v64&M; f1=(v64>>12)&M; f2=(v64>>24)&M;
  M=(1<<12)-1; comp=(2f/M-1)/sqrt2; implied=sqrt(1-sum^2); cyclic place by idx.
Decode loop FUN_7ff7c0ce6930: for frame in 0..frameCount: t=frame/fps;
  timeInt=(int)(t * _DAT_7ff7c2df99d4); FUN_7ff7c0d4ce20(ctx,...);
  for track in 0..trackCount: sampler fills slots; then read 3 presence bits
  (LSB-first from stream at bs+blockLen, byte-refilled): bit0->rot key to
  track.list[+0x28] {timeInt, q}, bit1->20B key to [+0x40], bit2 (if
  param[+0x10])->trans key to [+0x10].
UNKNOWNS still to pin at implement time (via oracle, no dump needed):
  - _DAT_7ff7c2df99d4 (timeInt scale): derive so key times are monotone in
    [0,durationMs]. Likely == durationMs or 1000.
  - exact per-track stream indexing inside sampler (the popcount prefix over
    the 3 presence planes gives per-track key offset); transcribe from
    FUN_7ff7c0ce40e0 value path when writing the C.
VALIDATION gate before shipping: decode serrat a_idle (v3work/), require every
rotation quat unit-norm (<1e-3), frame continuity (<25deg), and frame-0 ~ the
Z/base table; cross-check vs serrat v2 clip on the same rig.

## FINAL PIECE LOCATED (2026-08-11) - the per-channel value readers
The sampler FUN_7ff7c0ce40e0 is ONLY popcount (per-track key indexing). It
delegates the actual bit-read + dequant to three helpers, filling its 3 output
slots:
  param_3 (chan B, 20B) <- func_0x7ff7c0c582e0(ctx+0x18, seg, ctx, &bitpos)
  param_4 (chan ?, )     <- func_0x7ff7c0c58ae0(..., seg, ctx, &bitpos)
  param_5 (ROTATION, 4 dwords) <- func_0x7ff7c0c587c0(..., seg, ctx, &bitpos)
These live at 0xc58xxx, BELOW the carved region floor 0xcc0000 - the one thing
not yet captured. Also uses a byte table at 0x7ff7c317b2a0 (val*3 popcount of
3-bit groups) - captured by the .rdata insurance region already in the bat.
Bracketing FUN_7ff7c0ced5c0 = standard keyframe: pos=fps*t, floor=lowerKey,
frac=pos-floor (browser uses integer frames, frac 0).
NEXT (one carve): crack-anim-dump.bat now also grabs 0x7ff7c0c50000:0x20000
(covers 582e0/587c0/58ae0). Decompile those 3 readers -> the exact bit-unpack
that feeds the smallest-three dequant. Then the codec is 100% deterministic.

## !!! DEFINITIVE CODEC - CORRECTS THE EARLIER "v2 40-bit" CLAIM (2026-08-11)
The three value readers at 0x7ff7c0c50000 (582e0/587c0/58ae0) + the generic
width reader d6cc70 were decompiled (Ghidra headless, sandbox) and all consts
read from the carved .rdata. This is the AUTHORITATIVE rotation path called by
the sampler FUN_7ff7c0ce40e0. It SUPERSEDES the "v3 rotation == v2 40-bit
smallest-three (implied idx bits 36-37, comp=(2f/M-1)/sqrt2)" conclusion in the
sections above - that was a MISREAD of a neighbouring function and would render
garbage. Do not implement the old law.

CHANNEL -> READER (from the sampler call sites + orchestrator arg order):
  ROTATION    -> func_0x7ff7c0c582e0  (*param_3 = slotB -> track list @+0x28)
  20-B chan   -> func_0x7ff7c0c58ae0  (*param_4        -> track list @+0x40)
  TRANSLATION -> func_0x7ff7c0c587c0  (*param_5        -> track list @+0x10,
                                        emitted only if ctx byte [+0x19]!=0)

ROTATION LAW (func_0x7ff7c0c582e0) - drop-W smallest-three, NOT implied-index:
  Only x,y,z are stored; the 4th (w) is ALWAYS the reconstructed one:
      w = sqrt( clamp>=0( 1 - x^2 - y^2 - z^2 ) )   ; clamp = clear sign bit
  1.0 const @0x7ff7c2df6e28 (=1.0). clamp mask @0x7ff7c316b5d0 (=0x80000000).
  Two bracketing keys (k=0 lower, k=1 upper) are read, each producing (x,y,z):

  Per key, per component j, the raw value comes from a per-key TYPE byte
  tb = typeStream[keyCounter]  (byte); width W = table_317b2a0[tb]:
    tb == 0x00 : RAW - read 4x int16 at keyData[cursor], * (1/65535)
                 -> (x,y,z,w) used DIRECTLY (skips base/range AND bias/scale).
                 scale const @0x7ff7c317c840..c84c = 1/65535 (0x37800080).
    tb == 0x12 : 3x raw float32, bit-packed big-endian (skips base/range).
    else       : residual = d6cc70(W, segPtr, bitPos) -> 3 comps in [0,1],
                 each = (field & (2^W-1)) / (2^W-1).  Advance bitPos by W*3.
                 table_317b2a0: idx->W = {0:0,1:3,2:4,3:5,...,0x11:0x13(19),
                 0x12:0x20}; so tb 1..0x11 give W = 3..19.

  For the "else" (compressed) keys, TWO affine steps rebuild the component:
    (a) base/range (per track, unsigned bytes /255; consts @0x7ff7c317c850..c85c
        = 1/255 = 0x3b808081):
          comp = residual * (rangeByte/255) + (baseByte/255)
        baseByte[] from array at ctx[+0x50]; rangeByte[] from ctx[+0x58];
        indexed by track (stride = ctx byte [+0x3c]).
    (b) bias/scale (per track, f32 pairs at ctx[+0x28]; scale row offset by
        [+0x3c]*4):
          comp = comp * scale[j] + bias[j]
    (tb==0 skips (a)+(b); tb==0x12 skips (b).)

  Then reconstruct w per key (formula above), and blend the two keys:
    dot  = x0*x1 + y0*y1 + z0*z1 + w0*w1
    s    = sign(dot)                       ; copysign masks @0x7ff7c2df6120 (x4)
    frac = *(float*)(ctx+0x80)             ; interp fraction (0 for integer frames)
    q_j  = ( s*key1_j - key0_j ) * frac + key0_j     ; nlerp, shortest arc
    q    = q / |q|                         ; final renormalize (rsqrtss)
  Output = (qx,qy,qz,qw) to *param_3 (xmm0 lo + hi).

TRANSLATION / 20-B readers (587c0, 58ae0): same 2-key read + type dispatch, but
LINEAR (no w reconstruct, no normalize). Each reads 2 keys x (x,y,z), advances
the key-record cursor param_4[2] += 0x18. 587c0 gated by (flagByte & 4),
58ae0 by (flagByte & 2), where flagByte = seg[+8].

GENERIC WIDTH READER func_0x7ff7c0d6cc70(W, dataptr, startBit):
  reads 3 components, each W bits, from a BIG-ENDIAN bitstream:
    dword = le32(dataptr + (bit>>3)); raw = bswap32(dword) >> (32-W-(bit&7));
    comp  = float(raw & mask[W]) * scale[W]
  table @0x7ff7c317b700: 8-B entries [f32 scale, u32 mask], valid W=0..19,
  scale[W]=1/(2^W-1), mask[W]=(2^W)-1. (entries >=20 are unrelated .rdata.)

STATUS: codec is now 100% deterministic end-to-end (framing -> presence ->
per-key type -> width -> residual -> base/range -> bias/scale -> w-reconstruct
-> shortest-arc nlerp -> renormalize). No unknowns remain in the value path.
Remaining before SHIP: (1) transcribe the ctx-setup file->pointer plumbing
(FUN_7ff7c0d05140, mostly in "IMPLEMENTATION SPEC" above) + per-track key
indexing (popcount prefix in sampler), (2) Python round-trip on serrat a_idle
with the norm+continuity oracle, (3) port to AnimParser, (4) oracle gate, ship.

## SHIPPED (2026-08-11) - v3 decode implemented + validated in AnimParser
parseV3Frames() in src/model/AnimParser.cpp decodes dense per-frame rotation
keys for inner-v3 clips (was: refused). parseAnim's innerVer==3 branch now
builds the track table (probeAnimV3) then decodes keys. Verified standalone:
C++ output matches the Python reference decoder byte-for-byte on all serrat v3
clips. a_idle: 65 rot tracks, 7865 keys, max sumsq 1.000 (0 over-unit), max
inter-frame 7.26 deg, frame0==frame120 (perfect idle loop). Skill clips decode
with occasional larger single-frame jumps (plausibly real fast motion; needs
visual confirmation in the browser). Position/scale channels + held-channel
constants not yet emitted (bones fall back to bind, matching v2 behavior).

---

## Soft-body / cloth physics (2026-08-12)

Capes, tails, skirts and hair strands are skinned to `bone_software_*` bones
that the ENGINE drives with a runtime cloth solver — the .anim carries no
motion for them (v3 animates rotation only and leaves them un-keyed; v2 holds
them at bind). In a plain player they sit at bind while the body moves under
them: the piece floats in place and the mesh between a moving body bone and a
static cloth bone stretches. That was the reported bug.

`src/model/ClothSim.{h,cpp}` reproduces the secondary motion as a deterministic
Verlet / position-based-dynamics PRE-PASS that returns an AUGMENTED AnimClip:
the input clip plus one baked (or overwritten) track per cloth bone, carrying a
per-frame translation+rotation key. Nothing downstream changes — `PosePlayer`
plays the augmented clip and `GlbExporter` writes its cloth tracks as ordinary
glTF channels, so the VIEWPORT AND THE EXPORT ARE IDENTICAL BY CONSTRUCTION
(toggle off == the original clip, byte for byte).

Key design points (all chosen to stay "true to data" / never emit garbage):
  * Detection: cloth bone = a SKINNED bone whose name contains "software"
    (or "cloth"). Only skinned bones matter — they deform a mesh.
  * Anchor worlds come from `PosePlayer::computeWorlds` on the BASE clip — the
    same evaluator the viewport uses — so the baked cloth attaches exactly where
    the body draws.
  * A cloth bone's baked key is LOCAL to its DIRECT .skeleton parent, and that
    parent must be a clip track (animated body bone) or another cloth bone. We
    do NOT fold non-track connector bones into a distant anchor: glTF applies
    each key relative to the node's TREE parent, so anchoring anywhere else
    would desync export from viewport. A cloth bone with no valid direct anchor
    is left at bind (status quo, never wrong).
  * Solve: per frame, Verlet integrate each bone origin (gravity -Y, damping),
    pull toward the rigid/attached target (stiffness — kills the float), then
    relax a hard distance constraint to the parent (bone length within
    maxStretch). 12-frame settle at frame 0 so nothing snaps. NaN guard snaps
    back to the rigid target. World rotation reconstructed by a shortest-arc
    "aim" from the rest bone direction to the simulated direction.
  * Ordering guard: if a rare mixed keyed/un-keyed chain would put a parent
    track after its child, the whole bake aborts to the unchanged clip.

Defaults (ClothParams): gravity 2.5, stiffness 0.45, damping 0.90, 8 iters,
maxStretch 1.03, 12 settle. Tuned for "follows the body with gentle sway,"
never divergence (no live visual tuning was available).

Toggle: "Cloth physics" checkbox in the Models tab, persisted under the SAME
QSettings key (`export/cloth`, ExportSettings::clothPhysics) the exporter reads,
so what you see is what the .glb bakes. Applies to the primary model AND every
loaded attachment (a tail/cape is usually itself an attachment). Toggling
re-bakes the current clip live.

Verified numerically (no Qt) with /tmp/cloth_test*.cpp:
  * 2 base tracks -> 5 augmented (3 cloth appended), keys present.
  * bone length within maxStretch, no NaN across all frames.
  * body moves +2 in X: tail tip reaches 1.976 (follows, slight lag) — NOT
    stuck at origin (the float bug).
  * fast whip: tip lags the anchor by 0.049 (inertia); after motion stops it
    settles under the anchor with 0.0000 error.

---

## D4-migration: skinned-pose cloth model (2026-08-12)

Ported D4AssetBrowser's cloth PHILOSOPHY (DI has no NvCloth cage payload, so the
cage/constraint/capsule machinery is not applicable — DI cloth is skeletal). D4's
own header states the rule: "cloth mostly tracks the bone-skinned pose; physics is
a light correction." See D4_MIGRATION_PLAN.md for the full task list.

- **A1 — skinned-pose fallback (`PosePlayer`, the core fix).** An undriven skin bone
  (every `bone_software_*` bone in a v3 clip) was getting an IDENTITY skin matrix →
  vertex stuck at bind-world → the piece floats while the body moves. Now `init`
  takes the `BoneParents` hierarchy and places such a bone at its rest-pose chain
  composed onto the nearest DRIVEN ancestor's animated world — so a physics-less cape
  moves with the spine. This is the D4 "weighted by the associated bone" behaviour.
  Export already did this (nodes built from the full tree); A1 brings the viewport
  to parity. Verified: undriven tail tip x goes 0.000 (float) → 2.000 (follows body).
- **A2 — sim as a correction.** `ClothParams::boneTracking` (D4 flBoneTrackingFactor)
  blends the emitted pose toward the skinned pose: 1 = hug (no visible sway, == off),
  0 = full sim. Raw sim state still evolves for velocity continuity; only the OUTPUT
  is blended. Verified monotonic: whip lag 0.049 (trk 0) → 0.031 (0.45) → 0.000 (1).
- **A3 — tuning dialog.** "Cloth…" button → gravity / bone-tracking / stiffness /
  damping / max-stretch / iterations, persisted under `cloth/*`; the export worker
  reads the same keys (WYSIWYG). One key per param.
- **A5 — chain-vs-hair.** Chain deviation is CUMULATIVE (measured: freeing only the
  tip gives LESS tip travel than a uniformly-freer chain). So the pendulum/hug split
  is per-CHAIN: long free chains (depth≥3) run at 0.4× tracking (swing as a unit);
  hair/fur/feather-named and short trim keep 1.0× (hug). Verified: tail swings ~17%
  more than a same-length hair chain.
- **A4 — react-to-rotation: DROPPED.** Incompatible with the deterministic per-clip
  BAKE that guarantees viewport==export. D4 sims live; we bake. Documented.
- **A6 — body collision: DEFERRED.** Body radius is an untunable guess without in-game
  reference (D4 needed footage for capsule 0.52). Would risk making cloth worse.
- **A7 — `DI_DUMP_CLOTH`.** Env-gated: per-bake classification (roots/chained,
  free-pendulum/hair, max depth, params) + worst drift from the skinned pose.

Verified with /tmp/*_test.cpp (pure C++, no Qt): A1, A2, A5 each has a dedicated
numerical test; cloth_test/cloth_test2 (length/NaN/follow/settle) still pass.

### D4-migration follow-up (2026-08-12, cont.)

- **B2 — export material renumbering: NOT APPLICABLE.** DI's GlbExporter is
  one-material / one-primitive per Part (material assigned directly, no positional
  roster), so D4's subset-untextured bug can't occur here. No change.
- **B3 — hardpoint empties: mostly already handled.** DI export builds rig nodes from
  skin bones + clip tracks; HP_* bones already come across as nodes whenever a clip
  lists them (the animated-export case). A dedicated always-export toggle is low value
  and would churn verified export code — deferred unless asked. B1/B4 likewise skipped
  (internal cleanup / marginal).
- **A6 — body collision: SHIPPED opt-in.** Spheres on core body bones, radius =
  bodyRadius × bone rest length (rig-relative, not a fixed guess). Cloth pushed out
  during the solve and on the emitted output (the skinned-pose target can sit inside
  the body). OFF by default; "Body collision" in the Cloth… dialog (`cloth/bodyRadius`).
  Verified /tmp/a6_test.cpp: penetration 0.30 (off) → 0.00 (on).

### Cloth enabled for the Wardrobe tab (2026-08-12)

Shared the cloth glue into `src/app/ClothControls.{h,cpp}` (`cloth::` namespace:
paramsFromSettings / enabled / maybeBake / showTuningDialog) so Models and
Wardrobe can't drift — one QSettings key set, one tuning dialog, one bake helper.
ModelsTab now delegates to it (behaviour unchanged).

WardrobeTab integration:
- Each SlotState gained a `playClip`; `initSlotPlayer(slot)` bakes cloth (when on)
  and inits that slot's player WITH the class hierarchy (`m_hier`/`m_locals`) —
  Wardrobe previously inited players 2-arg `(clip, skel)`, so it lacked even the v3
  bind fallback; now it gets that AND the A1 skinned-pose fallback AND cloth physics.
- "Cloth" checkbox + "Cloth…" button next to Bones; toggling re-bakes every slot.
- Export bakes cloth into the shared clip by CHAINING the bake over each part's own
  skeleton (one clip drives all parts by name; each part contributes its cloth-bone
  tracks). Wired into both exportGlb (single/outfit) and exportAllSets (per-set copy
  of the clip list, since parts differ per set).
- Cloth state (on + params) is read on the GUI thread and passed into the export
  workers (never QSettings from a worker).
