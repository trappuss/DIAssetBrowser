#pragma once
// Ported 1:1 from D4AssetBrowser Native (src/tabs/BrowserTab.h), retrofitted to
// DI's store/index types. The shared panel/toolbar STYLE constants are copied
// verbatim so every DI tab's chrome matches the D4 tool exactly; the base-class
// plumbing is rebound to DI (DiAssetStore + AssetIndex, GLMeshView preview).

#include <QString>
#include <QWidget>

#include <memory>

namespace di { class DiAssetStore; }
struct AssetIndex;
class GLMeshView;

// ── Shared look for panel / page / section TITLES ────────────────────────────────────────────
// Deliberately NEUTRAL: the gold accent means something specific in this app — active filter
// counts, pulled animation clips, authored-vs-live divergence. Titles are structure, not state,
// so accenting every one of them just made the panels shout.
inline constexpr const char* kHdrQss    = "color:#dedede;font-weight:bold;";
inline constexpr const char* kSubHdrQss = "color:#9a9a9a;font-weight:bold;";

// ── Shared control skin for the view/header toolbars ────────────────────────────────────────
// One visual language: controls #2b2b2b/#555/3px with the red hover, panels a shade darker
// (#232323, #5a5a5a, 4px). Everything here is a named constant so a new control can't drift.
// TEXT buttons — the 8px side padding is what makes a label breathe.
inline constexpr const char* kToolBtnQss =
    "QToolButton{padding:2px 8px;border:1px solid #555;border-radius:3px;"
    "background:#2b2b2b;color:#bbb;}"
    "QToolButton:hover{border-color:#b0453c;}"
    "QToolButton:checked{background:#8a1414;color:#fff;border-color:#a01818;}"
    "QToolButton::menu-indicator{width:0px;}";   // we draw our own arrow where we want one
// ICON-ONLY buttons — same skin, but 8px of side padding would eat most of a small button.
inline constexpr const char* kIconBtnQss =
    "QToolButton{padding:1px;border:1px solid #555;border-radius:3px;"
    "background:#2b2b2b;color:#ddd;}"
    "QToolButton:hover{border-color:#b0453c;}"
    "QToolButton:checked{background:#8a1414;color:#fff;border-color:#a01818;}"
    "QToolButton::menu-indicator{width:0px;}";
// The slim arrow affordances: no padding at all, or the glyph has nowhere to draw.
inline constexpr const char* kArrowBtnQss =
    "QToolButton{padding:0px;border:1px solid #555;border-radius:3px;"
    "background:#2b2b2b;color:#ddd;font-size:13px;}"
    "QToolButton:hover{border-color:#b0453c;}"
    "QToolButton::menu-indicator{width:0px;}";
// QPushButton twin of kToolBtnQss — same D4 dark-red-hover skin for the plain
// action buttons (Frame / Save / Export / All / None / Invert / transport).
inline constexpr const char* kPushBtnQss =
    "QPushButton{padding:2px 8px;border:1px solid #555;border-radius:3px;"
    "background:#2b2b2b;color:#bbb;}"
    "QPushButton:hover{border-color:#b0453c;}"
    "QPushButton:pressed{background:#8a1414;color:#fff;}"
    "QPushButton:checked{background:#8a1414;color:#fff;border-color:#a01818;}"
    "QPushButton:disabled{color:#666;border-color:#3a3a3a;}";
inline constexpr const char* kPanelQss =
    "QFrame{background:#232323;border:1px solid #5a5a5a;border-radius:4px;}"
    "QLabel{color:#cccccc;border:none;} QCheckBox{color:#cccccc;border:none;}";
inline constexpr int kBarH = 26;   // uniform control height across the header + view toolbars

// Common base for the module tabs. Plain QWidget (no Q_OBJECT here — derived tabs declare their
// own). DI hands each tab the shared asset index; refresh() runs when the tab first shows.
class BrowserTab : public QWidget {
public:
    using QWidget::QWidget;
    void setIndex(std::shared_ptr<AssetIndex> index) { m_idx = std::move(index); onIndex(); }

    virtual void refresh() {}
    virtual void reset() {}                 // drop cached state so the next refresh() reloads
    virtual void onSettingsChanged() {}     // re-read settings after the Settings dialog closes
    virtual void persistView() {}           // flush per-tab view state on app close

    // ── Export hooks (used by the top-level Export menu) ─────────────────────────
    virtual GLMeshView* previewWidget() { return nullptr; }
    virtual bool    hasExportSelection() const { return false; }
    virtual void    exportSelection() {}
    virtual void    exportSelectionToLast() {}
    virtual QString exportNoun() const { return QStringLiteral("selection"); }
    virtual bool    hasAnimExport() const { return false; }
    virtual QString animExportLabel() const { return QStringLiteral("Export animations only (.glb)…"); }
    virtual void    exportAnimations() {}

protected:
    virtual void onIndex() {}                // the index just changed
    std::shared_ptr<AssetIndex> m_idx;
};
