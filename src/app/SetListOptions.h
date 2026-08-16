#pragma once
// ── Which armour sets the Wardrobe's Set list offers ───────────────────────
// DI ships awakened tiers of a cosmetic set as SEPARATE pieces whose names
// carry an "_aw<N>" suffix on top of the set key:
//
//   f_barbarian_yifu_t07_004        the base set
//   f_barbarian_yifu_t07_004_aw3    the same garment, awakened tier 3
//
// The set picker's key regex stops at the "t07_004" part, so every awakened
// tier collapsed onto the base key and none of them could ever be equipped as a
// set — the pieces were in the slot dropdowns, but nothing put them on
// together. They are now their own set entries ("t07_004_aw3"), which the rest
// of the tab needs no changes to handle: applySet matches on "_<key>", and
// "..._t07_004_aw3" ends with "_t07_004_aw3" exactly as a base piece ends with
// "_t07_004".
//
// That roughly quadruples the length of the Set list on classes with awakened
// gear, which is not always what you want — hence the toggles. The master
// switch is off by default so the list looks like it always has until you ask
// for more.
//
// The per-tier switches are 1-3 because that is what the data ships. A tier
// outside that range is NOT silently dropped: it follows the master switch and
// is logged, so new data shows up rather than disappearing into a filter that
// was written before it existed.

#include <QRegularExpression>
#include <QSettings>
#include <QString>

namespace SetList {

// Highest tier with its own toggle. Anything above follows the master switch.
inline constexpr int kMaxNamedTier = 3;

struct Options {
    bool includeAwakening = false;   // wardrobe/sets/awakening
    bool tier[kMaxNamedTier] = {};   // wardrobe/sets/aw1 .. aw3

    static Options load()
    {
        QSettings s;
        Options o;
        o.includeAwakening =
            s.value(QStringLiteral("wardrobe/sets/awakening"), false).toBool();
        for (int i = 0; i < kMaxNamedTier; ++i)
            o.tier[i] = s.value(QStringLiteral("wardrobe/sets/aw%1").arg(i + 1),
                                true)
                            .toBool();
        return o;
    }

    // tier <= 0 means "not an awakened key" and is always kept.
    bool wantTier(int tier) const
    {
        if (tier <= 0) return true;
        if (!includeAwakening) return false;
        if (tier > kMaxNamedTier) return true;   // unknown tier: master decides
        return this->tier[tier - 1];
    }
};

// The awakened tier a set key carries, or 0 for a base key.
// "t07_004_aw3" -> 3;  "t07_004" -> 0.
inline int tierOf(const QString& setKey)
{
    static const QRegularExpression re(QStringLiteral("_aw(\\d+)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(setKey);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

// The base key an awakened key belongs to ("t07_004_aw3" -> "t07_004").
inline QString baseKeyOf(const QString& setKey)
{
    const int t = tierOf(setKey);
    if (t <= 0) return setKey;
    const int at = setKey.lastIndexOf(QStringLiteral("_aw"), -1);
    return at > 0 ? setKey.left(at) : setKey;
}

}   // namespace SetList
