#pragma once
// ── Status readout: one base line plus bounded, de-duplicated notes ─────────
// The status label above each tab's panel column carries two different kinds of
// text, and they must not fight over it:
//
//   the BASE   what is loaded right now — the piece counts, the clip counts.
//              Replaced wholesale when the selection changes.
//   NOTES      things that happened along the way — a clip that would not
//              parse, a piece with no opposite-gender twin, an export that
//              wrote nothing. Appended under the base.
//
// The Models tab grew this behaviour first; the Wardrobe tab had a bare QLabel
// that every message overwrote, so a batch that hit twelve problems showed only
// the twelfth, and any of them could be wiped by the next routine status line
// before you had read it. This is that mechanism, extracted, so both tabs get
// the same one and it can only be fixed once.
//
// Two properties that matter more than they look:
//   * repeats COLLAPSE. The same note again bumps a counter and moves the line
//     to the end, so 22 clips failing identically read as one line with "x22"
//     rather than 22 lines that push everything else off the panel.
//   * the list is BOUNDED. Old notes fall off the front, because a status
//     readout that grows without limit stops being readable exactly when
//     something is going wrong and you most need to read it.
//
// Notes are HTML fragments (they are rendered into a rich-text QLabel). Callers
// passing text that came from data — a file name, a parser's error string —
// must escape it with QString::toHtmlEscaped() first.

#include <QLabel>
#include <QList>
#include <QString>
#include <QStringList>

namespace infonotes {

class Panel {
public:
    // How many distinct notes are kept. Four fits the panel without scrolling
    // at the default width; older ones drop off the front.
    static constexpr int kMax = 4;

    // The label this panel writes into. Not owned.
    void attach(QLabel* label) { m_label = label; }

    // Replace the base line and drop every note — a new selection's problems
    // are not the old selection's problems.
    void setBase(const QString& html)
    {
        m_base = html;
        m_notes.clear();
        m_counts.clear();
        render();
    }

    void addNote(const QString& html)
    {
        const int at = m_notes.indexOf(html);
        if (at >= 0) {
            const int c = m_counts[at] + 1;
            m_notes.removeAt(at);
            m_counts.removeAt(at);
            m_notes.append(html);
            m_counts.append(c);
        } else {
            m_notes.append(html);
            m_counts.append(1);
            while (m_notes.size() > kMax) {
                m_notes.removeFirst();
                m_counts.removeFirst();
            }
        }
        render();
    }

    void clear()
    {
        m_base.clear();
        m_notes.clear();
        m_counts.clear();
        if (m_label) m_label->clear();
    }

    const QString& base() const { return m_base; }

private:
    void render()
    {
        if (!m_label) return;
        QString s = m_base;
        for (int i = 0; i < m_notes.size(); ++i) {
            s += QStringLiteral("<br><i>%1</i>").arg(m_notes[i]);
            if (m_counts[i] > 1)
                s += QStringLiteral(" <b>x%1</b>").arg(m_counts[i]);
        }
        m_label->setText(s);
    }

    QLabel*     m_label = nullptr;
    QString     m_base;
    QStringList m_notes;
    QList<int>  m_counts;
};

}   // namespace infonotes
