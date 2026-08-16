#pragma once
// Texture Atlas descriptors — DI's answer to the D4 tool's "texframes".
//
// Measured on the live Resources.mpkinfo (2026-08-07): 13,665 physical entries
// named "Package/UIScript/<stem>.plist", and 13,533 of them pair BY STEM with a
// repository Texture2D (folder "ui"). The plist is the Cocos frame descriptor
// for that atlas texture: DI's UI is Cocos (measured .csb/.plist tree), and a
// Cocos .plist carries a "frames" dictionary of named sub-rectangles plus a
// "metadata" dictionary naming the sheet. Unlike D4's 2D_table.dat (count but
// no UVs, hence alpha segmentation), the plist carries REAL rectangles — so no
// segmentation heuristics are needed here.
//
// The parser is format-DETECTING, never format-assuming: it accepts the Apple
// XML plist encoding (all four Cocos frame formats, 0-3) and refuses anything
// else with the first bytes hex-dumped into the error string, so an unexpected
// on-disk encoding becomes a measurement in the log instead of garbage frames.
// (The plist BYTES were not reachable from this workstation — paks exceed the
// transfer cap — so the first real-data run is the verification pass; every
// failure mode is loud by design.)

#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace di { class MpkIndex; }

namespace AtlasPlist {

struct Frame {
    QString name;        // frame key, e.g. "icon_helm_01.png"
    int x = 0, y = 0;    // rect in the atlas (of the UNROTATED sprite content)
    int w = 0, h = 0;    // sprite size as displayed
    bool rotated = false;   // stored rotated 90° in the sheet (w/h swapped there)
    int srcW = 0, srcH = 0; // original (untrimmed) source size, 0 = unknown
    QStringList aliases;
};

struct Sheet {
    std::vector<Frame> frames;
    QString textureName;   // metadata realTextureFileName / textureFileName
    int format = -1;       // Cocos frame format 0..3, -1 = not stated
};

// Physical entry id of "Package/UIScript/<leaf>.plist" for a texture display
// name's leaf, or SIZE_MAX when the store has no descriptor for it.
size_t findDescriptor(const di::MpkIndex& mpk, const QString& displayName);

// Parse (already-inflated) plist bytes. False = not parsed; *err then states
// exactly why, including a hex head for unrecognized encodings.
bool parse(const uint8_t* data, size_t len, Sheet* out, QString* err);

}  // namespace AtlasPlist
