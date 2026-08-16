#include "index/ItemNames.h"

#include <QFile>
#include <QRegularExpression>
#include <QDateTime>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <map>

#include "index/NameTranslator.h"

namespace ItemNames {

namespace {

// the classes cosmeticName recognises, lowercased, for deriving the class of a
// display name (the folder segment "Char/f_barbarian" -> "barbarian")
QString classOf(const QString& displayLower)
{
    static const QRegularExpression re(
        QStringLiteral("(bloodknight|demonhunter|necromancer|barbarian|crusader|"
                       "sorceress|wizard|tempest|warlock|druid|monk)"));
    const auto m = re.match(displayLower);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString setKeyOf(const QString& display)
{
    QString key;
    NameTranslator::cosmeticName(display, &key);
    return key;   // "t07_004" style, or empty
}

} // namespace

Table load(const QString& csvPath)
{
    Table t;
    QFile f(csvPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return t;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;
        // setKey,class,name  — name may contain commas, so split only twice
        const int c1 = line.indexOf(QLatin1Char(','));
        if (c1 < 0) continue;
        const int c2 = line.indexOf(QLatin1Char(','), c1 + 1);
        if (c2 < 0) continue;
        const QString key = line.left(c1).trimmed().toLower();
        const QString cls = line.mid(c1 + 1, c2 - c1 - 1).trimmed().toLower();
        const QString name = line.mid(c2 + 1).trimmed();
        if (key.isEmpty() || name.isEmpty()) continue;   // "<key>,," rows skip
        t.byKey.insert(cls.isEmpty() ? key : key + QLatin1Char('|') + cls, name);
    }
    qInfo("item-names: loaded %d override rows from %s", t.size(), qPrintable(csvPath));
    return t;
}

QString nameFor(const Table& table, const QString& display)
{
    if (table.empty()) return {};
    const QString key = setKeyOf(display);
    if (key.isEmpty()) return {};
    const QString cls = classOf(display.toLower());
    if (!cls.isEmpty()) {                       // class-specific row wins
        auto it = table.byKey.find(key + QLatin1Char('|') + cls);
        if (it != table.byKey.end()) return it.value();
    }
    auto it = table.byKey.find(key);
    return it == table.byKey.end() ? QString() : it.value();
}

QString nameForSetKey(const Table& table, const QString& setKey,
                      const QString& className)
{
    if (table.empty() || setKey.isEmpty()) return {};
    const QString key = setKey.toLower();
    if (!className.isEmpty()) {                  // class-specific row wins
        auto it = table.byKey.find(key + QLatin1Char('|') + className.toLower());
        if (it != table.byKey.end()) return it.value();
    }
    auto it = table.byKey.find(key);
    return it == table.byKey.end() ? QString() : it.value();
}

std::string fingerprint(const QString& csvPath)
{
    QFileInfo fi(csvPath);
    if (!fi.exists()) return "none";
    return (QString::number(fi.size()) + QLatin1Char(':') +
            QString::number(fi.lastModified().toMSecsSinceEpoch()))
        .toStdString();
}

int exportTemplate(const std::shared_ptr<AssetIndex>& idx, const QString& csvPath)
{
    if (!idx) return -1;
    // one representative structural name per distinct set key (sorted)
    std::map<std::string, QString> byKey;
    for (const AssetRow& r : idx->rows) {
        if (r.type != QLatin1String("Model")) continue;
        QString key;
        const QString cn = NameTranslator::cosmeticName(r.display, &key);
        if (key.isEmpty() || cn.isEmpty()) continue;
        byKey.emplace(key.toStdString(), cn);   // first wins; keys are stable
    }
    if (byKey.empty()) return 0;

    QSaveFile f(csvPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return -1;
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "# DIAssetBrowser cosmetic-set real-name overrides\n"
        << "# Fill the third column with the in-game marketing name.\n"
        << "# Format:  setKey,class,name   (blank class = applies to all classes)\n"
        << "# The comment line above each row is the decoded structural name.\n"
        << "# setKey,class,name\n";
    for (const auto& [k, structural] : byKey)
        out << "# " << structural << '\n'
            << QString::fromStdString(k) << ",,\n";
    if (!f.commit())
        return -1;
    qInfo("item-names: wrote %zu set-key template rows to %s", byKey.size(),
          qPrintable(csvPath));
    return (int)byKey.size();
}

} // namespace ItemNames
