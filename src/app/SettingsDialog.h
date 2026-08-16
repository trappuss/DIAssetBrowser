#pragma once
// File > Settings… — tabbed dialog. "Information" documents, in plain
// language, how the tool does what it does (every claim in it was measured on
// the live game data; see di-browser project notes for the raw numbers).
// "Export" hosts the .glb export options; "Hotkeys" and "Hover" edit the
// Hotkeys.h / HoverInfo.h QSettings keys in-app (retiring edit-settings.bat).
// Every control writes the SAME QSettings key its consumer reads (one setting,
// one key) — hover knobs apply on the next hover, hotkeys at next launch.
//
// CONTROLS WRITE LIVE, so the dialog needs its own undo: it snapshots every key
// it owns when it opens, and Cancel puts that snapshot back. Restore Defaults
// REMOVES the owned keys instead, which is not the same thing — an absent key
// reads as the default its consumer compiled in, so there is exactly one
// definition of "default" in the codebase rather than a second copy here that
// could drift from the first.

#include <QDialog>
#include <QHash>
#include <QStringList>
#include <QVariant>

class QTabWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // The QSettings namespace this dialog is responsible for. Anything matching
    // one of these is snapshot-able, revertible and resettable; anything else
    // (window geometry, the game folder, per-tab view state) is not the
    // dialog's to touch. Keep in step with the keys the build* methods write.
    static QStringList ownedPrefixes();   // matched with startsWith
    static QStringList ownedKeys();       // matched exactly

public slots:
    // Cancel, Escape and the window's X all route through here, so all three
    // undo. Close (accept) is the only way to keep a change.
    void reject() override;

private:
    QWidget* buildInformationTab();
    QWidget* buildExportTab();
    QWidget* buildViewportTab();
    QWidget* buildMaintenanceTab();
    QWidget* buildHotkeysTab();
    QWidget* buildHoverTab();

    void buildTabs();            // (re)populate m_tabs, keeping the current page
    void snapshotOwnedKeys();    // called once, at open
    void revertToSnapshot();     // Cancel
    void restoreDefaults();      // Restore Defaults (with a confirmation)
    static bool owns(const QString& key);

    QTabWidget* m_tabs = nullptr;
    QHash<QString, QVariant> m_snapshot;   // owned keys as they were at open
};
