#pragma once
// The one filter state shared by every tab: search text (space = AND,
// "-term" = exclude) plus facet sets. An EMPTY set means "no constraint";
// within a set values OR, between groups they AND — the D4 funnel semantics.

#include <QSet>
#include <QString>

struct FilterSpec {
    // Animation constraint (Models tab). Rows carry a precomputed clip count,
    // so this costs nothing per row.
    enum Anim { AnimAny = 0, AnimOnly = 1, StaticOnly = 2 };

    // Player-content constraint. "Player" is anything a character can wear or
    // hold — class folders, the shared gear folder, hair/faces (NameTranslator).
    enum Who { WhoAny = 0, PlayerOnly = 1, NonPlayerOnly = 2 };

    QString        text;
    QSet<QString>  types;      // repository type / extension ("Mesh", "png", …)
    QSet<QString>  cats;       // first path segment  ("Char", "World", "ui", …)
    QSet<QString>  subcats;    // second path segment ("f_barbarian", …)
    QSet<QString>  classes;    // player class  ("Barbarian", "Crusader", …)
    QSet<QString>  cosSlots;   // cosmetic slot ("Chest", "Helmet", "Weapon", …)
    int            anim = AnimAny;
    int            who  = WhoAny;

    bool operator==(const FilterSpec& o) const
    {
        return text == o.text && types == o.types && cats == o.cats &&
               subcats == o.subcats && classes == o.classes && cosSlots == o.cosSlots &&
               anim == o.anim && who == o.who;
    }
};
