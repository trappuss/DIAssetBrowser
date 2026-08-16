#pragma once
// The per-tab export contract behind the adaptive Export menu — ported from
// the D4 browser's BrowserTab export-hook virtuals. MainWindow's Export menu
// (and the Ctrl+E / Ctrl+Shift+E / Ctrl+Shift+I shortcuts from Hotkeys.h)
// query the CURRENT tab through this interface on every menu open, so the
// same menu reads "Export 3 textures…" on one tab and "Export outfit…" on
// the next, offers "to last folder (…/DI/exports)" only when that tab has
// remembered one, and shows the atlas-frame actions only where frames exist.
//
// Handlers re-resolve the current tab at TRIGGER time, not menu-build time —
// a shortcut fired without opening the menu must act on the tab that is
// actually in front.
//
// Every tab implements this alongside QWidget (QWidget stays the first base
// so moc/object layout are untouched). Default implementations mean a tab
// only overrides what it can actually do — the menu omits or disables the
// rest, so no action is ever shown that cannot be performed (MenuText rule).

#include <QString>

struct ExportHooks {
    virtual ~ExportHooks() = default;

    // ── selection-scoped export (Ctrl+E prompts; Ctrl+Shift+E goes to the
    //    tab's remembered folder) ─────────────────────────────────────────
    virtual bool    canExport() const = 0;
    // Counted noun for labels: "1 texture", "12 models", "outfit", "raw asset".
    virtual QString exportWhat() const = 0;
    virtual void    exportNow(bool toLastDir) = 0;
    // Empty = this tab has no remembered folder (the to-last action hides).
    virtual QString lastExportDir() const { return {}; }

    // ── atlas frames (Textures tab) ────────────────────────────────────────
    virtual bool canExportFrames() const { return false; }
    virtual int  frameCount() const { return 0; }
    virtual int  frameSelCount() const { return 0; }
    virtual void exportFramesNow(bool all, bool toLastDir)
    {
        (void)all;
        (void)toLastDir;
    }
    virtual QString framesLastDir() const { return {}; }

    // ── preview image (decoded texture / 3D viewport grab) ─────────────────
    virtual bool canSaveImage() const { return false; }
    virtual void saveImageNow() {}

    // ── animated GIF (3D viewport tabs) ────────────────────────────────────
    // D4 keeps GIF out of the viewport toolbar entirely — it lives in the
    // Export menu and the right-click menu, gated to the tab that can make one.
    virtual bool canExportGif() const { return false; }      // turntable spin
    virtual bool canExportAnimGif() const { return false; }  // a clip is loaded
    virtual void exportGifTurntable() {}
    virtual void exportGifAnim() {}

    // ── export every row that passes the current filter (D4 "Export all
    //    matching") ──────────────────────────────────────────────────────────
    virtual bool canExportAll() const { return false; }
    virtual int  exportAllCount() const { return 0; }        // for the label
    virtual void exportAllNow(bool toLastDir) { (void)toLastDir; }

    // ── export the loaded model WITH all its animation clips (D4 "Export
    //    animations only") ──────────────────────────────────────────────────
    virtual bool canExportAnims() const { return false; }
    virtual void exportAnimsNow() {}

    // ── export the loaded model together with the attachments currently shown
    //    in the viewport (Models tab). Only offered when attachments are loaded,
    //    and it forces them in regardless of the export/attachments setting. ──
    virtual bool canExportWithAttachments() const { return false; }
    virtual void exportWithAttachmentsNow(bool toLastDir) { (void)toLastDir; }
};
