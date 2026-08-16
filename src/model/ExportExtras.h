#pragma once
// Optional files an export writes BESIDE the .glb — ported from
// D4AssetBrowser's deps/loose-texture writers and retrofitted to DI's MPK.
//
// Both are additive and never change the .glb itself: it keeps its embedded
// textures and stays one self-contained file. That is deliberate — rewriting
// the glTF to reference external URIs would change what a .glb IS and break
// every caller that assumes it is standalone.

#include <QImage>
#include <QString>

#include <cstdint>

#include "model/MeshTextures.h"
#include "store/AssetStore.h"

namespace di {

// Write the RAW game blobs a model came from into `destDir` (a "deps" folder):
// its mesh, material, skeleton and every bound texture, exactly as the MPK
// stores them — ZZZ4-wrapped blobs are inflated first, so what lands on disk is
// the same bytes this tool parses.
//
// ONE deps folder per output folder, shared by every model written beside it,
// so a texture ten pieces reference is stored once rather than ten times.
// Returns the number of files written. Every failure is non-fatal: a missing
// blob or an unwritable path skips that file and the export still succeeds.
int writeRawDeps(const di::DiAssetStore& store, int32_t repoIdx,
                 const QString& assetName, const QString& destDir);

// Write the four exported maps as PNGs into `destDir` (a "textures" folder):
//   <model>_basecolor.png · _normal.png · _mix.png · _emissive.png
// These are the glTF versions — the same QImages the .glb embedded — not the
// game's originals. `mix` is one image with three maps in its channels
// (R=roughness, G=metallic, B=AO), which is why it keeps that name rather than
// being split. Returns the number of files written.
int writeLooseTextures(const MeshTextures& tex, const QString& modelName,
                       const QString& destDir);

}   // namespace di
