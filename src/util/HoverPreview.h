#pragma once
// Hover-preview popup — the D4 browser's dwell popup, extracted into ONE
// shared engine so every list gets identical behaviour:
//
//   * dwell HoverInfo::delayMs() over a row, then a floating tooltip-style
//     popup: optional thumbnail on top, colour-coded info lines beneath
//   * wheel over the visible popup resizes the image ±24 px (64..1024,
//     gated by hover/scrollZoom) WITHOUT re-dwelling; the event is consumed
//     so the list under it does not scroll
//   * placement: +18,+18 from the cursor, flipped to the other side when it
//     would overflow, then clamped inside the window ∩ screen (D4 rule)
//   * hidden on leave, on any click, and on scroll
//
// The owning tab supplies a RESOLVER: given the hovered index, fill the info
// lines and (optionally) an image. The resolver runs on the GUI thread and
// must be cheap — a tab that wants a real decoded image kicks its own async
// decode and calls refresh() when the pixels arrive (see TexturesTab).
//
// Plain QObject (no Q_OBJECT: no signals/slots of its own) — everything runs
// through the event filter installed on the view's viewport.

#include <QColor>
#include <QModelIndex>
#include <QObject>
#include <QPoint>
#include <QString>

#include <functional>

class QAbstractItemView;
class QEvent;
class QLabel;
class QTimer;

class HoverPreview : public QObject {
public:
    struct Line {
        QString text;
        QColor  color;
    };
    // Return false = nothing to show for this index (popup stays hidden).
    // `image` may be left null for an info-only popup.
    using Resolver =
        std::function<bool(const QModelIndex&, QList<Line>*, QImage*)>;

    HoverPreview(QAbstractItemView* view, Resolver resolver, QObject* parent);

    // Re-run the resolver for the row the popup currently shows (async image
    // arrivals). No-op unless the popup is visible on that same row.
    void refresh();
    // Row the popup is showing (or dwelling towards); -1 = none.
    int  currentRow() const { return m_row; }
    void hidePopup();

    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void showFor(const QModelIndex& idx);
    void place();

    QAbstractItemView* m_view;
    Resolver m_resolver;
    QTimer*  m_timer;    // single-shot dwell
    QLabel*  m_popup;    // Qt::ToolTip window
    int      m_row = -1;
    QPoint   m_lastGlobal;
    int      m_px = 256;   // current image edge (wheel-resizable)
};
