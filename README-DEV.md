# DIAssetBrowser — dev notes

Native C++ Diablo Immortal asset browser, forked in discipline (and soon in code)
from Diablo4AssetBrowser Native. Audit findings: `DI_AUDIT.md`.

## Current state — shell + Assets browser (storage layer proven)

```
src/store/   framework-free storage seam (std + lz4, no Qt):
  Zzz4       ZZZ4/LZ4 blob inflate (CCCC-framed variant included)
  MpkIndex   .mpkinfo parse + lazy .mpk reads + patch-proof signature()
  Repository resource.repository catalog — u32 count (551,524 entries; the
             community template's u16 was wrong, measured 2026-08-01)
  AssetStore the seam: physical index + logical catalog + hash bridge
src/app/     MainWindow (background index load), AppPaths (portable data\),
             LogConsole, SehGuard — AppPaths/LogConsole/SehGuard verbatim from D4
src/index/   AssetIndex (store + per-row UI metadata), AssetListModel
             (677k rows, filtered-index architecture from SnoListModel)
src/tabs/    AssetsTab — search (-exclude grammar), type filter, details pane
tools/di_probe.cpp   console prover — measured numbers for every format claim
```

Machine-verified: index 677,307 (exact baseline match) · 107/107 paks ·
repository 551,524 entries, 0 bytes leftover · hash bridge 100% · Built.version
`2026-05-18/r46912050`.

## Build

- `build.bat` — first build (vcpkg resolves qtbase + lz4; the D4 binary cache
  should make Qt minutes, not hours) → deploys + launches via `run.bat`.
- `rebuild.bat` — incremental edit→test loop (verify-src, live output, error
  distill to `build_errors.txt`).
- `build-probe.bat` — storage-layer prover only (no Qt needed).
- Requires `VCPKG_ROOT`, same as the D4 tool. Log: `data\DIAssetBrowser.log`.

## Next

1. Textures tab (Texture2D/ASTC decode — shortest path to visible proof).
2. Models tab + viewport (port GLModelWidget, strip cloth/hardpoints).
3. Funnel filters; Bulk Extract (raw export ships early); package + smoke test.

Formats reference: the archived Python tool
(`Diablo Claude Archive\DiabloImmoralAssetBrowser\DiabloImmortal_AssetBrowser.Claude\diasset\`)
— its parsers are the spec; 010 templates and DIDT source are tie-breakers.
