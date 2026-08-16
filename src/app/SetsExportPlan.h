#pragma once
// ── What "Export all sets…" writes ──────────────────────────────────────────
// Three switches under Settings ▸ Export ▸ Wardrobe && Bulk. They are not
// independent knobs bolted onto the run, so the resolution lives here, once,
// and both the settings page's summary line and the exporter read it — they
// cannot end up disagreeing about what the current combination does.
//
// The run is up to TWO PASSES:
//
//   SET PASS      always runs. Every armour set of the class, as the pieces it
//                 RESOLVES to (its ranked mains plus its attachment pool — what
//                 equipping the set actually gives you).
//                   · `individual` changes its SHAPE: one .glb per piece
//                     instead of one assembled outfit per set.
//                   · `noWeapons` FILTERS it: the back cosmetic and both hands
//                     come out, so you get the armour on its own.
//
//   MATCH PASS    runs IN ADDITION when `nonArmorMatches` is on, never instead.
//                 Every piece whose name carries a set key, excluding helmet /
//                 chest / shoulders / legs / hair, one .glb each. This is a far
//                 wider net than the set pass — a set resolves to about five
//                 pieces, but a dozen or more weapons can carry its key — and
//                 that width is the point: it is how you get every weapon
//                 variant of every set alongside the outfits themselves.
//
// `noWeapons` deliberately does NOT touch the match pass. Its own description
// is "prevents weapons from exporting with the set outfit", and the match pass
// exists precisely to export those weapons on their own; applying the filter to
// both would make the two switches cancel each other out, which is the one
// combination nobody could want.
//
// Pool attachments carry slot -1: neither armour nor weapon, so no filter
// removes them. Deliberate — an attachment belongs to whatever it hangs off,
// and dropping it with the garment would silently strip detail from an
// otherwise complete outfit.

#include <QSettings>
#include <QString>
#include <QStringList>

namespace SetsExport {

// Slot classification. These mirror the Wardrobe's slot order; WardrobeTab
// static_asserts the hand slots against its own constants, and the armour range
// and back-weapon slot are fixed by the same kSlotLabels table.
inline constexpr int kFirstArmor = 0;   // helmet
inline constexpr int kLastArmor  = 4;   // ... chest, shoulders, legs, hair
inline constexpr int kBackWeapon = 5;
inline constexpr int kMainHand   = 6;
inline constexpr int kOffHand    = 7;

inline bool isWeaponSlot(int slot)
{
    return slot == kBackWeapon || slot == kMainHand || slot == kOffHand;
}
inline bool isArmorSlot(int slot)
{
    return slot >= kFirstArmor && slot <= kLastArmor;
}

struct Plan {
    bool noWeapons       = false;   // export/setsNoWeapons
    bool individual      = false;   // export/setsIndividual
    bool nonArmorMatches = false;   // export/setsNonArmorMatches

    static Plan load()
    {
        QSettings s;
        Plan p;
        p.noWeapons =
            s.value(QStringLiteral("export/setsNoWeapons"), false).toBool();
        p.individual =
            s.value(QStringLiteral("export/setsIndividual"), false).toBool();
        p.nonArmorMatches =
            s.value(QStringLiteral("export/setsNonArmorMatches"), false).toBool();
        return p;
    }

    // ── set pass ──────────────────────────────────────────────────────────
    bool setsPerPiece() const { return individual; }
    // slot < 0 = a pool attachment: never filtered (see the header).
    bool wantSetSlot(int slot) const
    {
        if (slot < 0) return true;
        return !(noWeapons && isWeaponSlot(slot));
    }

    // ── match pass ────────────────────────────────────────────────────────
    bool wantMatchPass() const { return nonArmorMatches; }
    // Armour only is excluded here. Weapons are kept on purpose — see the
    // header for why noWeapons does not reach into this pass.
    bool wantMatchSlot(int slot) const { return slot >= 0 && !isArmorSlot(slot); }

    // The plain-language resolution, shown live under the three checkboxes and
    // logged when a run starts. Reading it back tells you which boxes are on.
    QString describe() const
    {
        QString out = individual
                          ? QStringLiteral("every set as one .glb per piece")
                          : QStringLiteral("every set as one .glb");
        if (noWeapons) out += QStringLiteral(" (no weapons)");
        if (nonArmorMatches)
            out += QStringLiteral(", PLUS every non-armour piece carrying a set "
                                  "key as its own .glb");
        return out;
    }
};

}   // namespace SetsExport
