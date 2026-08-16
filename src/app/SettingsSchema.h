#pragma once
// ── Settings schema versioning ─────────────────────────────────────────────
// The settings file outlives the build that wrote it. A user updates the tool
// and their old profile is still sitting in data/DIAssetBrowser.ini, holding
// keys this build may have renamed, split or given a new meaning. Without a
// version stamp there is no way to tell "the user never set this" apart from
// "an older build wrote this under a different name", and the difference is the
// difference between honouring their configuration and silently resetting it.
//
// WHAT THIS REPLACES
//   The animation-scope split (one export/animMode enum -> the includeAnim /
//   animOriginal / animPreviewed / animBase booleans) shipped with its own
//   migration inlined into AnimExportScope::load(). That worked, but it re-ran
//   the check on every single export, it wrote settings from whatever thread
//   happened to be exporting, and it left no record that a migration had ever
//   happened. Every future rename would have added another such lump in another
//   header. They live here instead, run once, on the GUI thread, at startup.
//
// THE RULE
//   Bump kVersion when the MEANING of an existing key changes, when a key is
//   renamed, or when one key is split into several. Adding a brand-new key with
//   a sensible default needs no bump: an absent key already reads as its
//   default, which is the correct answer for a user who never set it.
//
// FORWARD COMPATIBILITY
//   A profile stamped NEWER than this build is left completely alone and the
//   fact is logged. An older build cannot know what a newer one meant by a key,
//   so "migrating" it would be guesswork that destroys the newer setup — the
//   one thing worse than not migrating.

#include <QSettings>
#include <QString>
#include <QStringList>

namespace SettingsSchema {

// Bump this WITH a matching case in migrate(); see the rule above.
//   1  export/animMode (0 = playing clip, 1 = every clip) split into
//      export/includeAnim + animOriginal + animPreviewed + animBase.
inline constexpr int kVersion = 1;

inline const char* kVersionKey = "settings/schemaVersion";

// v0 -> v1. The old enum only ever had two states and always embedded
// something, so "every clip" maps to the data-derived sources (original + base)
// and "the playing clip" maps to previewed; includeAnim goes on either way,
// because under the old model there was no way to ask for no animation at all.
inline void migrateTo1(QSettings& s)
{
    if (!s.contains(QStringLiteral("export/animMode"))) return;
    const bool wasAll = s.value(QStringLiteral("export/animMode"), 0).toInt() == 1;
    // Never overwrite a key the user has already set under the new scheme —
    // that would undo a deliberate choice made since the update.
    if (!s.contains(QStringLiteral("export/animOriginal"))) {
        s.setValue(QStringLiteral("export/includeAnim"), true);
        s.setValue(QStringLiteral("export/animOriginal"), wasAll);
        s.setValue(QStringLiteral("export/animPreviewed"), !wasAll);
        s.setValue(QStringLiteral("export/animBase"), wasAll);
        qInfo("settings: migrated export/animMode=%d to the animation-source "
              "booleans", wasAll ? 1 : 0);
    }
    s.remove(QStringLiteral("export/animMode"));   // the key no longer has a reader
}

// Run every migration this build knows about, once. Call from main() BEFORE the
// window is built, so no widget can read a key that is about to be rewritten.
inline void migrate()
{
    QSettings s;
    const bool fresh = s.allKeys().isEmpty();
    const int stored = s.value(QLatin1String(kVersionKey), 0).toInt();

    if (fresh) {                       // nothing to migrate; stamp and go
        s.setValue(QLatin1String(kVersionKey), kVersion);
        return;
    }
    if (stored > kVersion) {
        qWarning("settings: profile is schema v%d but this build understands v%d "
                 "- leaving it untouched (run the newer build, or delete "
                 "data/DIAssetBrowser.ini to start clean)",
                 stored, kVersion);
        return;                        // deliberately do NOT stamp it back down
    }
    if (stored == kVersion) return;

    qInfo("settings: migrating profile v%d -> v%d", stored, kVersion);
    for (int v = stored + 1; v <= kVersion; ++v) {
        switch (v) {
        case 1: migrateTo1(s); break;
        default: break;                // a version with no data change of its own
        }
    }
    s.setValue(QLatin1String(kVersionKey), kVersion);
    s.sync();
}

}   // namespace SettingsSchema
