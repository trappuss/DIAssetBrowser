#include "util/HoverPreview.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QHoverEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include "util/HoverInfo.h"

HoverPreview::HoverPreview(QAbstractItemView* view, Resolver resolver,
                           QObject* parent)
    : QObject(parent), m_view(view), m_resolver(std::move(resolver))
{
    m_px = HoverInfo::previewPx();
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_popup = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
    m_popup->setAttribute(Qt::WA_ShowWithoutActivating);
    // Never let the preview eat a click. The popup is placed offset from the
    // cursor, but the overflow flip can put it back under the pointer — and
    // over a combo dropdown (which holds a mouse grab) a window that swallows
    // the press means the row you clicked never gets selected.
    m_popup->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_popup->hide();
    QObject::connect(m_timer, &QTimer::timeout, m_popup, [this] {
        if (m_row >= 0) showFor(m_view->model()->index(m_row, 0));
    });
    m_view->viewport()->setMouseTracking(true);
    m_view->viewport()->installEventFilter(this);
    // Popup goes down with the view, not with a possibly-later parent.
    QObject::connect(m_view, &QObject::destroyed, m_popup, &QLabel::deleteLater);
}

void HoverPreview::hidePopup()
{
    m_timer->stop();
    m_popup->hide();
    m_row = -1;
}

bool HoverPreview::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj != m_view->viewport()) return QObject::eventFilter(obj, ev);
    switch (ev->type()) {
    case QEvent::MouseMove: {
        if (!HoverInfo::enabled()) break;
        auto* me = static_cast<QMouseEvent*>(ev);
        m_lastGlobal = me->globalPosition().toPoint();
        const QModelIndex idx = m_view->indexAt(me->pos());
        const int row = idx.isValid() ? idx.row() : -1;
        if (row != m_row) {
            m_popup->hide();
            m_row = row;
            if (row >= 0) {
                m_timer->setInterval(HoverInfo::delayMs());
                m_timer->start();
            } else {
                m_timer->stop();
            }
        } else if (m_popup->isVisible()) {
            place();   // follow the cursor along the row
        }
        break;
    }
    case QEvent::Wheel: {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (m_popup->isVisible() && HoverInfo::scrollZoom() &&
            !(we->modifiers() & Qt::ControlModifier)) {
            // resize the preview in place; never scroll the list under it
            m_px = qBound(64, m_px + (we->angleDelta().y() > 0 ? 24 : -24), 1024);
            if (m_row >= 0) showFor(m_view->model()->index(m_row, 0));
            return true;
        }
        hidePopup();   // a real scroll moves the rows out from under us
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::Leave:
    case QEvent::Hide:
        hidePopup();
        break;
    default:
        break;
    }
    return QObject::eventFilter(obj, ev);
}

void HoverPreview::refresh()
{
    if (m_popup->isVisible() && m_row >= 0)
        showFor(m_view->model()->index(m_row, 0));
}

void HoverPreview::showFor(const QModelIndex& idx)
{
    if (!idx.isValid() || !m_resolver) return;
    QList<Line> lines;
    QImage img;
    if (!m_resolver(idx, &lines, &img)) {
        m_popup->hide();
        return;
    }
    if (lines.isEmpty() && img.isNull()) {
        m_popup->hide();
        return;
    }
    const bool colour = HoverInfo::colourCode();
    const bool wantImg = HoverInfo::imagePreview() && !img.isNull();

    // ── compose: image on top, caption strip beneath (D4 layout) ───────────
    const QFontMetrics fm = m_popup->fontMetrics();
    const int lineH = fm.height() + 2;
    QPixmap imgPm;
    if (wantImg) {
        imgPm = QPixmap::fromImage(img).scaled(
            m_px, m_px, Qt::KeepAspectRatio,
            img.width() >= m_px ? Qt::SmoothTransformation
                                : Qt::FastTransformation);
    }
    int w = wantImg ? imgPm.width() : 0;
    for (const Line& l : lines)
        w = qMax(w, fm.horizontalAdvance(l.text) + 16);
    w = qBound(200, w, 900);
    const int capH = lines.isEmpty() ? 0 : lines.size() * lineH + 8;
    const int h = (wantImg ? imgPm.height() : 0) + capH;

    QPixmap pm(w, h);
    pm.fill(QColor(0x1a, 0x1a, 0x1c, 235));
    {
        QPainter p(&pm);
        int y = 0;
        if (wantImg) {
            p.fillRect(0, 0, w, imgPm.height(), QColor(0x11, 0x11, 0x12));
            p.drawPixmap((w - imgPm.width()) / 2, 0, imgPm);
            y = imgPm.height();
        }
        y += 4;
        for (const Line& l : lines) {
            p.setPen(colour ? l.color : QColor(0xcc, 0xcc, 0xcc));
            p.drawText(QRect(8, y, w - 16, lineH),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       fm.elidedText(l.text, Qt::ElideMiddle, w - 16));
            y += lineH;
        }
    }
    m_popup->setPixmap(pm);
    m_popup->resize(pm.size());
    place();
    m_popup->show();
}

void HoverPreview::place()
{
    // +18,+18 from the cursor; flip to the other side near an edge; clamp to
    // the app window ∩ the screen (D4 placement rule).
    QPoint at = m_lastGlobal + QPoint(18, 18);
    QRect bound;
    if (QScreen* scr = m_view->screen())
        bound = scr->availableGeometry();
    if (QWidget* win = m_view->window())
        bound = bound.isNull() ? win->frameGeometry()
                               : bound.intersected(win->frameGeometry());
    if (!bound.isNull()) {
        if (at.x() + m_popup->width() > bound.right())
            at.setX(m_lastGlobal.x() - 18 - m_popup->width());
        if (at.y() + m_popup->height() > bound.bottom())
            at.setY(m_lastGlobal.y() - 18 - m_popup->height());
        at.setX(qBound(bound.left(), at.x(),
                       qMax(bound.left(), bound.right() - m_popup->width())));
        at.setY(qBound(bound.top(), at.y(),
                       qMax(bound.top(), bound.bottom() - m_popup->height())));
    }
    m_popup->move(at);
}
