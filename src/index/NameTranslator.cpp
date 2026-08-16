#include "index/NameTranslator.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

namespace NameTranslator {

namespace {

// Curated from the measured token-frequency table (top ~600 tokens across all
// 551,524 repository names, 2026-08-01). Only tokens with a confident reading
// are mapped; ambiguous pinyin stays untranslated rather than guessed.
const QHash<QString, QString>& dict()
{
    static const QHash<QString, QString> d = {
        // ── hand-weapon holders and item weapons (measured 2026-08-02:
        //    Char/item carries 3,638 in-hand weapon models keyed by these
        //    leading tokens; the rig holds them at zhushou_/fushou_ nodes) ──
        {QStringLiteral("zhushou"),     QStringLiteral("main hand")},
        {QStringLiteral("fushou"),      QStringLiteral("off hand")},
        {QStringLiteral("shuangshou"),  QStringLiteral("two-handed")},
        {QStringLiteral("danshou"),     QStringLiteral("one-handed")},
        {QStringLiteral("dun"),         QStringLiteral("shield")},
        {QStringLiteral("gong"),        QStringLiteral("bow")},
        {QStringLiteral("peijian"),     QStringLiteral("attachment")},
        {QStringLiteral("tuteng"),      QStringLiteral("totem")},
        {QStringLiteral("yishijian"),   QStringLiteral("ritual sword")},
        // ── cosmetic / body detail tokens ────────────────────────────────
        {QStringLiteral("wenshi"),      QStringLiteral("ornament")},
        {QStringLiteral("chuncai"),     QStringLiteral("lip colour")},
        {QStringLiteral("hongmo"),      QStringLiteral("iris")},
        {QStringLiteral("shiti"),       QStringLiteral("corpse")},
        {QStringLiteral("juqing"),      QStringLiteral("story")},
        // ── equipment slots / character parts ─────────────────────────────
        {QStringLiteral("yifu"),        QStringLiteral("chest armor")},
        {QStringLiteral("toukui"),      QStringLiteral("helmet")},
        {QStringLiteral("jianjia"),     QStringLiteral("shoulders")},
        {QStringLiteral("tui"),         QStringLiteral("legs")},
        {QStringLiteral("bijia"),       QStringLiteral("bracers")},
        {QStringLiteral("quantao"),     QStringLiteral("fist weapon")},
        {QStringLiteral("toufa"),       QStringLiteral("hair")},
        {QStringLiteral("maofa"),       QStringLiteral("hair")},
        {QStringLiteral("huzi"),        QStringLiteral("beard")},
        {QStringLiteral("jiemao"),      QStringLiteral("eyelashes")},
        {QStringLiteral("meixing"),     QStringLiteral("eyebrows")},
        {QStringLiteral("lian"),        QStringLiteral("face")},
        {QStringLiteral("suyan"),       QStringLiteral("plain face")},
        {QStringLiteral("yanzhuang"),   QStringLiteral("face paint")},
        {QStringLiteral("bozi"),        QStringLiteral("neck")},
        {QStringLiteral("shoubi"),      QStringLiteral("arm")},
        {QStringLiteral("tou"),         QStringLiteral("head")},
        {QStringLiteral("qunzi"),       QStringLiteral("skirt")},
        {QStringLiteral("chibang"),     QStringLiteral("wings")},
        {QStringLiteral("yumao"),       QStringLiteral("feathers")},
        {QStringLiteral("piaodai"),     QStringLiteral("ribbons")},
        {QStringLiteral("beibao"),      QStringLiteral("backpack")},
        {QStringLiteral("shizhuang"),   QStringLiteral("cosmetic")},
        {QStringLiteral("fujian"),      QStringLiteral("accessory")},
        {QStringLiteral("benti"),       QStringLiteral("base body")},
        {QStringLiteral("emotoulu"),    QStringLiteral("demon skull")},
        // ── weapons ───────────────────────────────────────────────────────
        {QStringLiteral("wuqi"),        QStringLiteral("weapon")},
        {QStringLiteral("danshoujian"), QStringLiteral("1h sword")},
        {QStringLiteral("danshoufu"),   QStringLiteral("1h axe")},
        {QStringLiteral("danshouchui"), QStringLiteral("1h mace")},
        {QStringLiteral("danshounu"),   QStringLiteral("1h crossbow")},
        {QStringLiteral("danshoumao"),  QStringLiteral("1h spear")},
        {QStringLiteral("liandao"),     QStringLiteral("scythe")},
        {QStringLiteral("lianren"),     QStringLiteral("scythe blade")},
        {QStringLiteral("lianchui"),    QStringLiteral("flail")},
        {QStringLiteral("fazhang"),     QStringLiteral("staff")},
        {QStringLiteral("faqi"),        QStringLiteral("focus")},
        {QStringLiteral("faqiu"),       QStringLiteral("orb")},
        {QStringLiteral("txinggun"),    QStringLiteral("t-staff")},
        {QStringLiteral("juanzhou"),    QStringLiteral("scroll")},
        {QStringLiteral("xianjian"),    QStringLiteral("immortal sword")},
        // ── items / systems ───────────────────────────────────────────────
        {QStringLiteral("baoshi"),      QStringLiteral("gem")},
        {QStringLiteral("legendarygem"),QStringLiteral("legendary gem")},
        {QStringLiteral("jinghua"),     QStringLiteral("essence")},
        {QStringLiteral("yiwu"),        QStringLiteral("relic")},
        {QStringLiteral("yiwuxitong"),  QStringLiteral("relic system")},
        {QStringLiteral("tujiansuoyin"),QStringLiteral("codex index")},
        {QStringLiteral("shangcheng"),  QStringLiteral("shop")},
        {QStringLiteral("yueka"),       QStringLiteral("monthly pass")},
        {QStringLiteral("teshulibao"),  QStringLiteral("special bundle")},
        {QStringLiteral("jiangli"),     QStringLiteral("reward")},
        {QStringLiteral("jianglibig"),  QStringLiteral("reward (large)")},
        {QStringLiteral("huodong"),     QStringLiteral("event")},
        {QStringLiteral("yunying"),     QStringLiteral("live-ops")},
        {QStringLiteral("yunyinghuodong"), QStringLiteral("live-ops event")},
        {QStringLiteral("wanshengjie"), QStringLiteral("halloween")},
        {QStringLiteral("renwu"),       QStringLiteral("quest")},
        {QStringLiteral("zhandou"),     QStringLiteral("combat")},
        {QStringLiteral("zhenying"),    QStringLiteral("faction")},
        {QStringLiteral("fuben"),       QStringLiteral("dungeon run")},
        {QStringLiteral("maoxianzhe"),  QStringLiteral("adventurer")},
        {QStringLiteral("teshu"),       QStringLiteral("special")},
        {QStringLiteral("jichu"),       QStringLiteral("basic")},
        {QStringLiteral("junei"),       QStringLiteral("in-match")},
        {QStringLiteral("chuchang"),    QStringLiteral("entrance")},
        {QStringLiteral("fenxiang"),    QStringLiteral("share")},
        {QStringLiteral("chaifen"),     QStringLiteral("split")},
        {QStringLiteral("chongzu"),     QStringLiteral("rebuild")},
        {QStringLiteral("kuan"),        QStringLiteral("style")},
        {QStringLiteral("yujing"),      QStringLiteral("warning")},
        {QStringLiteral("toutu"),       QStringLiteral("header image")},
        {QStringLiteral("chatu"),       QStringLiteral("illustration")},
        {QStringLiteral("newchengjiu"), QStringLiteral("achievement (new)")},
        {QStringLiteral("hundunmijing"),QStringLiteral("chaos rift")},
        {QStringLiteral("shengjifazhen"),QStringLiteral("upgrade circle")},
        // ── world / scenery ───────────────────────────────────────────────
        {QStringLiteral("chuansongmen"),QStringLiteral("portal")},
        {QStringLiteral("zuzhouzhita"), QStringLiteral("cursed tower")},
        {QStringLiteral("tushazhidi"),  QStringLiteral("slaughter grounds")},
        {QStringLiteral("emengzhanchang"), QStringLiteral("nightmare battlefield")},
        {QStringLiteral("fengbaozhongxin"), QStringLiteral("storm center")},
        {QStringLiteral("jiguanzhiyu"), QStringLiteral("mechanism realm")},
        {QStringLiteral("shawaerhaungye"),  QStringLiteral("sharval wilds")},
        {QStringLiteral("shawaerhaungye2"), QStringLiteral("sharval wilds 2")},
        {QStringLiteral("shawaerhaungye3"), QStringLiteral("sharval wilds 3")},
        {QStringLiteral("dongxue"),     QStringLiteral("cave")},
        {QStringLiteral("dongku"),      QStringLiteral("cavern")},
        {QStringLiteral("dilao"),       QStringLiteral("dungeon")},
        {QStringLiteral("yaosai"),      QStringLiteral("fortress")},
        {QStringLiteral("jiaotang"),    QStringLiteral("cathedral")},
        {QStringLiteral("jiuchengqu"),  QStringLiteral("old town")},
        {QStringLiteral("shamo"),       QStringLiteral("desert")},
        {QStringLiteral("liusha"),      QStringLiteral("quicksand")},
        {QStringLiteral("xingkong"),    QStringLiteral("starry sky")},
        {QStringLiteral("miwu"),        QStringLiteral("mist")},
        {QStringLiteral("huoyan"),      QStringLiteral("flame")},
        {QStringLiteral("shandian"),    QStringLiteral("lightning")},
        {QStringLiteral("youling"),     QStringLiteral("specter")},
        {QStringLiteral("jingti"),      QStringLiteral("crystal")},
        {QStringLiteral("suishi"),      QStringLiteral("rubble")},
        {QStringLiteral("diban"),       QStringLiteral("floor")},
        {QStringLiteral("deng"),        QStringLiteral("lamp")},
        {QStringLiteral("shu"),         QStringLiteral("tree")},
        {QStringLiteral("gouzi"),       QStringLiteral("hook")},
        {QStringLiteral("lizi"),        QStringLiteral("particles")},
        {QStringLiteral("yanqiu"),      QStringLiteral("eyeball")},
        {QStringLiteral("huangjin"),    QStringLiteral("gold")},
        {QStringLiteral("fazhen"),      QStringLiteral("magic circle")},
        {QStringLiteral("gangu"),       QStringLiteral("dry valley")},
        {QStringLiteral("liuti"),       QStringLiteral("fluid")},
        {QStringLiteral("wenli"),       QStringLiteral("pattern")},
        {QStringLiteral("hepi"),        QStringLiteral("bark")},
        {QStringLiteral("dilie"),       QStringLiteral("ground crack")},
        {QStringLiteral("huiliu"),      QStringLiteral("backflow")},
        {QStringLiteral("caizhibg"),    QStringLiteral("material bg")},
        // ── common colours / animals in compounds ─────────────────────────
        {QStringLiteral("heilang"),     QStringLiteral("black wolf")},
        {QStringLiteral("huolang"),     QStringLiteral("fire wolf")},
        {QStringLiteral("langrensha"),  QStringLiteral("werewolf game")},
        // ── generic markers ───────────────────────────────────────────────
        {QStringLiteral("da"),          QStringLiteral("large")},
        {QStringLiteral("tuijianda"),   QStringLiteral("featured (large)")},
    };
    return d;
}

} // namespace

namespace {

// Measured cosmetic vocabulary (see NameTranslator.h). Longest class names
// first so "bloodknight" isn't shadowed by a shorter prefix.
const char* const kCosClasses[] = {
    "bloodknight", "demonhunter", "necromancer", "barbarian", "crusader",
    "sorceress", "wizard", "tempest", "warlock", "druid", "monk"};

const QHash<QString, QString>& slotGloss()
{
    static const QHash<QString, QString> d = {
        {QStringLiteral("yifu"), QStringLiteral("Chest")},
        {QStringLiteral("toukui"), QStringLiteral("Helmet")},
        {QStringLiteral("jianjia"), QStringLiteral("Shoulders")},
        {QStringLiteral("tui"), QStringLiteral("Legs")},
        {QStringLiteral("toufa"), QStringLiteral("Hair")},
        {QStringLiteral("bijia"), QStringLiteral("Bracers")},
        {QStringLiteral("wuqi"), QStringLiteral("Weapon")},
        {QStringLiteral("juanzhou"), QStringLiteral("Scroll")},
        {QStringLiteral("qunzi"), QStringLiteral("Skirt")},
        {QStringLiteral("bozi"), QStringLiteral("Neck")},
        {QStringLiteral("lian"), QStringLiteral("Face")},
        {QStringLiteral("huzi"), QStringLiteral("Beard")},
        {QStringLiteral("yanqiu"), QStringLiteral("Eyes")},
        {QStringLiteral("jiemao"), QStringLiteral("Eyelashes")},
        {QStringLiteral("mask"), QStringLiteral("Mask")},
        {QStringLiteral("fazhang"), QStringLiteral("Staff")},
        {QStringLiteral("faqiu"), QStringLiteral("Orb")},
        {QStringLiteral("faqi"), QStringLiteral("Focus")},
    };
    return d;
}

const QHash<QString, QString>& subGloss()
{
    static const QHash<QString, QString> d = {
        {QStringLiteral("all"), QStringLiteral("full")},
        {QStringLiteral("half"), QStringLiteral("half")},
        {QStringLiteral("parts"), QStringLiteral("parts")},
        {QStringLiteral("back"), QStringLiteral("back")},
        {QStringLiteral("left"), QStringLiteral("left")},
        {QStringLiteral("right"), QStringLiteral("right")},
        {QStringLiteral("face"), QStringLiteral("face")},
        {QStringLiteral("maofa"), QStringLiteral("hair")},
        {QStringLiteral("piaodai"), QStringLiteral("ribbon")},
        {QStringLiteral("beard"), QStringLiteral("beard")},
        {QStringLiteral("fujian"), QStringLiteral("accessory")},
        {QStringLiteral("wing"), QStringLiteral("wings")},
        {QStringLiteral("head"), QStringLiteral("headpiece")},
        {QStringLiteral("nocolor"), QStringLiteral("uncolored")},
    };
    return d;
}

const QHash<QString, QString>& variantGloss()
{
    static const QHash<QString, QString> d = {
        {QStringLiteral("aw1"), QStringLiteral("Awakened I")},
        {QStringLiteral("aw2"), QStringLiteral("Awakened II")},
        {QStringLiteral("aw3"), QStringLiteral("Awakened III")},
        {QStringLiteral("b1"), QStringLiteral("recolor B1")},
        {QStringLiteral("b2"), QStringLiteral("recolor B2")},
        {QStringLiteral("g1"), QStringLiteral("recolor G1")},
        {QStringLiteral("g2"), QStringLiteral("recolor G2")},
        {QStringLiteral("ext"), QStringLiteral("outer form")},
        {QStringLiteral("int"), QStringLiteral("inner form")},
        {QStringLiteral("fx"), QStringLiteral("FX")},
    };
    return d;
}

bool isSetFamily(const QString& t)   // "sz08" / "t07" / "s04" / "ty01"
{
    static const QRegularExpression re(QStringLiteral("^(sz|t|s|ty)\\d+$"));
    return re.match(t).hasMatch();
}

} // namespace

QString classDisplay(const QString& folderLeaf)
{
    QString n = folderLeaf.toLower();
    QString suffix;
    if (n.startsWith(QLatin1String("f_"))) { suffix = QStringLiteral(" (F)"); n = n.mid(2); }
    else if (n.startsWith(QLatin1String("m_"))) { suffix = QStringLiteral(" (M)"); n = n.mid(2); }
    for (const char* c : kCosClasses) {
        if (n == QLatin1String(c)) {
            QString disp = n;
            disp[0] = disp[0].toUpper();
            return disp + suffix;
        }
    }
    return {};
}

Facets facetsOf(const QString& display, const QString& cat1, const QString& cat2)
{
    Facets f;

    // class + slot straight off the asset-name grammar
    int slash = display.lastIndexOf(QLatin1Char('/'));
    QString leaf = (slash >= 0 ? display.mid(slash + 1) : display).toLower();
    const int dot = leaf.indexOf(QLatin1Char('.'));
    if (dot > 0) leaf.truncate(dot);
    // Single-letter prefixes stack: gender (m_/f_) and the hair/face variant
    // markers (B_/W_/Y_ in Char/nielian and Char/dl_*). Strip them all —
    // "B_m_crusader_lian_003" has to reach "crusader" to classify at all.
    while (leaf.size() > 2 && leaf[1] == QLatin1Char('_')) leaf = leaf.mid(2);

    // The class can sit anywhere in the token run, not just first:
    // "Mesh_druid_werewolf_fx03" is druid content too. Whole-token matching
    // only, so a class name can never be caught inside a longer word.
    const QStringList toks = leaf.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    int clsAt = -1;
    for (int i = 0; i < toks.size() && clsAt < 0; ++i)
        for (const char* c : kCosClasses)
            if (toks[i] == QLatin1String(c)) {
                f.cls = toks[i];
                f.cls[0] = f.cls[0].toUpper();
                clsAt = i;
                break;
            }
    // The slot token follows the class; with no class it can still lead
    // ("fazhang_…" in the shared gear folder).
    const int slotAt = clsAt + 1;
    if (slotAt < toks.size()) {
        auto sit = slotGloss().find(toks[slotAt]);
        if (sit != slotGloss().end()) f.slot = sit.value();
    }

    if (cat1 == QLatin1String("Char"))
        f.player = !classDisplay(cat2).isEmpty() || cat2 == QLatin1String("item") ||
                   cat2 == QLatin1String("nielian") ||
                   cat2.startsWith(QLatin1String("dl_"));
    if (!f.cls.isEmpty()) f.player = true;
    return f;
}

QString cosmeticName(const QString& display, QString* setKeyOut)
{
    if (setKeyOut) setKeyOut->clear();
    int slash = display.lastIndexOf(QLatin1Char('/'));
    QString leaf = (slash >= 0 ? display.mid(slash + 1) : display).toLower();
    const int dot = leaf.indexOf(QLatin1Char('.'));
    if (dot > 0) leaf.truncate(dot);

    QString rest = leaf;
    QString gender;
    if (rest.startsWith(QLatin1String("f_"))) { gender = QStringLiteral("(F)"); rest = rest.mid(2); }
    else if (rest.startsWith(QLatin1String("m_"))) { gender = QStringLiteral("(M)"); rest = rest.mid(2); }

    QString cls;
    for (const char* c : kCosClasses) {
        const QString pref = QLatin1String(c) + QLatin1Char('_');
        if (rest.startsWith(pref)) { cls = QLatin1String(c); rest = rest.mid(pref.size()); break; }
    }
    if (cls.isEmpty()) return {};

    const QStringList toks = rest.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (toks.isEmpty()) return {};
    const QString slot = toks.first();
    auto sit = slotGloss().find(slot);
    if (sit == slotGloss().end()) return {};   // not a recognised cosmetic slot

    // Measured: real names have at most ONE set-family token and never a
    // numeric subtype ahead of it. Guard anyway — only treat a bare number as
    // the item number when no family token follows it (audit hardening).
    bool familyAhead = false;
    for (int i = 1; i < toks.size(); ++i)
        if (isSetFamily(toks[i])) { familyAhead = true; break; }

    QStringList subs, variants, unknown;
    QString setKey, itemNum;
    for (int i = 1; i < toks.size(); ++i) {
        const QString& t = toks[i];
        if (setKey.isEmpty() && isSetFamily(t)) {
            setKey = t;
            if (i + 1 < toks.size()) {
                bool num = true;
                for (QChar ch : toks[i + 1]) if (!ch.isDigit()) { num = false; break; }
                if (num) { itemNum = toks[++i]; }
            }
            for (int j = i + 1; j < toks.size(); ++j) variants << toks[j];
            break;
        }
        bool allDigit = !t.isEmpty();
        for (QChar ch : t) if (!ch.isDigit()) { allDigit = false; break; }
        if (setKey.isEmpty() && itemNum.isEmpty() && allDigit && !familyAhead) {
            itemNum = t;                          // bare item number, no set family
            for (int j = i + 1; j < toks.size(); ++j) variants << toks[j];
            break;
        }
        subs << t;
    }

    QString name = cls;
    name[0] = name[0].toUpper();
    QStringList out{name};
    if (!gender.isEmpty()) out << gender;
    out << QStringLiteral("—");

    QStringList subTxt;
    for (const QString& s : subs) {
        auto it = subGloss().find(s);
        if (it != subGloss().end()) subTxt << it.value();
        else unknown << s;
    }
    QString slotPhrase = sit.value();
    if (!subTxt.isEmpty()) slotPhrase = subTxt.join(QLatin1Char(' ')) + QLatin1Char(' ') + slotPhrase;
    out << slotPhrase;

    if (!setKey.isEmpty())
        out << (itemNum.isEmpty() ? QStringLiteral("· set %1").arg(setKey)
                                  : QStringLiteral("· set %1_%2").arg(setKey, itemNum));
    else if (!itemNum.isEmpty())
        out << QStringLiteral("· #%1").arg(itemNum);

    QStringList vTxt;
    for (const QString& v : variants) {
        auto it = variantGloss().find(v);
        if (it != variantGloss().end()) vTxt << it.value();
        else unknown << v;
    }
    if (!vTxt.isEmpty()) out << QStringLiteral("(%1)").arg(vTxt.join(QStringLiteral(", ")));

    QString result = out.join(QLatin1Char(' '));
    if (!unknown.isEmpty())   // flag, never guess
        result += QStringLiteral(" [%1]").arg(unknown.join(QLatin1Char(' ')));
    if (setKeyOut && !setKey.isEmpty())
        *setKeyOut = itemNum.isEmpty() ? setKey
                                       : QStringLiteral("%1_%2").arg(setKey, itemNum);
    return result;
}

QString translate(const QString& display, const QString& type)
{
    // A player cosmetic decodes to a full structured true name; prefer it.
    if (type == QLatin1String("Model") || type == QLatin1String("Mesh") ||
        type == QLatin1String("LodModel") || type.isEmpty()) {
        const QString cn = cosmeticName(display, nullptr);
        if (!cn.isEmpty()) return cn;
    }
    // gloss the LEAF segment only — folders stay as-is in the Name column
    const int slash = display.lastIndexOf(QLatin1Char('/'));
    QString leaf = slash >= 0 ? display.mid(slash + 1) : display;
    const int dot = leaf.indexOf(QLatin1Char('.'));
    if (dot > 0) leaf.truncate(dot);

    const QStringList tokens = leaf.toLower().split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) return {};

    const auto& d = dict();
    QStringList out;
    bool any = false;
    for (int i = 0; i < tokens.size(); ++i) {
        const QString& t = tokens[i];
        // gender prefix, only in leading position ("f_barbarian_...")
        if (i == 0 && tokens.size() > 1 &&
            (t == QLatin1String("f") || t == QLatin1String("m"))) {
            out << (t == QLatin1String("f") ? QStringLiteral("F") : QStringLiteral("M"));
            continue;
        }
        // texture-kind suffix, only in final position on Texture2D rows
        if (i == tokens.size() - 1 && t.size() == 1 &&
            type == QLatin1String("Texture2D")) {
            if (t == QLatin1String("d")) { out << QStringLiteral("(diffuse)");  any = true; continue; }
            if (t == QLatin1String("n")) { out << QStringLiteral("(normal)");   any = true; continue; }
            if (t == QLatin1String("m")) { out << QStringLiteral("(mix)");      any = true; continue; }
            if (t == QLatin1String("e")) { out << QStringLiteral("(emissive)"); any = true; continue; }
        }
        auto it = d.find(t);
        if (it == d.end()) {
            // "jinghua2" -> stem lookup + kept digit
            int k = t.size();
            while (k > 0 && t[k - 1].isDigit()) --k;
            if (k > 0 && k < t.size())
                it = d.find(t.left(k));
            if (it != d.end()) {
                out << it.value() + QLatin1Char(' ') + t.mid(k);
                any = true;
                continue;
            }
            out << t;
            continue;
        }
        out << it.value();
        any = true;
    }
    return any ? out.join(QLatin1Char(' ')) : QString();
}

} // namespace NameTranslator
