#include "app/SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QImageWriter>
#include <QLineEdit>
#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QProcess>
#include <QRadioButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "app/AppLog.h"
#include "tabs/BrowserTab.h"   // kPushBtnQss
#include "app/Prereqs.h"
#include "app/ExportSettings.h"
#include "app/SetsExportPlan.h"
#include "util/ExportLayout.h"
#include "util/NameTemplate.h"
#include "app/Hotkeys.h"

// ── The dialog's settings namespace ────────────────────────────────────────
// Everything the build* methods below write lives under one of these. Cancel
// and Restore Defaults act on exactly this set and nothing else, so neither can
// reach the window geometry, the game folder, or a tab's own view state.

QStringList SettingsDialog::ownedPrefixes()
{
    return {QStringLiteral("export/"), QStringLiteral("hover/"),
            QStringLiteral("hotkeys/"), QStringLiteral("view/")};
}

QStringList SettingsDialog::ownedKeys()
{
    // Two Wardrobe behaviour switches that live on the Interface tab. Named
    // individually rather than by a "wardrobe/" prefix, which would also sweep
    // up the tab's panel layout and camera state.
    return {QStringLiteral("wardrobe/setClearsUnmatched"),
            QStringLiteral("wardrobe/view/thumb3d"),
            QStringLiteral("wardrobe/sets/awakening"),
            QStringLiteral("wardrobe/sets/aw1"),
            QStringLiteral("wardrobe/sets/aw2"),
            QStringLiteral("wardrobe/sets/aw3")};
}

bool SettingsDialog::owns(const QString& key)
{
    for (const QString& p : ownedPrefixes())
        if (key.startsWith(p)) return true;
    return ownedKeys().contains(key);
}

void SettingsDialog::snapshotOwnedKeys()
{
    m_snapshot.clear();
    QSettings s;
    for (const QString& k : s.allKeys())
        if (owns(k)) m_snapshot.insert(k, s.value(k));
}

void SettingsDialog::revertToSnapshot()
{
    QSettings s;
    // Remove first, then restore: a key the user CREATED in this session (one
    // that had no stored value when the dialog opened) has to disappear, not
    // just be overwritten, or it would shadow its consumer's default forever.
    const QStringList now = s.allKeys();
    for (const QString& k : now)
        if (owns(k)) s.remove(k);
    for (auto it = m_snapshot.constBegin(); it != m_snapshot.constEnd(); ++it)
        s.setValue(it.key(), it.value());
    s.sync();
}

void SettingsDialog::restoreDefaults()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Restore defaults"),
        QStringLiteral("Reset every setting on the Viewport, Interface, Export "
                       "and Hotkeys tabs to its default?\n\n"
                       "Your game folder, window layout and per-tab view state "
                       "are not touched. Cancel still undoes this."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    QSettings s;
    // REMOVE rather than write default values: an absent key reads as whatever
    // default its consumer compiled in, so there is one definition of "default"
    // in the codebase instead of a second copy here that could drift from it.
    int n = 0;
    const QStringList now = s.allKeys();
    for (const QString& k : now)
        if (owns(k)) { s.remove(k); ++n; }
    s.sync();
    qInfo("settings: restored defaults (%d key(s) cleared)", n);
    buildTabs();   // every control re-reads, so the dialog shows the defaults
}

void SettingsDialog::buildTabs()
{
    if (!m_tabs) return;
    const int keep = m_tabs->currentIndex();
    while (m_tabs->count() > 0) {
        QWidget* page = m_tabs->widget(0);
        m_tabs->removeTab(0);
        delete page;               // takes the scroll area and its child page
    }
    // Each tab scrolls (D4 pattern) so a small window never clips controls.
    auto scrollWrap = [this](QWidget* page) -> QWidget* {
        auto* sc = new QScrollArea(this);
        sc->setWidgetResizable(true);
        sc->setFrameShape(QFrame::NoFrame);
        sc->setWidget(page);
        return sc;
    };
    // Order mirrors D4: the settings-you-change tabs first, reference last.
    m_tabs->addTab(scrollWrap(buildViewportTab()), QStringLiteral("Viewport"));
    m_tabs->addTab(scrollWrap(buildHoverTab()), QStringLiteral("Interface"));
    m_tabs->addTab(scrollWrap(buildExportTab()), QStringLiteral("Export"));
    m_tabs->addTab(scrollWrap(buildHotkeysTab()), QStringLiteral("Hotkeys"));
    m_tabs->addTab(scrollWrap(buildMaintenanceTab()), QStringLiteral("Maintenance"));
    m_tabs->addTab(scrollWrap(buildInformationTab()), QStringLiteral("Information"));
    if (keep >= 0 && keep < m_tabs->count()) m_tabs->setCurrentIndex(keep);
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Settings"));
    resize(860, 640);
    snapshotOwnedKeys();   // before a single control is built and can write
    auto* lay = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    buildTabs();
    lay->addWidget(m_tabs, 1);

    // Close keeps what was written (the controls already applied it); Cancel
    // puts the whole owned namespace back to how the dialog found it. Both are
    // offered because "every control writes live" is otherwise a trap: there
    // was no way out of a change made by accident.
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Close | QDialogButtonBox::Cancel |
            QDialogButtonBox::RestoreDefaults,
        this);
    if (QPushButton* b = buttons->button(QDialogButtonBox::Close)) {
        b->setDefault(true);
        b->setToolTip(QStringLiteral("Keep every change (they are already applied)"));
    }
    if (QPushButton* b = buttons->button(QDialogButtonBox::Cancel))
        b->setToolTip(QStringLiteral(
            "Undo everything changed since this dialog opened"));
    if (QPushButton* b = buttons->button(QDialogButtonBox::RestoreDefaults))
        b->setToolTip(QStringLiteral(
            "Clear these tabs' settings so each one falls back to its default"));
    connect(buttons, &QDialogButtonBox::clicked, this,
            [this, buttons](QAbstractButton* b) {
                switch (buttons->standardButton(b)) {
                case QDialogButtonBox::Close:  accept(); break;
                case QDialogButtonBox::Cancel: reject(); break;
                case QDialogButtonBox::RestoreDefaults: restoreDefaults(); break;
                default: break;
                }
            });
    lay->addWidget(buttons);
}

// Cancel, Escape and the window's X all land here, and all three mean the same
// thing: undo. Routing the revert through reject() rather than through the
// Cancel button's handler is what makes that true — otherwise Escape would
// quietly KEEP changes that the identically-placed Cancel button discards,
// which is the kind of difference nobody discovers until it costs them.
void SettingsDialog::reject()
{
    revertToSnapshot();
    QDialog::reject();
}

QWidget* SettingsDialog::buildExportTab()
{
    // Ported from D4AssetBrowser's Export tab: four sub-pages plus an
    // always-visible "All exports" box. Every control writes its QSettings key
    // LIVE, so a change applies to the very next export without an OK round-trip.
    auto* w = new QWidget(this);
    auto* lay = new QVBoxLayout(w);

    // Bold sub-heading INSIDE a group box — a group box nested in a group box
    // inside a tab is three borders.
    auto sectionLabel = [](QWidget* parent, const QString& text) {
        auto* l = new QLabel(QStringLiteral("<b>%1</b>").arg(text), parent);
        l->setStyleSheet(QStringLiteral("color:#aaa;margin-top:8px;"));
        return l;
    };
    // The checkbox factory: field column, blank label column, live-persisting.
    auto exChk = [this](QFormLayout* form, const char* key, const QString& label,
                        bool def, const QString& tip) {
        auto* cb = new QCheckBox(label, form->parentWidget());
        if (!tip.isEmpty()) cb->setToolTip(tip);
        cb->setChecked(QSettings().value(QLatin1String(key), def).toBool());
        connect(cb, &QCheckBox::toggled, this, [key](bool on) {
            QSettings().setValue(QLatin1String(key), on);
        });
        form->addRow(QString(), cb);
        return cb;
    };
    auto tplRow = [this](QFormLayout* form, const QString& label, const char* key,
                         const QString& def, const QString& tip) {
        auto* le = new QLineEdit(form->parentWidget());
        le->setText(QSettings().value(QLatin1String(key), def).toString());
        le->setPlaceholderText(def);
        le->setToolTip(tip);
        // Blanking the field restores the DEFAULT rather than storing empty —
        // an empty template would produce nameless files.
        connect(le, &QLineEdit::textChanged, this, [key, def](const QString& t) {
            QSettings().setValue(QLatin1String(key),
                                 t.trimmed().isEmpty() ? def : t);
        });
        form->addRow(label, le);
        return le;
    };

    // Each sub-page scrolls on its own: the Models page is taller than a
    // modest window and would otherwise clip its last section.
    auto scrollWrap = [this](QWidget* page) -> QWidget* {
        auto* sc = new QScrollArea(this);
        sc->setWidgetResizable(true);
        sc->setFrameShape(QFrame::NoFrame);
        sc->setWidget(page);
        return sc;
    };

    auto* sub = new QTabWidget(w);
    sub->setDocumentMode(true);

    // ── Models ─────────────────────────────────────────────────────────────
    auto* pgModels = new QWidget(sub);
    {
        auto* pv = new QVBoxLayout(pgModels);
        pv->setContentsMargins(8, 8, 8, 8);
        pv->setSpacing(8);
        auto* box = new QGroupBox(QStringLiteral("Model export (.glb)"), pgModels);
        auto* f = new QFormLayout(box);

        f->addRow(sectionLabel(box, QStringLiteral("Geometry")));
        exChk(f, "export/hiddenParts",
              QStringLiteral("Export parts hidden in the viewport"), false,
              QStringLiteral(
                  "Export every submesh even when the PARTS panel is hiding some.\n\n"
                  "Off (the default) means the export matches what you see."));
        exChk(f, "export/attachments",
              QStringLiteral("Include loaded attachments"), true,
              QStringLiteral(
                  "When a model has attachment parts loaded (a body's tail, extra\n"
                  "pieces), bake them into the .glb too — each with its own skeleton\n"
                  "and its clip matching the exported animation."));
        exChk(f, "export/cloth",
              QStringLiteral("Bake cloth physics into the exported clip"), true,
              QStringLiteral(
                  "Simulate soft-body bones (cape / tail / hair) and bake the result\n"
                  "into the exported animation, so the file matches the viewport.\n\n"
                  "Same setting as the viewport's Cloth toggle — what you see is what\n"
                  "you export."));

        f->addRow(sectionLabel(box, QStringLiteral("Textures")));
        exChk(f, "export/includeTex",
              QStringLiteral("Embed textures in the .glb"), true,
              QStringLiteral(
                  "Embed the decoded material textures in the exported .glb so it is\n"
                  "one self-contained file. Off writes geometry and rig only."));
        exChk(f, "export/looseTextures",
              QStringLiteral("Also write the maps as PNGs beside it"), false,
              QStringLiteral(
                  "Saves the exported maps as separate PNG files in a textures\\ folder\n"
                  "beside the .glb:\n\n"
                  "    <model>_basecolor.png\n"
                  "    <model>_normal.png\n"
                  "    <model>_mix.png\n"
                  "    <model>_emissive.png\n\n"
                  "The .glb does not change — it keeps its embedded copies and stays\n"
                  "self-contained. This is purely for when you also want loose files.\n\n"
                  "\"mix\" is one image holding three maps in its channels: roughness in\n"
                  "red, metalness in green, ambient occlusion in blue."));

        f->addRow(sectionLabel(box, QStringLiteral("Folder layout")));
        {
            auto* combo = new QComboBox(box);
            combo->addItem(QStringLiteral("Flat — everything in the folder you chose"),
                           ExportLayout::kFlat());
            combo->addItem(QStringLiteral("Subfolders by type"), ExportLayout::kType());
            combo->addItem(QStringLiteral("Subfolders by game folder"),
                           ExportLayout::kFolder());
            combo->addItem(QStringLiteral("Subfolders by model"), ExportLayout::kModel());
            const int at = combo->findData(ExportLayout::mode());
            combo->setCurrentIndex(at < 0 ? 0 : at);
            combo->setToolTip(QStringLiteral(
                "How an export of SEVERAL models arranges them. Whichever you pick,\n"
                "each folder gets the same layout inside it: the models, plus\n"
                "textures\\ and deps\\ for whichever of those you have switched on.\n\n"
                "Subfolders by model is the one to pick when you want each asset\n"
                "self-contained — its .glb, its textures and its raw sources\n"
                "together, ready to move elsewhere.\n\n"
                "Exporting a SINGLE model ignores this: there is nothing to group,\n"
                "and quietly turning one file into a folder is not what you asked for."));
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [combo](int i) {
                        QSettings().setValue(QStringLiteral("export/folderLayout"),
                                             combo->itemData(i).toString());
                    });
            f->addRow(QStringLiteral("Multi-model exports"), combo);
        }

        f->addRow(sectionLabel(box, QStringLiteral("Animations to embed")));
        exChk(f, "export/includeAnim",
              QStringLiteral("Include animation"), false,
              QStringLiteral(
                  "Master switch for the sources below: with this off, a model export\n"
                  "embeds none of them."));
        exChk(f, "export/animOriginal",
              QStringLiteral("Original animations (found next to the model)"), true,
              QStringLiteral(
                  "Clips in the model's own folder (its /ani/, walking up).\n\n"
                  "A class body has these. A single armour piece usually has NONE of\n"
                  "its own — which is what \"Base animations\" below is for."));
        exChk(f, "export/animPreviewed",
              QStringLiteral("Previewed animation (the clip playing now)"), true,
              QStringLiteral(
                  "The single clip currently playing in the viewport. Applies to the\n"
                  "loaded model; a batch item that is not loaded has no previewed clip."));
        exChk(f, "export/animBase",
              QStringLiteral("Base animations (the class rig's clips)"), false,
              QStringLiteral(
                  "The class rig's clips, which a worn piece INHERITS — armour carries\n"
                  "no animation of its own, so this is the source that matters in the\n"
                  "Wardrobe.\n\n"
                  "Deduped against \"Original\" by name: a clip the model already owns is\n"
                  "never embedded twice."));

        f->addRow(sectionLabel(box, QStringLiteral("Clip filters")));
        {
            auto* sp = new QSpinBox(box);
            sp->setRange(0, 5000);
            sp->setSingleStep(25);
            sp->setSuffix(QStringLiteral(" frames"));
            sp->setSpecialValueText(QStringLiteral("no limit"));
            sp->setValue(QSettings().value(QStringLiteral("export/animMaxFrames"), 0).toInt());
            sp->setToolTip(QStringLiteral(
                "Skip clips longer than this. 0 = keep every length.\n\n"
                "A handful of very long clips (cutscene poses, character-select loops)\n"
                "can dominate an export's size. A cap around 200-300 keeps everything\n"
                "you would realistically animate with."));
            connect(sp, qOverload<int>(&QSpinBox::valueChanged), this, [](int v) {
                QSettings().setValue(QStringLiteral("export/animMaxFrames"), v);
            });
            f->addRow(QStringLiteral("Skip clips longer than:"), sp);
            auto* le = new QLineEdit(box);
            le->setText(QSettings().value(QStringLiteral("export/animExclude")).toString());
            le->setPlaceholderText(QStringLiteral("e.g.  _emote_, _ui_, _dying"));
            le->setToolTip(QStringLiteral(
                "Comma-separated. A clip whose name CONTAINS any of these\n"
                "(case-insensitive) is not embedded.\n\nLeave empty to keep everything."));
            connect(le, &QLineEdit::textChanged, this, [](const QString& t) {
                QSettings().setValue(QStringLiteral("export/animExclude"), t);
            });
            f->addRow(QStringLiteral("Exclude names containing:"), le);
            auto* note = new QLabel(
                QStringLiteral("<i>Filters apply to Original and Base. A clip you are "
                               "previewing is an explicit choice and is never "
                               "filtered out.</i>"), box);
            note->setWordWrap(true);
            note->setStyleSheet(QStringLiteral("color:#888;"));
            f->addRow(note);
        }

        f->addRow(sectionLabel(box, QStringLiteral("Files written")));
        exChk(f, "export/bothGenders",
              QStringLiteral("Also export the matching opposite-gender item"), false,
              QStringLiteral(
                  "Export the male AND female version of whatever you export, as two\n"
                  "files suffixed _M and _F.\n\n"
                  "Wardrobe: the whole outfit is exported again with every piece\n"
                  "swapped for its opposite-gender twin.\n\n"
                  "Every other option here still applies to BOTH files. A class with no\n"
                  "opposite-gender twin in the data is exported once."));
        exChk(f, "export/withDeps",
              QStringLiteral("Also export raw game files (mesh / material / textures)"),
              false,
              QStringLiteral(
                  "Alongside the exported .glb, write the RAW game files it came from —\n"
                  "its mesh, material, skeleton and every texture — into a \"deps\"\n"
                  "subfolder, exactly as the archive stores them (ZZZ4 containers are\n"
                  "inflated, so the bytes are what this tool actually parses).\n\n"
                  "ONE deps folder per output folder, shared by every model written\n"
                  "beside it, so a texture that ten pieces reference is stored once\n"
                  "rather than ten times.\n\n"
                  "Each file is named after its repository entry with its type as the\n"
                  "extension, because the archive itself stores these under GUID names."));
        auto* nt = new QLabel(
            QStringLiteral("<i>Ticking more animation sources makes larger .glb files "
                           "and slower exports — every clip is decoded.</i>"), box);
        nt->setWordWrap(true);
        nt->setStyleSheet(QStringLiteral("color:#888;"));
        f->addRow(nt);

        pv->addWidget(box);
        pv->addStretch(1);
    }
    sub->addTab(scrollWrap(pgModels), QStringLiteral("Models"));

    // ── Images ─────────────────────────────────────────────────────────────
    auto* pgImages = new QWidget(sub);
    {
        auto* pv = new QVBoxLayout(pgImages);
        pv->setContentsMargins(8, 8, 8, 8);
        pv->setSpacing(8);
        auto* box = new QGroupBox(QStringLiteral("Image && GIF capture"), pgImages);
        auto* f = new QFormLayout(box);

        f->addRow(sectionLabel(box, QStringLiteral("Still image")));
        exChk(f, "export/transparentBg",
              QStringLiteral("Transparent background (captures)"), false,
              QStringLiteral(
                  "Save captures with an alpha-0 background instead of the viewport\n"
                  "colour. Ignored for JPEG, which has no alpha channel."));
        {
            auto* fmt = new QComboBox(box);
            fmt->addItem(QStringLiteral("PNG  (lossless)"), QStringLiteral("png"));
            fmt->addItem(QStringLiteral("JPEG (small, no alpha)"), QStringLiteral("jpg"));
            if (QImageWriter::supportedImageFormats().contains("webp"))
                fmt->addItem(QStringLiteral("WebP (small, keeps alpha)"),
                             QStringLiteral("webp"));
            const int at = fmt->findData(
                QSettings().value(QStringLiteral("export/imageFormat"),
                                  QStringLiteral("png")).toString());
            fmt->setCurrentIndex(at < 0 ? 0 : at);
            fmt->setToolTip(QStringLiteral(
                "The container a saved preview image uses. The save dialog takes its\n"
                "default extension from here; you can still override it per save."));
            f->addRow(QStringLiteral("Image format:"), fmt);

            auto* q = new QSpinBox(box);
            q->setRange(1, 100);
            q->setValue(qBound(1, QSettings().value(QStringLiteral("export/imageQuality"), 92).toInt(), 100));
            q->setToolTip(QStringLiteral(
                "Compression quality for the lossy formats. PNG ignores it."));
            q->setEnabled(fmt->currentData().toString() != QLatin1String("png"));
            connect(q, qOverload<int>(&QSpinBox::valueChanged), this, [](int v) {
                QSettings().setValue(QStringLiteral("export/imageQuality"), v);
            });
            connect(fmt, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [fmt, q](int i) {
                        const QString v = fmt->itemData(i).toString();
                        QSettings().setValue(QStringLiteral("export/imageFormat"), v);
                        q->setEnabled(v != QLatin1String("png"));
                    });
            f->addRow(QStringLiteral("Image quality:"), q);

            auto* sc = new QComboBox(box);
            for (int p : {25, 50, 75, 100, 200, 300, 400})
                sc->addItem(p > 100
                                ? QStringLiteral("%1%  (supersampled)").arg(p)
                                : QStringLiteral("%1%").arg(p), p);
            const int sat = sc->findData(
                qBound(25, QSettings().value(QStringLiteral("export/imageScale"), 100).toInt(), 400));
            sc->setCurrentIndex(sat < 0 ? 3 : sat);
            sc->setToolTip(QStringLiteral(
                "Output size relative to the viewport. Above 100% the scene is\n"
                "genuinely re-rendered larger and downsampled — not upscaled — so the\n"
                "result is sharper than a resize."));
            connect(sc, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [sc](int i) {
                        QSettings().setValue(QStringLiteral("export/imageScale"),
                                             sc->itemData(i).toInt());
                    });
            f->addRow(QStringLiteral("Image resolution:"), sc);
        }

        f->addRow(sectionLabel(box, QStringLiteral("Animated GIF")));
        {
            auto addSpin = [&](const QString& label, const char* key, int def,
                               int lo, int hi, int step, const QString& suffix,
                               const QString& tip) {
                auto* sp = new QSpinBox(box);
                sp->setRange(lo, hi);
                sp->setSingleStep(step);
                if (!suffix.isEmpty()) sp->setSuffix(suffix);
                sp->setValue(qBound(lo, QSettings().value(QLatin1String(key), def).toInt(), hi));
                sp->setToolTip(tip);
                connect(sp, qOverload<int>(&QSpinBox::valueChanged), this, [key](int v) {
                    QSettings().setValue(QLatin1String(key), v);
                });
                f->addRow(label, sp);
                return sp;
            };
            addSpin(QStringLiteral("GIF frame rate:"), "export/gifFps", 25, 1, 60, 1,
                    QStringLiteral(" fps"),
                    QStringLiteral("Turntable GIFs only — an animation-loop GIF uses the\n"
                                   "clip's own authored rate."));
            addSpin(QStringLiteral("Turntable frames:"), "export/gifTurntableFrames", 48,
                    8, 240, 4, QString(),
                    QStringLiteral("Steps in one full revolution. More frames = smoother\n"
                                   "spin and a bigger file."));
            addSpin(QStringLiteral("GIF scale:"), "export/gifScale", 100, 25, 100, 5,
                    QStringLiteral(" %"),
                    QStringLiteral("Output size as a percentage of the viewport."));
            addSpin(QStringLiteral("GIF colors:"), "export/gifMaxColors", 256, 16, 256, 16,
                    QString(),
                    QStringLiteral("Palette size. Fewer colours make smaller files and a\n"
                                   "coarser image."));
            exChk(f, "export/gifDither",
                  QStringLiteral("Dither (reduces banding)"), true,
                  QStringLiteral(
                      "Ordered dithering, which depends only on pixel position — so the\n"
                      "pattern is identical in every frame and does not crawl as the\n"
                      "model turns."));
            auto* opt = exChk(f, "export/gifOptimize",
                              QStringLiteral("Optimize to target size"), false,
                              QStringLiteral(
                                  "Re-encode (never re-render) with a smaller palette, then\n"
                                  "without dither, then at a lower resolution, until the file\n"
                                  "fits the target below."));
            auto* tgt = addSpin(QStringLiteral("Target size:"), "export/gifTargetMB", 10,
                                1, 200, 1, QStringLiteral(" MB"),
                                QStringLiteral("The size the optimizer aims for."));
            tgt->setEnabled(opt->isChecked());
            connect(opt, &QCheckBox::toggled, tgt, &QWidget::setEnabled);
            exChk(f, "export/gifCropToModel",
                  QStringLiteral("Crop to model  (images and GIFs)"), false,
                  QStringLiteral(
                      "Trim the output to the subject's bounds. The box is computed once\n"
                      "across the whole sequence, so the model does not swim inside a\n"
                      "per-frame crop."));
        }
        pv->addWidget(box);
        pv->addStretch(1);
    }
    sub->addTab(scrollWrap(pgImages), QStringLiteral("Images"));

    // ── Wardrobe & Bulk ────────────────────────────────────────────────────
    auto* pgWard = new QWidget(sub);
    {
        auto* pv = new QVBoxLayout(pgWard);
        pv->setContentsMargins(8, 8, 8, 8);
        pv->setSpacing(8);
        auto* box = new QGroupBox(QStringLiteral("Wardrobe"), pgWard);
        auto* f = new QFormLayout(box);
        {
            auto* combo = new QComboBox(box);
            combo->addItem(QStringLiteral("Everything shown in the preview"), 0);
            combo->addItem(QStringLiteral("Equipped items only"), 1);
            combo->setCurrentIndex(
                qBound(0, QSettings().value(QStringLiteral("export/wardrobeScope"), 0).toInt(), 1));
            combo->setToolTip(QStringLiteral(
                "What goes into an exported outfit.\n\n"
                "Everything shown in the preview — exactly what the viewport displays,\n"
                "including the base body, face, eyes and hair.\n\n"
                "Equipped items only — the gear alone, for fitting onto your own\n"
                "character. The base-body pieces are left out."));
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [](int i) {
                        QSettings().setValue(QStringLiteral("export/wardrobeScope"), i);
                    });
            f->addRow(QStringLiteral("Outfit contents:"), combo);
            exChk(f, "wardrobe/setClearsUnmatched",
                  QStringLiteral("Switching sets empties slots the new set "
                                 "does not fill"), true,
                  QStringLiteral(
                      "Sets are not all the same shape — some ship a hair piece "
                      "or a back weapon, most do not.\n\n"
                      "On (the default), moving to a set that has nothing for a "
                      "slot empties that slot, so you see the new set and only "
                      "the new set.\n\n"
                      "Off leaves whatever was there, which lets you keep a hair "
                      "or weapon you like across several sets — at the cost of "
                      "the previous set's pieces riding along unnoticed."));
            auto* note = new QLabel(
                QStringLiteral("<i>Everything under \"Models\" applies to outfits as well "
                               "— including to both files when \"both genders\" is on. The "
                               "outfit file name is set under \"File names\".</i>"), box);
            note->setWordWrap(true);
            note->setStyleSheet(QStringLiteral("color:#888;"));
            f->addRow(note);
        }
        pv->addWidget(box);

        // ── Export all sets ────────────────────────────────────────────────
        // Three switches that are not independent: they pick a SHAPE, a SOURCE
        // and a FILTER, and some combinations mean something you would not
        // guess from the labels alone. SetsExport::Plan resolves them, and the
        // live summary underneath prints that resolution — so the answer to
        // "what will this actually write" is on screen rather than inferred.
        auto* setsBox = new QGroupBox(QStringLiteral("Export all sets"), pgWard);
        setsBox->setToolTip(QStringLiteral(
            "These affect the \"Export all sets…\" button only. File names come "
            "from the \"File names\" page and \"both genders\" from \"Models\", "
            "the same as every other export."));
        {
            auto* sf = new QFormLayout(setsBox);
            auto* summary = new QLabel(setsBox);
            summary->setWordWrap(true);
            summary->setStyleSheet(QStringLiteral("color:#9a8f78;"));
            auto refresh = [summary] {
                summary->setText(QStringLiteral("<i>Writes: %1</i>")
                                     .arg(SetsExport::Plan::load().describe()));
            };
            auto setsChk = [this, sf, setsBox, refresh](const char* key,
                                                        const QString& label,
                                                        const QString& tip) {
                auto* cb = new QCheckBox(label, setsBox);
                cb->setToolTip(tip);
                cb->setChecked(QSettings().value(QLatin1String(key), false).toBool());
                connect(cb, &QCheckBox::toggled, this, [key, refresh](bool on) {
                    QSettings().setValue(QLatin1String(key), on);
                    refresh();
                });
                sf->addRow(QString(), cb);
                return cb;
            };
            setsChk("export/setsNoWeapons",
                    QStringLiteral("Exclude weapons from outfit export"),
                    QStringLiteral(
                        "Leave the back cosmetic and both hand weapons out of each\n"
                        "set's .glb, so you get the armour on its own.\n\n"
                        "Applies to \"Export all sets…\" only — the single \"Export\n"
                        "outfit\" button still writes exactly what the viewport shows.\n\n"
                        "It does NOT reach into the non-armour pass below: that pass\n"
                        "exists to export those weapons on their own, so filtering both\n"
                        "would leave the two options cancelling each other out."));
            setsChk("export/setsIndividual",
                    QStringLiteral("Export all items individually"),
                    QStringLiteral(
                        "One .glb per PIECE instead of one per set, named through the\n"
                        "model template on the \"File names\" page. Same pieces either\n"
                        "way — a set's ranked mains plus its attachments — just not\n"
                        "assembled into one file.\n\n"
                        "A piece shared by two sets is written once."));
            setsChk("export/setsNonArmorMatches",
                    QStringLiteral("Also export all non-armour set matches"),
                    QStringLiteral(
                        "IN ADDITION to the sets above, every piece whose name carries a\n"
                        "set key, EXCLUDING helmet, chest, shoulders, legs and hair —\n"
                        "one .glb each.\n\n"
                        "A much wider net than the sets themselves: a set resolves to\n"
                        "about five pieces, but a dozen or more weapons can carry its\n"
                        "key. It is how you get every weapon variant of every set\n"
                        "alongside the outfits.\n\n"
                        "Anything already written by the set pass is not written twice."));
            refresh();
            sf->addRow(summary);
        }
        pv->addWidget(setsBox);
        pv->addStretch(1);
    }
    sub->addTab(scrollWrap(pgWard), QStringLiteral("Wardrobe && Bulk"));

    // ── File names ─────────────────────────────────────────────────────────
    auto* pgNames = new QWidget(sub);
    {
        auto* pv = new QVBoxLayout(pgNames);
        pv->setContentsMargins(8, 8, 8, 8);
        pv->setSpacing(8);
        auto* box = new QGroupBox(QStringLiteral("Templates"), pgNames);
        auto* f = new QFormLayout(box);
        const QString phTip = QStringLiteral(
            "Placeholders: {{FileName}} = asset name · {{Id}} = repository index · "
            "{{Meaning}} = decoded cosmetic name");
        tplRow(f, QStringLiteral("Models (.glb):"), "export/nameModel",
               NameTemplate::kModelDefault(),
               phTip + QStringLiteral("\ne.g.  {{FileName}} [{{Id}}]"));
        tplRow(f, QStringLiteral("Textures:"), "export/nameTexture",
               NameTemplate::kTextureDefault(),
               phTip + QStringLiteral("\ne.g.  {{FileName}} [{{Id}}]"));
        tplRow(f, QStringLiteral("Outfits (.glb):"), "export/wardrobeNameTemplate",
               NameTemplate::kOutfitDefault(),
               QStringLiteral(
                   "Wardrobe outfits. Placeholders (case-insensitive):\n"
                   "  {{Class}}      Barbarian, Crusader, …\n"
                   "  {{Gender}}     M or F\n"
                   "  {{Set}}        the equipped set key, or \"custom\"\n"
                   "  {{Name}}       the set's real in-game name when known\n"
                   "  {{Helmet}} {{Chest}} {{Shoulders}} {{Legs}} {{Hair}}\n"
                   "  {{Main}} {{Off}}\n"
                   "  {{Date}}       YYYY-MM-DD\n\n"
                   "Empty slots collapse away instead of leaving stray underscores.\n"
                   "\"Export both genders\" appends _M / _F after this."));
        pv->addWidget(box);
        pv->addStretch(1);
    }
    sub->addTab(scrollWrap(pgNames), QStringLiteral("File names"));

    lay->addWidget(sub, 1);

    // ── All exports (below the sub-tabs, visible on every page) ────────────
    auto* allBox = new QGroupBox(QStringLiteral("All exports"), w);
    {
        auto* f = new QFormLayout(allBox);
        exChk(f, "export/osNotify",
              QStringLiteral("Notify when an export finishes"), true,
              QStringLiteral(
                  "Show a summary when an export or bulk run completes, with a\n"
                  "shortcut to open the folder it wrote to."));
        exChk(f, "export/overwrite",
              QStringLiteral("Overwrite files that are already there"), true,
              QStringLiteral(
                  "On (the default), a run replaces any file of the same name.\n\n"
                  "Off, it writes \"name_2.glb\", \"name_3.glb\" … instead, so a\n"
                  "second run ADDS to a folder rather than rewriting it. Worth\n"
                  "turning off when your file-name template cannot tell two assets\n"
                  "apart — a placeholder that has no value for a given export is\n"
                  "dropped from the name, so more things can share one.\n\n"
                  "This is about files ALREADY ON DISK. Two files in the SAME run\n"
                  "are never allowed to collide either way."));
        exChk(f, "export/writeReport",
              QStringLiteral("Write a report file after a bulk run"), false,
              QStringLiteral(
                  "Drops a timestamped _export_report_<date>.txt beside the output\n"
                  "listing every file written, everything that failed and why,\n"
                  "anything skipped, and the settings the run actually used.\n\n"
                  "A run writes hundreds of files across several threads and the\n"
                  "status line has room for four numbers — \"3 failed\" out of 900\n"
                  "is not something you can act on.\n\n"
                  "Applies to \"Export all sets\", \"Export set matches\" and Bulk\n"
                  "Extract. Each report is timestamped, so a second run never\n"
                  "overwrites the evidence from the first."));
    }
    lay->addWidget(allBox);
    return w;
}

// Viewport / camera settings — their own tab (D4 keeps rendering/camera options
// out of the Export tab).
QWidget* SettingsDialog::buildViewportTab()
{
    auto* w = new QWidget(this);
    auto* lay = new QVBoxLayout(w);
    lay->addWidget(new QLabel(QStringLiteral("<b>Camera</b>"), w));
    auto mkInv = [&](const QString& label, const char* key, const QString& tip) {
        auto* cb = new QCheckBox(label, w);
        cb->setChecked(QSettings().value(QLatin1String(key), false).toBool());
        cb->setToolTip(tip);
        connect(cb, &QCheckBox::toggled, this, [key](bool on) {
            QSettings().setValue(QLatin1String(key), on);
        });
        lay->addWidget(cb);
    };
    mkInv(QStringLiteral("Invert rotate — horizontal (left-drag X)"),
          "view/invOrbitX",
          QStringLiteral("Reverse the left-drag left/right rotation."));
    mkInv(QStringLiteral("Invert rotate — vertical (left-drag Y)"),
          "view/invOrbitY",
          QStringLiteral("Reverse the left-drag up/down rotation."));
    mkInv(QStringLiteral("Invert pan (right/middle-drag)"),
          "view/invPan",
          QStringLiteral("Reverse the right/middle-drag panning direction."));

    lay->addSpacing(10);
    lay->addWidget(new QLabel(QStringLiteral("<b>Shading</b>"), w));
    auto* pbr = new QCheckBox(QStringLiteral("Enhanced (PBR) lighting"), w);
    pbr->setChecked(QSettings().value(QStringLiteral("view/pbr"), true).toBool());
    pbr->setToolTip(QStringLiteral(
        "Studio 3-light physically-based shading with soft tone-mapping.\n"
        "Uncheck for the flat legacy headlight look."));
    connect(pbr, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("view/pbr"), on);
    });
    lay->addWidget(pbr);

    auto* expRow = new QWidget(w);
    auto* expLay = new QHBoxLayout(expRow);
    expLay->setContentsMargins(0, 0, 0, 0);
    expLay->addWidget(new QLabel(QStringLiteral("Exposure"), expRow));
    auto* exp = new QSlider(Qt::Horizontal, expRow);
    exp->setRange(20, 400);   // 0.20 .. 4.00 ×100
    exp->setValue(qBound(20,
        int(QSettings().value(QStringLiteral("view/exposure"), 1.0).toDouble() * 100.0), 400));
    exp->setToolTip(QStringLiteral("Brightness of the PBR lighting (applies on the next repaint)."));
    auto* expVal = new QLabel(expRow);
    auto showExp = [expVal](int v) { expVal->setText(QStringLiteral("%1").arg(v / 100.0, 0, 'f', 2)); };
    showExp(exp->value());
    connect(exp, &QSlider::valueChanged, this, [showExp](int v) {
        QSettings().setValue(QStringLiteral("view/exposure"), v / 100.0);
        showExp(v);
    });
    expLay->addWidget(exp, 1);
    expLay->addWidget(expVal);
    lay->addWidget(expRow);

    lay->addStretch(1);
    return w;
}

// Maintenance — logging + housekeeping (D4 has a Maintenance tab).
QWidget* SettingsDialog::buildMaintenanceTab()
{
    auto* w = new QWidget(this);
    auto* lay = new QVBoxLayout(w);
    lay->addWidget(new QLabel(QStringLiteral("<b>Diagnostics</b>"), w));
    auto* logCb = new QCheckBox(
        QStringLiteral("Write log to file (data/DIAssetBrowser.log)"), w);
    logCb->setChecked(applog::fileLogging());
    logCb->setToolTip(QStringLiteral(
        "Continuously write the diagnostic log to data/DIAssetBrowser.log.\n"
        "Truncated on each launch and size-capped, so it never bloats. Handy for\n"
        "sharing what the tool saw without using Help > Export log each time."));
    connect(logCb, &QCheckBox::toggled, this,
            [](bool on) { applog::setFileLogging(on); });
    lay->addWidget(logCb);
    auto* logPath = new QLabel(
        QStringLiteral("<small>%1</small>").arg(applog::logPath().toHtmlEscaped()), w);
    logPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    logPath->setStyleSheet(QStringLiteral("color:#888;"));
    lay->addWidget(logPath);

    // ── Prerequisites ──────────────────────────────────────────────────────
    // Two things can stop a portable copy running on a machine that is not the
    // one it was built on, and Windows reports the first of them with a bare
    // "the code execution cannot proceed" box before main() runs — at which
    // point nothing in this app can tell you anything. Shown here so that once
    // the app IS running, it can at least say what a copy of itself would need.
    lay->addSpacing(10);
    lay->addWidget(new QLabel(QStringLiteral("<b>Prerequisites</b>"), w));
    {
        auto* status = new QLabel(w);
        status->setWordWrap(true);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* fixBtn = new QPushButton(
            QStringLiteral("Download the Visual C++ runtime…"), w);
        fixBtn->setStyleSheet(QLatin1String(kPushBtnQss));
        auto* bar = new QProgressBar(w);
        bar->setVisible(false);
        bar->setTextVisible(true);

        const auto refresh = [status, fixBtn] {
            const Prereqs::Check checks[] = {Prereqs::checkMsvcRuntime(),
                                             Prereqs::checkOpenGl()};
            QString html;
            bool anyFixable = false;
            for (const Prereqs::Check& c : checks) {
                QString mark, colour;
                switch (c.state) {
                case Prereqs::State::Ok:
                    mark = QStringLiteral("OK");       colour = QStringLiteral("#7fb069"); break;
                case Prereqs::State::Missing:
                    mark = QStringLiteral("MISSING");  colour = QStringLiteral("#c76b63"); break;
                case Prereqs::State::Degraded:
                    mark = QStringLiteral("TOO OLD");  colour = QStringLiteral("#d0a94a"); break;
                default:
                    mark = QStringLiteral("UNKNOWN");  colour = QStringLiteral("#9a9a9a"); break;
                }
                html += QStringLiteral(
                            "<p style='margin:2px 0'><b>%1</b> — "
                            "<span style='color:%2'>%3</span><br>"
                            "<span style='color:#9a9a9a'>%4</span>")
                            .arg(c.name.toHtmlEscaped(), colour, mark,
                                 c.detail.toHtmlEscaped());
                if (!c.advice.isEmpty())
                    html += QStringLiteral("<br><span style='color:#b8b8b8'>%1</span>")
                                .arg(c.advice.toHtmlEscaped());
                html += QStringLiteral("</p>");
                if (c.fixable && c.state != Prereqs::State::Ok) anyFixable = true;
            }
            status->setText(html);
            // The button is offered even when nothing is missing — reinstalling
            // the runtime is harmless, and someone packaging this for a friend
            // may want it before hitting the problem.
            fixBtn->setText(anyFixable
                                ? QStringLiteral("Download and install it now…")
                                : QStringLiteral("Reinstall the Visual C++ runtime…"));
        };
        refresh();
        lay->addWidget(status);
        lay->addWidget(bar);

        auto* row = new QHBoxLayout();
        row->addWidget(fixBtn);
        auto* recheck = new QPushButton(QStringLiteral("Re-check"), w);
        recheck->setStyleSheet(QLatin1String(kPushBtnQss));
        connect(recheck, &QPushButton::clicked, this, [refresh] { refresh(); });
        row->addWidget(recheck);
        row->addStretch(1);
        lay->addLayout(row);

        // The URL is taken from Prereqs::vcRedistUrl(), not retyped: a hardcoded
        // copy here silently became a lie the moment the real link changed.
        auto* note = new QLabel(
            QStringLiteral("<small>This is the only thing the app ever downloads. "
                           "The installer comes straight from Microsoft "
                           "(<code>%1</code>), "
                           "goes to your temp folder — never into <code>data\\</code> "
                           "— and is run by you. Everything else works offline."
                           "</small>")
                .arg(Prereqs::vcRedistUrl().mid(QStringLiteral("https://").size())), w);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#888;"));
        lay->addWidget(note);

        connect(fixBtn, &QPushButton::clicked, this,
                [this, fixBtn, bar, refresh] {
                    auto* f = new Prereqs::Fetcher(this);
                    fixBtn->setEnabled(false);
                    bar->setVisible(true);
                    bar->setRange(0, 0);          // indeterminate until a size arrives
                    bar->setFormat(QStringLiteral("downloading…"));
                    connect(f, &Prereqs::Fetcher::progress, bar,
                            [bar](qint64 got, qint64 total) {
                                if (total > 0) {
                                    bar->setRange(0, 100);
                                    bar->setValue(int(got * 100 / total));
                                    bar->setFormat(
                                        QStringLiteral("%1 / %2 MB")
                                            .arg(got / 1048576.0, 0, 'f', 1)
                                            .arg(total / 1048576.0, 0, 'f', 1));
                                }
                            });
                    connect(f, &Prereqs::Fetcher::finished, this,
                            [this, f, fixBtn, bar, refresh](const QString& err,
                                                            const QString& path) {
                                bar->setVisible(false);
                                fixBtn->setEnabled(true);
                                f->deleteLater();
                                if (!err.isEmpty()) {
                                    QMessageBox::warning(
                                        this, QStringLiteral("Download failed"),
                                        QStringLiteral(
                                            "Could not download the Visual C++ "
                                            "runtime:\n\n%1\n\nYou can install it "
                                            "by hand from:\n%2")
                                            .arg(err, Prereqs::vcRedistUrl()));
                                    return;
                                }
                                // Launched detached and NOT waited on: it is an
                                // elevating Microsoft installer, and blocking the
                                // app behind a UAC prompt looks like a freeze.
                                if (!QProcess::startDetached(path, {})) {
                                    QMessageBox::information(
                                        this, QStringLiteral("Downloaded"),
                                        QStringLiteral("Saved to:\n%1\n\nRun it to "
                                                       "install the runtime.").arg(path));
                                    return;
                                }
                                QMessageBox::information(
                                    this, QStringLiteral("Installer started"),
                                    QStringLiteral(
                                        "Microsoft's installer is running. When it "
                                        "finishes, press Re-check.\n\nRestart "
                                        "DIAssetBrowser afterwards if it was the "
                                        "runtime that was missing."));
                                refresh();
                            });
                    f->start(Prereqs::vcRedistUrl(),
                             QStringLiteral("vc_redist.x64.exe"));
                });
    }

    lay->addStretch(1);
    return w;
}

QWidget* SettingsDialog::buildHotkeysTab()
{
    auto* w = new QWidget(this);
    auto* lay = new QVBoxLayout(w);
    auto* note = new QLabel(
        QStringLiteral("Rebind the Export shortcuts. Blank = unbound. Changes "
                       "apply on the next launch (the menu registers them at "
                       "startup)."),
        w);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#9a8f78;"));
    lay->addWidget(note);

    auto* form = new QFormLayout();
    // One row per Hotkeys::def — the SAME list the menu reads, so the editor
    // can never drift from what is actually bound.
    for (const Hotkeys::Def& d : Hotkeys::defs()) {
        auto* row = new QWidget(w);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QKeySequenceEdit(row);
        edit->setKeySequence(Hotkeys::seq(d.key, d.def));
        h->addWidget(edit, 1);
        auto* reset = new QPushButton(QStringLiteral("Default"), row);
        reset->setToolTip(d.def.isEmpty()
                              ? QStringLiteral("Default: unbound")
                              : QStringLiteral("Default: %1").arg(d.def));
        h->addWidget(reset);
        const QString key = d.key, def = d.def;
        auto write = [key](const QKeySequence& s) {
            QSettings().setValue(key, s.toString(QKeySequence::PortableText));
        };
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [write](const QKeySequence& s) { write(s); });
        connect(reset, &QPushButton::clicked, this, [edit, def] {
            edit->setKeySequence(def.isEmpty() ? QKeySequence()
                                               : QKeySequence(def));
        });
        form->addRow(d.label, row);
    }
    lay->addLayout(form);
    lay->addStretch(1);
    return w;
}

QWidget* SettingsDialog::buildHoverTab()
{
    auto* w = new QWidget(this);
    auto* lay = new QVBoxLayout(w);
    auto* note = new QLabel(
        QStringLiteral("Hover-preview popups on the Textures and Models lists. "
                       "These apply immediately — move the cursor to see the "
                       "effect."),
        w);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#9a8f78;"));
    lay->addWidget(note);

    // Small helpers so every control writes its key and nothing else.
    auto boolRow = [&](QFormLayout* form, const QString& label, const char* key,
                       bool def, const QString& tip) {
        auto* cb = new QCheckBox(w);
        cb->setChecked(QSettings().value(QLatin1String(key), def).toBool());
        cb->setToolTip(tip);
        connect(cb, &QCheckBox::toggled, this, [key](bool on) {
            QSettings().setValue(QLatin1String(key), on);
        });
        form->addRow(label, cb);
    };

    // Bold section labels, not nested QGroupBoxes — a frame inside a frame
    // inside a tab is three borders (D4 convention).
    lay->addWidget(new QLabel(QStringLiteral("<b>Thumbnails</b>"), w));
    {
        auto* tf = new QFormLayout();
        boolRow(tf, QStringLiteral("3D icons in the Wardrobe piece lists"),
                "wardrobe/view/thumb3d", false,
                QStringLiteral(
                    "Render each cosmetic as a small 3D preview in the slot "
                    "dropdowns instead of showing its flat base-colour texture. "
                    "Meshes are parsed on worker threads and drawn offscreen; "
                    "icons appear as they finish. Takes effect the next time a "
                    "dropdown is opened."));
        lay->addLayout(tf);
    }

    // ── Wardrobe Set list ──────────────────────────────────────────────────
    // DI ships awakened tiers of a set as separate "_aw<N>" pieces. They used
    // to collapse onto the base set key, which meant no awakened set could ever
    // be equipped as a set. They are now their own entries — and since that can
    // multiply the length of the Set list several times over, which tiers show
    // is a choice.
    lay->addWidget(new QLabel(QStringLiteral("<b>Wardrobe Set list</b>"), w));
    {
        auto* sf = new QFormLayout();
        auto* master = new QCheckBox(w);
        master->setChecked(
            QSettings().value(QStringLiteral("wardrobe/sets/awakening"), false).toBool());
        master->setToolTip(QStringLiteral(
            "List each awakened tier of a set as its own entry, so you can "
            "equip one.\n\n"
            "Off (the default), only base sets are listed — the awakened pieces "
            "are still in the slot dropdowns, there is just no set entry that "
            "puts a whole awakened outfit on at once.\n\n"
            "Takes effect immediately: the class is rescanned when you close "
            "this dialog."));
        sf->addRow(QStringLiteral("Include awakening levels"), master);
        QList<QCheckBox*> tiers;
        for (int i = 1; i <= 3; ++i) {
            auto* cb = new QCheckBox(w);
            const QString key = QStringLiteral("wardrobe/sets/aw%1").arg(i);
            cb->setChecked(QSettings().value(key, true).toBool());
            cb->setToolTip(QStringLiteral(
                "List the tier-%1 awakened version of each set that has one.").arg(i));
            connect(cb, &QCheckBox::toggled, this, [key](bool on) {
                QSettings().setValue(key, on);
            });
            sf->addRow(QStringLiteral("Include Aw%1").arg(i), cb);
            tiers << cb;
        }
        // The per-tier rows only mean anything once the master is on, and a
        // row that cannot do anything should not look like it can.
        const auto syncTiers = [tiers](bool on) {
            for (QCheckBox* cb : tiers) cb->setEnabled(on);
        };
        syncTiers(master->isChecked());
        connect(master, &QCheckBox::toggled, this, [syncTiers](bool on) {
            QSettings().setValue(QStringLiteral("wardrobe/sets/awakening"), on);
            syncTiers(on);
        });
        lay->addLayout(sf);
    }

    lay->addWidget(new QLabel(QStringLiteral("<b>General</b>"), w));
    auto* mf = new QFormLayout();
    boolRow(mf, QStringLiteral("Show hover popups"), "hover/enabled", true,
            QStringLiteral("Master switch for the dwell popup."));
    // dwell delay (double seconds)
    {
        auto* sp = new QDoubleSpinBox(w);
        sp->setRange(0.0, 5.0);
        sp->setSingleStep(0.1);
        sp->setDecimals(1);
        sp->setSuffix(QStringLiteral(" s"));
        sp->setValue(QSettings().value(QStringLiteral("hover/delaySec"), 0.5).toDouble());
        connect(sp, &QDoubleSpinBox::valueChanged, this, [](double v) {
            QSettings().setValue(QStringLiteral("hover/delaySec"), v);
        });
        mf->addRow(QStringLiteral("Dwell before showing"), sp);
    }
    boolRow(mf, QStringLiteral("Include the image"), "hover/imagePreview", true,
            QStringLiteral("Off = info lines only, no thumbnail."));
    boolRow(mf, QStringLiteral("Wheel resizes the popup"), "hover/scrollZoom",
            true, QStringLiteral("Wheel over a visible popup grows/shrinks its "
                                 "image (64..1024 px)."));
    boolRow(mf, QStringLiteral("Colour-code the lines"), "hover/colour", true,
            QStringLiteral("Off = every line in plain grey."));
    // preview px (int slider + spin)
    {
        auto* row = new QWidget(w);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* sl = new QSlider(Qt::Horizontal, row);
        sl->setRange(64, 1024);
        auto* sp = new QSpinBox(row);
        sp->setRange(64, 1024);
        sp->setSuffix(QStringLiteral(" px"));
        const int v = qBound(64, QSettings().value(QStringLiteral("hover/previewPx"), 256).toInt(), 1024);
        sl->setValue(v);
        sp->setValue(v);
        connect(sl, &QSlider::valueChanged, sp, &QSpinBox::setValue);
        connect(sp, &QSpinBox::valueChanged, sl, &QSlider::setValue);
        connect(sp, &QSpinBox::valueChanged, this, [](int nv) {
            QSettings().setValue(QStringLiteral("hover/previewPx"), nv);
        });
        h->addWidget(sl, 1);
        h->addWidget(sp);
        mf->addRow(QStringLiteral("Image size"), row);
    }
    lay->addLayout(mf);

    // Per-line toggles, grouped by tab. Keys match resolveHover() exactly.
    lay->addSpacing(10);
    lay->addWidget(new QLabel(QStringLiteral("<b>Info lines</b>"), w));
    auto* lf = new QFormLayout();
    lf->addRow(new QLabel(QStringLiteral("<b>Textures</b>"), w));
    boolRow(lf, QStringLiteral("Repository path"), "hover/tex/path", true, {});
    boolRow(lf, QStringLiteral("Decoded meaning"), "hover/tex/meaning", true, {});
    boolRow(lf, QStringLiteral("Size and pak"), "hover/tex/info", true, {});
    boolRow(lf, QStringLiteral("Atlas marker"), "hover/tex/atlas", true, {});
    lf->addRow(new QLabel(QStringLiteral("<b>Models</b>"), w));
    boolRow(lf, QStringLiteral("Decoded meaning "), "hover/mdl/meaning", true, {});
    boolRow(lf, QStringLiteral("Repository path "), "hover/mdl/path", true, {});
    boolRow(lf, QStringLiteral("Type, size and pak"), "hover/mdl/info", true, {});
    boolRow(lf, QStringLiteral("Loaded marker"), "hover/mdl/loaded", true, {});
    lay->addLayout(lf);

    lay->addStretch(1);
    return w;
}

QWidget* SettingsDialog::buildInformationTab()
{
    auto* browser = new QTextBrowser(this);
    browser->setOpenExternalLinks(false);
    browser->setHtml(QStringLiteral(R"HTML(
<h2>How this tool works</h2>
<p>Everything below was <b>measured on the live game data</b> — byte-level
verification against the installed PC client, never assumptions carried over
from other games or tools. Where the data refused to give an answer, the tool
says so instead of guessing.</p>

<h3>1. Reading the archive (MPK)</h3>
<p>The PC client stores its 83 GB of content in <code>Package/MPK</code>:
<code>Resources.mpkinfo</code> is the index (677,307 entries: name, offset,
length, pak number) and <code>Resources.mpk</code> … <code>Resources105.mpk</code>
hold the raw bytes. The pak field divided by two selects the file. Most
entries are GUID-named blobs; compressed ones carry a ZZZ4 header (LZ4).
Nothing is written back — the game folder is read-only to this tool.</p>

<h3>2. The resource repository (logical names)</h3>
<p>One special entry, <code>resource.repository</code>, is the game's own
database of 551,524 logical assets: display name, folder, type (Model, Mesh,
Texture2D, Material, SkinSkeleton, …) and a list of dependency hashes. A
"hash bridge" maps each entry's GUID hash to its physical blob. That is how a
friendly name like <code>f_barbarian_yifu_t07_004</code> becomes bytes on
disk. Indexing is cached (metadata only, never asset bytes) and keyed to the
archive signature, so a game patch automatically rebuilds it.</p>

<h3>3. How models are ripped</h3>
<p>Every Model entry has exactly one Mesh and one Material dependency
(measured across all 68,946 Models), plus an optional SkinSkeleton. The mesh
blob is a MESSIAH container: positions, normals and tangents (packed
quaternion frames), UVs, bone indices and weights. The material blob is
key=value text naming the texture GUIDs (<code>tBaseMap</code>,
<code>tNormalMap</code>, <code>tMixMap</code>, <code>tEmissionMap</code>).
Bare Mesh entries with no material of their own are resolved through their
owning Model (a reverse-dependency map covers 79.8% of them). Textures are
BC7 or raw RGBA8; mip chains are stored smallest-first, so the largest mip is
picked by area. The mix map's channels were measured as R=roughness,
G=metallic, B=ambient occlusion.</p>

<h3>4. Skeletons, animation and the 40-bit quaternion</h3>
<p>Each character folder ships a <code>.skeleton</code> property tree: the
authoritative bone hierarchy plus each bone's local rest transform
(accumulating them reproduces the skin bind pose to a few millionths of a
unit). Animations are CHAR::ANIM clips: per-bone tracks of position keys and
rotation keys, the rotations packed into 40 bits — three 12-bit fields plus a
2-bit index of the omitted largest component, reconstructed cyclically. That
law was cracked by measuring rotation continuity across real clips, not by
fitting a single pose. A newer v3 format (the fashion-pose clips) is a 30 fps
frame-block layout; its header, name table and block structure are decoded
and the tool reports exactly what remains unread instead of playing garbage.</p>

<h3>5. The Wardrobe</h3>
<p>The 20 playable rigs are found by data signature: a class folder has its
own skeleton and at least 25 chest pieces. All pieces of one character are
modelled in a single shared space, so equipping is just drawing the selected
parts together; one clip drives every part through its own skin subset.
Armor sets are multi-piece groups keyed by tokens like <code>t07_004</code>:
one main garment per slot plus matching sub-attachments (chest-back pieces,
shoulder parts, neck, bracers), which the set picker equips as a complete
outfit. Dye and Awakened variants are sibling materials over the same mesh.
Hand weapons live in a shared item folder, modelled at the origin with a
one-bone skin; the rig's weapon-holder bones place them in hand — the same
transform law drives the viewport and the export.</p>

<h3>6. Exporting (.glb)</h3>
<p>Exports are glTF 2.0 binaries built from the same parsed data the viewport
renders: real inverse-bind matrices, rest poses from the skeleton tree,
per-part materials with the mix map swizzled into glTF's occlusion/roughness/
metallic layout, and (optionally) animation channels written straight from
the decoded clips. The exporter was verified numerically: rest-pose skinning
reproduces the mesh exactly, and animated joint matrices match the in-app
player.</p>

<h3>7. True names — what the client can and cannot say</h3>
<p>The PC client ships <b>no localized item-name text</b>. This was verified
by sweeping the whole install: the MPK has no string tables, the Battle.net
CASC folder is only a launcher bootstrap, and the local caches hold shaders.
Marketing names ("Dishonored Legionnaire") come from the server at runtime.
So the tool derives names from the strongest on-disk signal, in two layers:</p>
<p><b>Structural true names.</b> Asset names follow a strict grammar —
gender, class, slot (pinyin: yifu=chest, toukui=helmet, jianjia=shoulders,
tui=legs…), sub-part, set key, variant. The decoder turns
<code>f_barbarian_yifu_t07_004_aw3</code> into
"Barbarian (F) — Chest · set t07_004 (Awakened III)". 99.2% of all 16,671
cosmetics decode; anything unrecognised is shown in [brackets], never
invented.</p>
<p><b>Real-name overrides.</b> There are only 165 distinct set keys in the
whole game. <code>data\di_item_names.csv</code> maps a set key (optionally
per class) to the real in-game name, and it takes priority everywhere the
Meaning column appears. Names → Export template writes the full key list
ready to fill in; Names → Reload applies it.</p>

<h3>8. Texture Atlases (UI sprite sheets)</h3>
<p>DI's interface is Cocos-based, and its UI sprite sheets ship with real
frame descriptors: for 13,533 of the 13,665 <code>Package\UIScript\*.plist</code>
files there is a repository Texture2D with the same stem — the plist lists
every named sub-image with its exact rectangle (this replaces the D4 tool's
"texframes", which had to segment atlases by alpha because its frame table
carried no UVs; here the rectangles are authored data). When the Textures tab
shows an atlas, its frames appear in a table: click one to preview it, export
selected or all frames as PNGs, optionally trimmed to their alpha bounds.
Rotated frames (stored 90° turned in the sheet) are un-rotated on the way
out. If a descriptor is found but its encoding is not the expected plist XML,
the tab says so verbatim — including the first bytes — rather than guessing
frames.</p>

<h3>9. Bulk extraction</h3>
<p>The Bulk Extract tab turns any filtered query into files: rigged .glb per
model, decoded PNG per texture, or the exact stored bytes. Runs are
incremental — <code>_bulk_manifest.json</code> in the output folder records
what was written, and "Only new" re-runs after a game patch extract just the
additions. Failures land in <code>_bulk_failed.txt</code> with their reasons,
one crash costs one file (each item is individually guarded), and the
persistent queue lets you hand-pick items across different filter passes.
Parallel workers stream different paks concurrently; output is byte-identical
to a single-threaded run.</p>

<h3>10. Visual effects (FX): what the data does and doesn't hold</h3>
<p>DI's cosmetic effects live in the <code>ParticleSystem</code> repository type
(folder <code>EffectCom</code>, ~43,900 of them): each is a ZZZ4-compressed
typed property tree, the same container as meshes and materials, laid out as
<code>Emitters -&gt; Elements</code> with a source (emission rate/duration),
init distributions (lifetime, size, velocity, position), over-life curves
(colour, alpha, size), a sub-UV flipbook, and forces. The format is byte-level
cracked (verified on 497 of 500 sample blobs).</p>
<p><b>The attachment problem.</b> Skills, portals and NPC auras bind their
effects through readable <code>.graph</code> files, so those are recoverable.
<b>Worn gear is not</b>: which effect a cosmetic uses is applied from the
<i>encrypted client item table</i> at equip time and appears in no readable
asset — it can only be recovered by scanning a live process dump for the
effect names resident while the piece is equipped. So the tool never guesses a
piece's particle effect: for the one set confirmed this way (sz11_008
crusader), only the weapon and offhand carry particles; the armour pieces
carry none — their in-game richness is the animated <b>material</b>, not
particles.</p>
<p><b>What the tool renders.</b> The "FX" toggle shows the model material's own
authored animated emissive layer — the arcane scroll, star sparkle and fresnel
rim glow read straight from the material constants. That is true-to-data and
always correct. A separate experiment to re-simulate the <code>ParticleSystem</code>
particles in real time (billboards, flipbooks, twist-warp, bloom) was built and
then removed: without the encrypted binding table and the game's exact shader
setup it could not be made faithful, and a not-quite-right fire is worse than
none. The full research — parser, runtime, the two load-bearing findings
(sprite <code>Patch</code> = authored quad + UV, so no per-effect size guessing;
<code>System.Property.Bias</code> = an offset from the hand hardpoint, not the
model origin) and every confirmed binding — is preserved in
<code>FX_ARCHIVE.md</code> for anyone who later gets the item table, and
summarised in <code>docs/FX_NOTES.md</code>.</p>

<h3>11. Ground rules the tool is built on</h3>
<p>Every format claim is measured before code depends on it. Output quality
is never silently degraded — failures are reported, not papered over. Caches
hold derived metadata only, never asset bytes, and are signature-keyed so
game patches invalidate them. The log (<code>data\DIAssetBrowser.log</code>)
records what loaded, how it resolved and how long it took, so the tool's
behaviour is always auditable after the fact.</p>
)HTML"));
    return browser;
}
