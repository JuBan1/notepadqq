/*
    Copyright (C) 2016 Volker Krause <vkrause@kde.org>

    Permission is hereby granted, free of charge, to any person obtaining
    a copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "repository.h"

#include "definition.h"
#include "definition_p.h"
#include "repository_p.h"
#include "theme.h"
#include "themedata_p.h"
#include "wildcardmatcher_p.h"

#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#ifndef NO_STANDARD_PATHS
#include <QStandardPaths>
#endif

#include <limits>

using namespace ote;

RepositoryPrivate* RepositoryPrivate::get(Repository* repo)
{
    return repo->d.get();
}

Repository::Repository(const QString& dataPath)
    : d(new RepositoryPrivate)
{
    d->m_customSearchPaths.append(dataPath);
    d->load(this);
}

Repository::~Repository()
{
    // reset repo so we can detect in still alive definition instances
    // that the repo was deleted
    foreach (const auto& def, d->m_sortedDefs)
        DefinitionData::get(def)->repo = nullptr;
}

Definition Repository::definitionForName(const QString& defName) const
{
    return d->m_defs.value(defName);
}

static Definition bestCandidate(QVector<Definition>& candidates)
{
    if (candidates.isEmpty())
        return Definition();

    std::partial_sort(
        candidates.begin(), candidates.begin() + 1, candidates.end(), [](const Definition& lhs, const Definition& rhs) {
            return lhs.priority() > rhs.priority();
        });

    return candidates.at(0);
}

Definition Repository::definitionForFileName(const QString& fileName) const
{
    QFileInfo fi(fileName);
    const auto name = fi.fileName();

    QVector<Definition> candidates;
    for (auto it = d->m_defs.constBegin(); it != d->m_defs.constEnd(); ++it) {
        auto def = it.value();
        foreach (const auto& pattern, def.extensions()) {
            if (WildcardMatcher::exactMatch(name, pattern)) {
                candidates.push_back(def);
                break;
            }
        }
    }

    for (const auto& det : d->m_fileNameDetections) {
        if (det.fileNames.contains(name)) {
            candidates.push_back(det.def);
            break;
        }
    }

    return bestCandidate(candidates);
}

Definition Repository::definitionForMimeType(const QString& mimeType) const
{
    QVector<Definition> candidates;
    for (auto it = d->m_defs.constBegin(); it != d->m_defs.constEnd(); ++it) {
        auto def = it.value();
        foreach (const auto& matchType, def.mimeTypes()) {
            if (mimeType == matchType) {
                candidates.push_back(def);
                break;
            }
        }
    }

    return bestCandidate(candidates);
}

Definition Repository::definitionForContent(const QString& content) const
{
    QString firstLine = content.mid(0, content.indexOf('\n'));
    // FIXME: Test firstLine corner cases, actually get empty line, etc

    for (const auto& cd : d->m_contentDetections) {
        for (const auto& rule : cd.rules) {
            if (firstLine.contains(rule))
                return cd.def;
        }
    }
    return Definition();
}

QVector<Definition> Repository::definitions() const
{
    return d->m_sortedDefs;
}

QVector<Theme> Repository::themes() const
{
    return d->m_themes;
}

Theme Repository::theme(const QString& themeName) const
{
    for (const auto& theme : d->m_themes) {
        if (theme.name() == themeName) {
            return theme;
        }
    }

    return Theme();
}

Theme Repository::defaultTheme(Repository::DefaultTheme t) const
{
    if (t == DarkTheme)
        return theme(QLatin1String("Breeze Dark"));
    return theme(QLatin1String("Default"));
}

void RepositoryPrivate::load(Repository* repo)
{
    // always add invalid default "None" highlighting
    addDefinition(Definition());

    // do lookup in standard paths, if not disabled
#ifndef NO_STANDARD_PATHS
    foreach (const auto& dir,
        QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
            QStringLiteral("org.kde.syntax-highlighting/syntax"),
            QStandardPaths::LocateDirectory))
        loadSyntaxFolder(repo, dir);

    // backward compatibility with Kate
    foreach (const auto& dir,
        QStandardPaths::locateAll(
            QStandardPaths::GenericDataLocation, QStringLiteral("katepart5/syntax"), QStandardPaths::LocateDirectory))
        loadSyntaxFolder(repo, dir);
#endif

    // default resources are always used
    loadSyntaxFolder(repo, QStringLiteral(":/org.kde.syntax-highlighting/syntax"));

    // user given extra paths
    foreach (const auto& path, m_customSearchPaths)
        loadSyntaxFolder(repo, path + QStringLiteral("/syntax"));

    m_sortedDefs.reserve(m_defs.size());
    for (auto it = m_defs.constBegin(); it != m_defs.constEnd(); ++it)
        m_sortedDefs.push_back(it.value());
    std::sort(m_sortedDefs.begin(), m_sortedDefs.end(), [](const Definition& left, const Definition& right) {
        const auto comparison = left.translatedName().compare(right.translatedName(), Qt::CaseInsensitive);
        return comparison < 0;
    });

    // load themes

    // do lookup in standard paths, if not disabled
#ifndef NO_STANDARD_PATHS
    foreach (const auto& dir,
        QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
            QStringLiteral("org.kde.syntax-highlighting/themes"),
            QStandardPaths::LocateDirectory))
        loadThemeFolder(dir);
#endif

    // default resources are always used
    loadThemeFolder(QStringLiteral(":/org.kde.syntax-highlighting/themes"));

    // user given extra paths
    foreach (const auto& path, m_customSearchPaths)
        loadThemeFolder(path + QStringLiteral("/themes"));

    // load content detection rules. Syntax definitions need to be loaded by this time
    foreach (const auto& path, m_customSearchPaths)
        loadContentDetectionFile(path);
}

void RepositoryPrivate::loadContentDetectionFile(const QString& path)
{
    QFile file(path + QStringLiteral("/contentDetection.json"));
    if (!file.open(QFile::ReadOnly))
        return;

    const auto jdoc(QJsonDocument::fromJson(file.readAll()));
    const auto& base = jdoc.object();

    for (const auto& key : base.keys()) {
        const auto& obj = base[key].toObject();

        const auto& it = m_defs.constFind(key);

        if (it == m_defs.constEnd()) {
            qDebug() << "Found content detection rules for an unknown definition: " << key;
            continue;
        }

        const auto& content = obj["content"];
        if (content.isArray()) {
            ContentDetection d;
            d.def = *it;

            for (QJsonValueRef rule : content.toArray())
                d.rules.push_back(QRegularExpression(rule.toString()));

            m_contentDetections.push_back(d);
        }

        const auto& fileNames = obj["fileNames"];
        if (fileNames.isArray()) {
            FileNameDetection fd;
            fd.def = *it;

            for (const QJsonValueRef name : fileNames.toArray())
                fd.fileNames.push_back(name.toString());

            m_fileNameDetections.push_back(fd);
        }
    }
}

void RepositoryPrivate::loadSyntaxFolder(Repository* repo, const QString& path)
{
    if (loadSyntaxFolderFromIndex(repo, path))
        return;

    QDirIterator it(path, QStringList() << QLatin1String("*.xml"), QDir::Files);
    while (it.hasNext()) {
        Definition def;
        auto defData = DefinitionData::get(def);
        defData->repo = repo;
        if (defData->loadMetaData(it.next()))
            addDefinition(def);
    }
}

bool RepositoryPrivate::loadSyntaxFolderFromIndex(Repository* repo, const QString& path)
{
    QFile indexFile(path + QLatin1String("/index.katesyntax"));
    if (!indexFile.open(QFile::ReadOnly))
        return false;

    const auto indexDoc(QJsonDocument::fromBinaryData(indexFile.readAll()));
    const auto index = indexDoc.object();
    for (auto it = index.begin(); it != index.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const auto fileName = QString(path + QLatin1Char('/') + it.key());
        const auto defMap = it.value().toObject();
        Definition def;
        auto defData = DefinitionData::get(def);
        defData->repo = repo;
        if (defData->loadMetaData(fileName, defMap))
            addDefinition(def);
    }
    return true;
}

void RepositoryPrivate::addDefinition(const Definition& def)
{
    const auto it = m_defs.constFind(def.name());
    if (it == m_defs.constEnd()) {
        m_defs.insert(def.name(), def);
        return;
    }

    if (it.value().version() >= def.version())
        return;
    m_defs.insert(def.name(), def);
}

void RepositoryPrivate::loadThemeFolder(const QString& path)
{
    QDirIterator it(path, QStringList() << QLatin1String("*.theme"), QDir::Files);
    while (it.hasNext()) {
        auto themeData = std::unique_ptr<ThemeData>(new ThemeData);
        if (themeData->load(it.next()))
            addTheme(Theme(themeData.release()));
    }
}

static int themeRevision(const Theme& theme)
{
    auto data = ThemeData::get(theme);
    return data->revision();
}

void RepositoryPrivate::addTheme(const Theme& theme)
{
    const auto it = std::lower_bound(m_themes.begin(), m_themes.end(), theme, [](const Theme& lhs, const Theme& rhs) {
        return lhs.name() < rhs.name();
    });
    if (it == m_themes.end() || (*it).name() != theme.name()) {
        m_themes.insert(it, theme);
        return;
    }
    if (themeRevision(*it) < themeRevision(theme))
        *it = theme;
}

quint16 RepositoryPrivate::foldingRegionId(const QString& defName, const QString& foldName)
{
    const auto it = m_foldingRegionIds.constFind(qMakePair(defName, foldName));
    if (it != m_foldingRegionIds.constEnd())
        return it.value();
    m_foldingRegionIds.insert(qMakePair(defName, foldName), ++m_foldingRegionId);
    return m_foldingRegionId;
}

quint16 RepositoryPrivate::nextFormatId()
{
    Q_ASSERT(m_formatId < std::numeric_limits<quint16>::max());
    return ++m_formatId;
}

void Repository::reload()
{
    qDebug() << "Reloading syntax definitions!";
    foreach (const auto& def, d->m_sortedDefs)
        DefinitionData::get(def)->clear();
    d->m_defs.clear();
    d->m_sortedDefs.clear();

    d->m_themes.clear();

    d->m_foldingRegionId = 0;
    d->m_foldingRegionIds.clear();

    d->m_formatId = 0;

    d->m_contentDetections.clear();

    d->load(this);
}

void Repository::addCustomSearchPath(const QString& path)
{
    d->m_customSearchPaths.append(path);
    reload();
}

QVector<QString> Repository::customSearchPaths() const
{
    return d->m_customSearchPaths;
}
