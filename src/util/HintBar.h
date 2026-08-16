#pragma once
// Ported from the D4 browser. One-line dismissible first-run tip pinned to the top
// of a tab. Once dismissed it never returns (per-key settings flag). Deliberately
// quiet styling — a hint, not a banner — because the features it teaches (hotkeys,
// part selection, wheel gestures) are otherwise invisible.

#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QSettings>
#include <QToolButton>
#include <QWidget>

// Returns the bar, or nullptr when this hint was already dismissed:
//   if (auto* b = makeHintBar(this, tip, "hints/textures")) layout->addWidget(b);
inline QWidget* makeHintBar(QWidget* parent, const QString& text, const char* settingsKey)
{
    if (QSettings().value(QLatin1String(settingsKey), false).toBool())
        return nullptr;
    auto* bar = new QWidget(parent);
    bar->setStyleSheet(QStringLiteral("background:#26241e;"));
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(8, 2, 4, 2);
    h->setSpacing(6);
    auto* lbl = new QLabel(text, bar);
    lbl->setStyleSheet(QStringLiteral("color:#b9a877;background:transparent;border:none;"));
    h->addWidget(lbl, 1);
    auto* x = new QToolButton(bar);
    x->setText(QStringLiteral("✕"));   // ✕ — same dismiss glyph the FilterBar chips use
    x->setAutoRaise(true);
    x->setCursor(Qt::PointingHandCursor);
    x->setToolTip(QStringLiteral("Got it — don't show this tip again"));
    x->setStyleSheet(QStringLiteral(
        "QToolButton{border:none;background:transparent;color:#8a8a8a;}"
        "QToolButton:hover{color:#e0e0e0;}"));
    QObject::connect(x, &QToolButton::clicked, bar, [bar, settingsKey] {
        QSettings().setValue(QLatin1String(settingsKey), true);
        bar->deleteLater();
    });
    h->addWidget(x);
    return bar;
}
