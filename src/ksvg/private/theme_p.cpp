/*
    SPDX-FileCopyrightText: 2006-2007 Aaron Seigo <aseigo@kde.org>
    SPDX-FileCopyrightText: 2013 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "theme_p.h"
#include "debug_p.h"
#include "framesvg.h"
#include "framesvg_p.h"
#include "svg_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>

#include <KDirWatch>
#include <KIconLoader>
#include <KIconTheme>
#include <KSharedConfig>
#include <kpluginmetadata.h>

namespace KSvg
{
const char ThemePrivate::defaultTheme[] = "default";
const char ThemePrivate::themeRcFile[] = "plasmarc";
// the system colors theme is used to cache unthemed svgs with colorization needs
// these svgs do not follow the theme's colors, but rather the system colors
const char ThemePrivate::systemColorsTheme[] = "internal-system-colors";

ThemePrivate *ThemePrivate::globalTheme = nullptr;
QHash<QString, ThemePrivate *> ThemePrivate::themes = QHash<QString, ThemePrivate *>();
using QSP = QStandardPaths;

KSharedConfig::Ptr configForTheme(const QString &basePath, const QString &theme)
{
    const QString baseName = basePath % theme;
    QString configPath = QSP::locate(QSP::GenericDataLocation, baseName + QLatin1String("/plasmarc"));
    if (!configPath.isEmpty()) {
        return KSharedConfig::openConfig(configPath, KConfig::SimpleConfig);
    }
    QString metadataPath = QSP::locate(QSP::GenericDataLocation, baseName + QLatin1String("/metadata.desktop"));
    return KSharedConfig::openConfig(metadataPath, KConfig::SimpleConfig);
}

KPluginMetaData metaDataForTheme(const QString &basePath, const QString &theme)
{
    const QString packageBasePath = QSP::locate(QSP::GenericDataLocation, basePath % theme, QSP::LocateDirectory);
    if (packageBasePath.isEmpty()) {
        qWarning(LOG_KSVG) << "Could not locate plasma theme" << theme << "in" << basePath << "using search path"
                           << QSP::standardLocations(QSP::GenericDataLocation);
        return {};
    }
    if (QFileInfo::exists(packageBasePath + QLatin1String("/metadata.json"))) {
        return KPluginMetaData::fromJsonFile(packageBasePath + QLatin1String("/metadata.json"));
    } else {
        qCWarning(LOG_KSVG) << "Could not locate metadata for theme" << theme;
        return {};
    }
}

ThemePrivate::ThemePrivate(QObject *parent)
    : QObject(parent)
    , colorScheme(QPalette::Active, KColorScheme::Window, KSharedConfigPtr(nullptr))
    , selectionColorScheme(QPalette::Active, KColorScheme::Selection, KSharedConfigPtr(nullptr))
    , buttonColorScheme(QPalette::Active, KColorScheme::Button, KSharedConfigPtr(nullptr))
    , viewColorScheme(QPalette::Active, KColorScheme::View, KSharedConfigPtr(nullptr))
    , complementaryColorScheme(QPalette::Active, KColorScheme::Complementary, KSharedConfigPtr(nullptr))
    , headerColorScheme(QPalette::Active, KColorScheme::Header, KSharedConfigPtr(nullptr))
    , tooltipColorScheme(QPalette::Active, KColorScheme::Tooltip, KSharedConfigPtr(nullptr))
    , pixmapCache(nullptr)
    , cacheSize(0)
    , cachesToDiscard(NoCache)
    , isDefault(true)
    , useGlobal(true)
    , fixedName(false)
    , apiMajor(1)
    , apiMinor(0)
    , apiRevision(0)
{
    ThemeConfig config;
    cacheTheme = config.cacheTheme();

    const QString org = QCoreApplication::organizationName();
    if (!org.isEmpty()) {
        basePath += u'/' + org;
    }
    const QString appName = QCoreApplication::applicationName();
    if (!appName.isEmpty()) {
        basePath += u'/' + appName;
    }
    if (basePath.isEmpty()) {
        basePath = QStringLiteral("ksvg");
    }
    basePath += u"/svgtheme/";

    pixmapSaveTimer = new QTimer(this);
    pixmapSaveTimer->setSingleShot(true);
    pixmapSaveTimer->setInterval(600);
    QObject::connect(pixmapSaveTimer, &QTimer::timeout, this, &ThemePrivate::scheduledCacheUpdate);

    updateNotificationTimer = new QTimer(this);
    updateNotificationTimer->setSingleShot(true);
    updateNotificationTimer->setInterval(100);
    QObject::connect(updateNotificationTimer, &QTimer::timeout, this, &ThemePrivate::notifyOfChanged);

    QCoreApplication::instance()->installEventFilter(this);

    const QString configFile = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QLatin1Char('/') + QLatin1String(themeRcFile);
    KDirWatch::self()->addFile(configFile);

    // Catch both, direct changes to the config file ...
    connect(KDirWatch::self(), &KDirWatch::dirty, this, &ThemePrivate::settingsFileChanged);
    // ... but also remove/recreate cycles, like KConfig does it
    connect(KDirWatch::self(), &KDirWatch::created, this, &ThemePrivate::settingsFileChanged);

    QObject::connect(KIconLoader::global(), &KIconLoader::iconChanged, this, [this]() {
        scheduleThemeChangeNotification(PixmapCache | SvgElementsCache);
    });

}

ThemePrivate::~ThemePrivate()
{
    FrameSvgPrivate::s_sharedFrames.remove(this);
    delete pixmapCache;
}

KConfigGroup &ThemePrivate::config()
{
    if (!cfg.isValid()) {
        QString groupName = QStringLiteral("Theme");

        if (!useGlobal) {
            QString app = QCoreApplication::applicationName();

            if (!app.isEmpty()) {
#ifndef NDEBUG
                // qCDebug(LOG_KSVG) << "using theme for app" << app;
#endif
                groupName.append(QLatin1Char('-')).append(app);
            }
        }
        cfg = KConfigGroup(KSharedConfig::openConfig(QFile::decodeName(themeRcFile)), groupName);
    }

    return cfg;
}

bool ThemePrivate::useCache()
{
    bool cachesTooOld = false;

    if (cacheTheme && !pixmapCache) {
        if (cacheSize == 0) {
            ThemeConfig config;
            cacheSize = config.themeCacheKb();
        }
        const bool isRegularTheme = themeName != QLatin1String(systemColorsTheme);
        QString cacheFile = QLatin1String("plasma_theme_") + themeName;

        // clear any cached values from the previous theme cache
        themeVersion.clear();

        if (!themeMetadataPath.isEmpty()) {
            KDirWatch::self()->removeFile(themeMetadataPath);
        }
        themeMetadataPath = configForTheme(basePath, themeName)->name();
        if (isRegularTheme) {
            const auto *iconTheme = KIconLoader::global()->theme();
            if (iconTheme) {
                iconThemeMetadataPath = iconTheme->dir() + QStringLiteral("index.theme");
            }

            const QString cacheFileBase = cacheFile + QLatin1String("*.kcache");

            QString currentCacheFileName;
            if (!themeMetadataPath.isEmpty()) {
                // now we record the theme version, if we can
                const KPluginMetaData data = metaDataForTheme(basePath, themeName);
                if (data.isValid()) {
                    themeVersion = data.version();
                }
                if (!themeVersion.isEmpty()) {
                    cacheFile += QLatin1String("_v") + themeVersion;
                    currentCacheFileName = cacheFile + QLatin1String(".kcache");
                }

                // watch the metadata file for changes at runtime
                KDirWatch::self()->addFile(themeMetadataPath);
                QObject::connect(KDirWatch::self(), &KDirWatch::created, this, &ThemePrivate::settingsFileChanged, Qt::UniqueConnection);
                QObject::connect(KDirWatch::self(), &KDirWatch::dirty, this, &ThemePrivate::settingsFileChanged, Qt::UniqueConnection);

                if (!iconThemeMetadataPath.isEmpty()) {
                    KDirWatch::self()->addFile(iconThemeMetadataPath);
                }
            }

            // now we check for, and remove if necessary, old caches
            QDir cacheDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation));
            cacheDir.setNameFilters(QStringList({cacheFileBase}));

            const auto files = cacheDir.entryInfoList();
            for (const QFileInfo &file : files) {
                if (currentCacheFileName.isEmpty() //
                    || !file.absoluteFilePath().endsWith(currentCacheFileName)) {
                    QFile::remove(file.absoluteFilePath());
                }
            }
        }

        // now we do a sanity check: if the metadata.desktop file is newer than the cache, drop the cache
        if (isRegularTheme && !themeMetadataPath.isEmpty()) {
            // now we check to see if the theme metadata file itself is newer than the pixmap cache
            // this is done before creating the pixmapCache object since that can change the mtime
            // on the cache file

            // FIXME: when using the system colors, if they change while the application is not running
            // the cache should be dropped; we need a way to detect system color change when the
            // application is not running.
            // check for expired cache
            const QString cacheFilePath =
                QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QLatin1Char('/') + cacheFile + QLatin1String(".kcache");
            if (!cacheFilePath.isEmpty()) {
                const QFileInfo cacheFileInfo(cacheFilePath);
                const QFileInfo metadataFileInfo(themeMetadataPath);
                const QFileInfo iconThemeMetadataFileInfo(iconThemeMetadataPath);

                cachesTooOld = (cacheFileInfo.lastModified().toSecsSinceEpoch() < metadataFileInfo.lastModified().toSecsSinceEpoch())
                    || (cacheFileInfo.lastModified().toSecsSinceEpoch() < iconThemeMetadataFileInfo.lastModified().toSecsSinceEpoch());
            }
        }

        ThemeConfig config;
        pixmapCache = new KImageCache(cacheFile, config.themeCacheKb() * 1024);
        pixmapCache->setEvictionPolicy(KSharedDataCache::EvictLeastRecentlyUsed);

        if (cachesTooOld) {
            discardCache(PixmapCache | SvgElementsCache);
        }
    }

    if (cacheTheme) {
        QString currentIconThemePath;
        const auto *iconTheme = KIconLoader::global()->theme();
        if (iconTheme) {
            currentIconThemePath = iconTheme->dir();
        }

        const QString oldIconThemePath = SvgRectsCache::instance()->iconThemePath();
        if (oldIconThemePath != currentIconThemePath) {
            discardCache(PixmapCache | SvgElementsCache);
            SvgRectsCache::instance()->setIconThemePath(currentIconThemePath);
        }
    }

    return cacheTheme;
}

void ThemePrivate::onAppExitCleanup()
{
    pixmapsToCache.clear();
    delete pixmapCache;
    pixmapCache = nullptr;
    cacheTheme = false;
}

QString ThemePrivate::imagePath(const QString &theme, const QString &type, const QString &image)
{
    QString subdir = basePath % theme % type % image;
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation, subdir);
}

QString ThemePrivate::findInTheme(const QString &image, const QString &theme, bool cache)
{
    if (cache) {
        auto it = discoveries.constFind(image);
        if (it != discoveries.constEnd()) {
            return it.value();
        }
    }

    QString search;

    // TODO: use also QFileSelector::allSelectors?
    // TODO: check if the theme supports selectors starting with +
    for (const QString &type : std::as_const(selectors)) {
        search = imagePath(theme, QLatin1Char('/') % type % QLatin1Char('/'), image);
        if (!search.isEmpty()) {
            break;
        }
    }

    // not found in selectors
    if (search.isEmpty()) {
        search = imagePath(theme, QStringLiteral("/"), image);
    }

    if (cache && !search.isEmpty()) {
        discoveries.insert(image, search);
    }

    return search;
}

void ThemePrivate::discardCache(CacheTypes caches)
{
    if (caches & PixmapCache) {
        pixmapsToCache.clear();
        pixmapSaveTimer->stop();
        if (pixmapCache) {
            pixmapCache->clear();
        }
    } else {
        // This deletes the object but keeps the on-disk cache for later use
        delete pixmapCache;
        pixmapCache = nullptr;
    }

    cachedSvgStyleSheets.clear();
    cachedSelectedSvgStyleSheets.clear();
    cachedInactiveSvgStyleSheets.clear();

    if (caches & SvgElementsCache) {
        discoveries.clear();
    }
}

void ThemePrivate::scheduledCacheUpdate()
{
    if (useCache()) {
        QHashIterator<QString, QPixmap> it(pixmapsToCache);
        while (it.hasNext()) {
            it.next();
            pixmapCache->insertPixmap(idsToCache[it.key()], it.value());
        }
    }

    pixmapsToCache.clear();
    keysToCache.clear();
    idsToCache.clear();
}

void ThemePrivate::colorsChanged()
{
    // in the case the theme follows the desktop settings, refetch the colorschemes
    // and discard the svg pixmap cache
    if (!colors) {
        KSharedConfig::openConfig()->reparseConfiguration();
    }
    colorScheme = KColorScheme(QPalette::Active, KColorScheme::Window, colors);
    buttonColorScheme = KColorScheme(QPalette::Active, KColorScheme::Button, colors);
    viewColorScheme = KColorScheme(QPalette::Active, KColorScheme::View, colors);
    selectionColorScheme = KColorScheme(QPalette::Active, KColorScheme::Selection, colors);
    complementaryColorScheme = KColorScheme(QPalette::Active, KColorScheme::Complementary, colors);
    headerColorScheme = KColorScheme(QPalette::Active, KColorScheme::Header, colors);
    tooltipColorScheme = KColorScheme(QPalette::Active, KColorScheme::Tooltip, colors);
    palette = KColorScheme::createApplicationPalette(colors);
    scheduleThemeChangeNotification(PixmapCache | SvgElementsCache);
    Q_EMIT applicationPaletteChange();
}

void ThemePrivate::scheduleThemeChangeNotification(CacheTypes caches)
{
    cachesToDiscard |= caches;
    updateNotificationTimer->start();
}

void ThemePrivate::notifyOfChanged()
{
    // qCDebug(LOG_KSVG) << cachesToDiscard;
    discardCache(cachesToDiscard);
    cachesToDiscard = NoCache;
    Q_EMIT themeChanged();
}

const QString ThemePrivate::processStyleSheet(const QString &css, KSvg::Svg::Status status)
{
    QString stylesheet;

    QHash<QString, QString> elements;
    // If you add elements here, make sure their names are sufficiently unique to not cause
    // clashes between element keys
    elements[QStringLiteral("%textcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightedTextColor : (status == Svg::Status::Inactive ? Theme::DisabledTextColor : Theme::TextColor),
              Theme::NormalColorGroup)
            .name();
    elements[QStringLiteral("%backgroundcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightColor : Theme::BackgroundColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%highlightcolor")] = color(Theme::HighlightColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%highlightedtextcolor")] = color(Theme::HighlightedTextColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%visitedlink")] = color(Theme::VisitedLinkColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%activatedlink")] = color(Theme::HighlightColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%hoveredlink")] = color(Theme::HighlightColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%link")] = color(Theme::LinkColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%positivetextcolor")] = color(Theme::PositiveTextColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%neutraltextcolor")] = color(Theme::NeutralTextColor, Theme::NormalColorGroup).name();
    elements[QStringLiteral("%negativetextcolor")] = color(Theme::NegativeTextColor, Theme::NormalColorGroup).name();

    elements[QStringLiteral("%buttontextcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightedTextColor : Theme::TextColor, Theme::ButtonColorGroup).name();
    elements[QStringLiteral("%buttonbackgroundcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightColor : Theme::BackgroundColor, Theme::ButtonColorGroup).name();
    elements[QStringLiteral("%buttonhovercolor")] = color(Theme::HoverColor, Theme::ButtonColorGroup).name();
    elements[QStringLiteral("%buttonfocuscolor")] = color(Theme::FocusColor, Theme::ButtonColorGroup).name();
    elements[QStringLiteral("%buttonhighlightedtextcolor")] = color(Theme::HighlightedTextColor, Theme::ButtonColorGroup).name();
    elements[QStringLiteral("%buttonpositivetextcolor")] = color(Theme::PositiveTextColor, Theme::ButtonColorGroup).name();
    elements[QStringLiteral("%buttonneutraltextcolor")] = color(Theme::NeutralTextColor, Theme::ButtonColorGroup).name();
    elements[QStringLiteral("%buttonnegativetextcolor")] = color(Theme::NegativeTextColor, Theme::ButtonColorGroup).name();

    elements[QStringLiteral("%viewtextcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightedTextColor : Theme::TextColor, Theme::ViewColorGroup).name();
    elements[QStringLiteral("%viewbackgroundcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightColor : Theme::BackgroundColor, Theme::ViewColorGroup).name();
    elements[QStringLiteral("%viewhovercolor")] = color(Theme::HoverColor, Theme::ViewColorGroup).name();
    elements[QStringLiteral("%viewfocuscolor")] = color(Theme::FocusColor, Theme::ViewColorGroup).name();
    elements[QStringLiteral("%viewhighlightedtextcolor")] = color(Theme::HighlightedTextColor, Theme::ViewColorGroup).name();
    elements[QStringLiteral("%viewpositivetextcolor")] = color(Theme::PositiveTextColor, Theme::ViewColorGroup).name();
    elements[QStringLiteral("%viewneutraltextcolor")] = color(Theme::NeutralTextColor, Theme::ViewColorGroup).name();
    elements[QStringLiteral("%viewnegativetextcolor")] = color(Theme::NegativeTextColor, Theme::ViewColorGroup).name();

    elements[QStringLiteral("%tooltiptextcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightedTextColor : Theme::TextColor, Theme::ToolTipColorGroup).name();
    elements[QStringLiteral("%tooltipbackgroundcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightColor : Theme::BackgroundColor, Theme::ToolTipColorGroup).name();
    elements[QStringLiteral("%tooltiphovercolor")] = color(Theme::HoverColor, Theme::ToolTipColorGroup).name();
    elements[QStringLiteral("%tooltipfocuscolor")] = color(Theme::FocusColor, Theme::ToolTipColorGroup).name();
    elements[QStringLiteral("%tooltiphighlightedtextcolor")] = color(Theme::HighlightedTextColor, Theme::ToolTipColorGroup).name();
    elements[QStringLiteral("%tooltippositivetextcolor")] = color(Theme::PositiveTextColor, Theme::ToolTipColorGroup).name();
    elements[QStringLiteral("%tooltipneutraltextcolor")] = color(Theme::NeutralTextColor, Theme::ToolTipColorGroup).name();
    elements[QStringLiteral("%tooltipnegativetextcolor")] = color(Theme::NegativeTextColor, Theme::ToolTipColorGroup).name();

    elements[QStringLiteral("%complementarytextcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightedTextColor : Theme::TextColor, Theme::ComplementaryColorGroup).name();
    elements[QStringLiteral("%complementarybackgroundcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightColor : Theme::BackgroundColor, Theme::ComplementaryColorGroup).name();
    elements[QStringLiteral("%complementaryhovercolor")] = color(Theme::HoverColor, Theme::ComplementaryColorGroup).name();
    elements[QStringLiteral("%complementaryfocuscolor")] = color(Theme::FocusColor, Theme::ComplementaryColorGroup).name();
    elements[QStringLiteral("%complementaryhighlightedtextcolor")] = color(Theme::HighlightedTextColor, Theme::ComplementaryColorGroup).name();
    elements[QStringLiteral("%complementarypositivetextcolor")] = color(Theme::PositiveTextColor, Theme::ComplementaryColorGroup).name();
    elements[QStringLiteral("%complementaryneutraltextcolor")] = color(Theme::NeutralTextColor, Theme::ComplementaryColorGroup).name();
    elements[QStringLiteral("%complementarynegativetextcolor")] = color(Theme::NegativeTextColor, Theme::ComplementaryColorGroup).name();

    elements[QStringLiteral("%headertextcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightedTextColor : Theme::TextColor, Theme::HeaderColorGroup).name();
    elements[QStringLiteral("%headerbackgroundcolor")] =
        color(status == Svg::Status::Selected ? Theme::HighlightColor : Theme::BackgroundColor, Theme::HeaderColorGroup).name();
    elements[QStringLiteral("%headerhovercolor")] = color(Theme::HoverColor, Theme::HeaderColorGroup).name();
    elements[QStringLiteral("%headerfocuscolor")] = color(Theme::FocusColor, Theme::HeaderColorGroup).name();
    elements[QStringLiteral("%headerhighlightedtextcolor")] = color(Theme::HighlightedTextColor, Theme::HeaderColorGroup).name();
    elements[QStringLiteral("%headerpositivetextcolor")] = color(Theme::PositiveTextColor, Theme::HeaderColorGroup).name();
    elements[QStringLiteral("%headerneutraltextcolor")] = color(Theme::NeutralTextColor, Theme::HeaderColorGroup).name();
    elements[QStringLiteral("%headernegativetextcolor")] = color(Theme::NegativeTextColor, Theme::HeaderColorGroup).name();

    QFont font = QGuiApplication::font();
    elements[QStringLiteral("%fontsize")] = QStringLiteral("%1pt").arg(font.pointSize());
    QString family{font.family()};
    family.truncate(family.indexOf(QLatin1Char('[')));
    elements[QStringLiteral("%fontfamily")] = family;
    elements[QStringLiteral("%smallfontsize")] = QStringLiteral("%1pt").arg(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont).pointSize());

    QHash<QString, QString>::const_iterator it = elements.constBegin();
    QHash<QString, QString>::const_iterator itEnd = elements.constEnd();
    for (; it != itEnd; ++it) {
        stylesheet.replace(it.key(), it.value());
    }
    return stylesheet;
}

const QString ThemePrivate::svgStyleSheet(KSvg::Theme::ColorGroup group, KSvg::Svg::Status status)
{
    QString stylesheet = (status == Svg::Status::Selected)
        ? cachedSelectedSvgStyleSheets.value(group)
        : (status == Svg::Status::Inactive ? cachedInactiveSvgStyleSheets.value(group) : cachedSvgStyleSheets.value(group));
    if (stylesheet.isEmpty()) {
        QString skel = QStringLiteral(".ColorScheme-%1{color:%2;}");

        switch (group) {
        case Theme::ButtonColorGroup:
            stylesheet += skel.arg(QStringLiteral("Text"), QStringLiteral("%buttontextcolor"));
            stylesheet += skel.arg(QStringLiteral("Background"), QStringLiteral("%buttonbackgroundcolor"));

            stylesheet += skel.arg(QStringLiteral("Highlight"), QStringLiteral("%buttonhovercolor"));
            stylesheet += skel.arg(QStringLiteral("HighlightedText"), QStringLiteral("%buttonhighlightedtextcolor"));
            stylesheet += skel.arg(QStringLiteral("PositiveText"), QStringLiteral("%buttonpositivetextcolor"));
            stylesheet += skel.arg(QStringLiteral("NeutralText"), QStringLiteral("%buttonneutraltextcolor"));
            stylesheet += skel.arg(QStringLiteral("NegativeText"), QStringLiteral("%buttonnegativetextcolor"));
            break;
        case Theme::ViewColorGroup:
            stylesheet += skel.arg(QStringLiteral("Text"), QStringLiteral("%viewtextcolor"));
            stylesheet += skel.arg(QStringLiteral("Background"), QStringLiteral("%viewbackgroundcolor"));

            stylesheet += skel.arg(QStringLiteral("Highlight"), QStringLiteral("%viewhovercolor"));
            stylesheet += skel.arg(QStringLiteral("HighlightedText"), QStringLiteral("%viewhighlightedtextcolor"));
            stylesheet += skel.arg(QStringLiteral("PositiveText"), QStringLiteral("%viewpositivetextcolor"));
            stylesheet += skel.arg(QStringLiteral("NeutralText"), QStringLiteral("%viewneutraltextcolor"));
            stylesheet += skel.arg(QStringLiteral("NegativeText"), QStringLiteral("%viewnegativetextcolor"));
            break;
        case Theme::ComplementaryColorGroup:
            stylesheet += skel.arg(QStringLiteral("Text"), QStringLiteral("%complementarytextcolor"));
            stylesheet += skel.arg(QStringLiteral("Background"), QStringLiteral("%complementarybackgroundcolor"));

            stylesheet += skel.arg(QStringLiteral("Highlight"), QStringLiteral("%complementaryhovercolor"));
            stylesheet += skel.arg(QStringLiteral("HighlightedText"), QStringLiteral("%complementaryhighlightedtextcolor"));
            stylesheet += skel.arg(QStringLiteral("PositiveText"), QStringLiteral("%complementarypositivetextcolor"));
            stylesheet += skel.arg(QStringLiteral("NeutralText"), QStringLiteral("%complementaryneutraltextcolor"));
            stylesheet += skel.arg(QStringLiteral("NegativeText"), QStringLiteral("%complementarynegativetextcolor"));
            break;
        case Theme::HeaderColorGroup:
            stylesheet += skel.arg(QStringLiteral("Text"), QStringLiteral("%headertextcolor"));
            stylesheet += skel.arg(QStringLiteral("Background"), QStringLiteral("%headerbackgroundcolor"));

            stylesheet += skel.arg(QStringLiteral("Highlight"), QStringLiteral("%headerhovercolor"));
            stylesheet += skel.arg(QStringLiteral("HighlightedText"), QStringLiteral("%headerhighlightedtextcolor"));
            stylesheet += skel.arg(QStringLiteral("PositiveText"), QStringLiteral("%headerpositivetextcolor"));
            stylesheet += skel.arg(QStringLiteral("NeutralText"), QStringLiteral("%headerneutraltextcolor"));
            stylesheet += skel.arg(QStringLiteral("NegativeText"), QStringLiteral("%headernegativetextcolor"));
            break;
        case Theme::ToolTipColorGroup:
            stylesheet += skel.arg(QStringLiteral("Text"), QStringLiteral("%tooltiptextcolor"));
            stylesheet += skel.arg(QStringLiteral("Background"), QStringLiteral("%tooltipbackgroundcolor"));

            stylesheet += skel.arg(QStringLiteral("Highlight"), QStringLiteral("%tooltiphovercolor"));
            stylesheet += skel.arg(QStringLiteral("HighlightedText"), QStringLiteral("%tooltiphighlightedtextcolor"));
            stylesheet += skel.arg(QStringLiteral("PositiveText"), QStringLiteral("%tooltippositivetextcolor"));
            stylesheet += skel.arg(QStringLiteral("NeutralText"), QStringLiteral("%tooltipneutraltextcolor"));
            stylesheet += skel.arg(QStringLiteral("NegativeText"), QStringLiteral("%tooltipnegativetextcolor"));
            break;
        default:
            stylesheet += skel.arg(QStringLiteral("Text"), QStringLiteral("%textcolor"));
            stylesheet += skel.arg(QStringLiteral("Background"), QStringLiteral("%backgroundcolor"));

            stylesheet += skel.arg(QStringLiteral("Highlight"), QStringLiteral("%highlightcolor"));
            stylesheet += skel.arg(QStringLiteral("HighlightedText"), QStringLiteral("%highlightedtextcolor"));
            stylesheet += skel.arg(QStringLiteral("PositiveText"), QStringLiteral("%positivetextcolor"));
            stylesheet += skel.arg(QStringLiteral("NeutralText"), QStringLiteral("%neutraltextcolor"));
            stylesheet += skel.arg(QStringLiteral("NegativeText"), QStringLiteral("%negativetextcolor"));
        }

        stylesheet += skel.arg(QStringLiteral("ButtonText"), QStringLiteral("%buttontextcolor"));
        stylesheet += skel.arg(QStringLiteral("ButtonBackground"), QStringLiteral("%buttonbackgroundcolor"));
        stylesheet += skel.arg(QStringLiteral("ButtonHover"), QStringLiteral("%buttonhovercolor"));
        stylesheet += skel.arg(QStringLiteral("ButtonFocus"), QStringLiteral("%buttonfocuscolor"));
        stylesheet += skel.arg(QStringLiteral("ButtonHighlightedText"), QStringLiteral("%buttonhighlightedtextcolor"));
        stylesheet += skel.arg(QStringLiteral("ButtonPositiveText"), QStringLiteral("%buttonpositivetextcolor"));
        stylesheet += skel.arg(QStringLiteral("ButtonNeutralText"), QStringLiteral("%buttonneutraltextcolor"));
        stylesheet += skel.arg(QStringLiteral("ButtonNegativeText"), QStringLiteral("%buttonnegativetextcolor"));

        stylesheet += skel.arg(QStringLiteral("ViewText"), QStringLiteral("%viewtextcolor"));
        stylesheet += skel.arg(QStringLiteral("ViewBackground"), QStringLiteral("%viewbackgroundcolor"));
        stylesheet += skel.arg(QStringLiteral("ViewHover"), QStringLiteral("%viewhovercolor"));
        stylesheet += skel.arg(QStringLiteral("ViewFocus"), QStringLiteral("%viewfocuscolor"));
        stylesheet += skel.arg(QStringLiteral("ViewHighlightedText"), QStringLiteral("%viewhighlightedtextcolor"));
        stylesheet += skel.arg(QStringLiteral("ViewPositiveText"), QStringLiteral("%viewpositivetextcolor"));
        stylesheet += skel.arg(QStringLiteral("ViewNeutralText"), QStringLiteral("%viewneutraltextcolor"));
        stylesheet += skel.arg(QStringLiteral("ViewNegativeText"), QStringLiteral("%viewnegativetextcolor"));

        stylesheet += skel.arg(QStringLiteral("ComplementaryText"), QStringLiteral("%complementarytextcolor"));
        stylesheet += skel.arg(QStringLiteral("ComplementaryBackground"), QStringLiteral("%complementarybackgroundcolor"));
        stylesheet += skel.arg(QStringLiteral("ComplementaryHover"), QStringLiteral("%complementaryhovercolor"));
        stylesheet += skel.arg(QStringLiteral("ComplementaryFocus"), QStringLiteral("%complementaryfocuscolor"));
        stylesheet += skel.arg(QStringLiteral("ComplementaryHighlightedText"), QStringLiteral("%complementaryhighlightedtextcolor"));
        stylesheet += skel.arg(QStringLiteral("ComplementaryPositiveText"), QStringLiteral("%complementarypositivetextcolor"));
        stylesheet += skel.arg(QStringLiteral("ComplementaryNeutralText"), QStringLiteral("%complementaryneutraltextcolor"));
        stylesheet += skel.arg(QStringLiteral("ComplementaryNegativeText"), QStringLiteral("%complementarynegativetextcolor"));

        stylesheet += skel.arg(QStringLiteral("HeaderText"), QStringLiteral("%headertextcolor"));
        stylesheet += skel.arg(QStringLiteral("HeaderBackground"), QStringLiteral("%headerbackgroundcolor"));
        stylesheet += skel.arg(QStringLiteral("HeaderHover"), QStringLiteral("%headerhovercolor"));
        stylesheet += skel.arg(QStringLiteral("HeaderFocus"), QStringLiteral("%headerfocuscolor"));
        stylesheet += skel.arg(QStringLiteral("HeaderHighlightedText"), QStringLiteral("%headerhighlightedtextcolor"));
        stylesheet += skel.arg(QStringLiteral("HeaderPositiveText"), QStringLiteral("%headerpositivetextcolor"));
        stylesheet += skel.arg(QStringLiteral("HeaderNeutralText"), QStringLiteral("%headerneutraltextcolor"));
        stylesheet += skel.arg(QStringLiteral("HeaderNegativeText"), QStringLiteral("%headernegativetextcolor"));

        stylesheet += skel.arg(QStringLiteral("TootipText"), QStringLiteral("%tooltiptextcolor"));
        stylesheet += skel.arg(QStringLiteral("TootipBackground"), QStringLiteral("%tooltipbackgroundcolor"));
        stylesheet += skel.arg(QStringLiteral("TootipHover"), QStringLiteral("%tooltiphovercolor"));
        stylesheet += skel.arg(QStringLiteral("TootipFocus"), QStringLiteral("%tooltipfocuscolor"));
        stylesheet += skel.arg(QStringLiteral("TootipHighlightedText"), QStringLiteral("%tooltiphighlightedtextcolor"));
        stylesheet += skel.arg(QStringLiteral("TootipPositiveText"), QStringLiteral("%tooltippositivetextcolor"));
        stylesheet += skel.arg(QStringLiteral("TootipNeutralText"), QStringLiteral("%tooltipneutraltextcolor"));
        stylesheet += skel.arg(QStringLiteral("TootipNegativeText"), QStringLiteral("%tooltipnegativetextcolor"));

        stylesheet = processStyleSheet(stylesheet, status);
        if (status == Svg::Status::Selected) {
            cachedSelectedSvgStyleSheets.insert(group, stylesheet);
        } else if (status == Svg::Status::Inactive) {
            cachedInactiveSvgStyleSheets.insert(group, stylesheet);
        } else {
            cachedSvgStyleSheets.insert(group, stylesheet);
        }
    }

    return stylesheet;
}

void ThemePrivate::settingsFileChanged(const QString &file)
{
    qCDebug(LOG_KSVG) << "settingsFile: " << file;
    if (file == themeMetadataPath) {
        const KPluginMetaData data = metaDataForTheme(basePath, themeName);
        if (!data.isValid() || themeVersion != data.version()) {
            scheduleThemeChangeNotification(SvgElementsCache);
        }
    } else if (file.endsWith(QLatin1String(themeRcFile))) {
        config().config()->reparseConfiguration();
        settingsChanged(true);
    }
}

void ThemePrivate::settingsChanged(bool emitChanges)
{
    if (fixedName) {
        return;
    }
    // qCDebug(LOG_KSVG) << "Settings Changed!";
    KConfigGroup cg = config();
    setThemeName(cg.readEntry("name", ThemePrivate::defaultTheme), false, emitChanges);
}

QColor ThemePrivate::color(Theme::ColorRole role, Theme::ColorGroup group) const
{
    const KColorScheme *scheme = nullptr;

    // Before 5.0 Plasma theme really only used Normal and Button
    // many old themes are built on this assumption and will break
    // otherwise
    if (apiMajor < 5 && group != Theme::NormalColorGroup) {
        group = Theme::ButtonColorGroup;
    }

    switch (group) {
    case Theme::ButtonColorGroup: {
        scheme = &buttonColorScheme;
        break;
    }

    case Theme::ViewColorGroup: {
        scheme = &viewColorScheme;
        break;
    }

    // this doesn't have a real kcolorscheme
    case Theme::ComplementaryColorGroup: {
        scheme = &complementaryColorScheme;
        break;
    }

    case Theme::HeaderColorGroup: {
        scheme = &headerColorScheme;
        break;
    }

    case Theme::ToolTipColorGroup: {
        scheme = &tooltipColorScheme;
        break;
    }

    case Theme::NormalColorGroup:
    default: {
        scheme = &colorScheme;
        break;
    }
    }

    switch (role) {
    case Theme::TextColor:
        return scheme->foreground(KColorScheme::NormalText).color();

    case Theme::BackgroundColor:
        return scheme->background(KColorScheme::NormalBackground).color();

    case Theme::HoverColor:
        return scheme->decoration(KColorScheme::HoverColor).color();

    case Theme::HighlightColor:
        return selectionColorScheme.background(KColorScheme::NormalBackground).color();

    case Theme::FocusColor:
        return scheme->decoration(KColorScheme::FocusColor).color();

    case Theme::LinkColor:
        return scheme->foreground(KColorScheme::LinkText).color();

    case Theme::VisitedLinkColor:
        return scheme->foreground(KColorScheme::VisitedText).color();

    case Theme::HighlightedTextColor:
        return selectionColorScheme.foreground(KColorScheme::NormalText).color();

    case Theme::PositiveTextColor:
        return scheme->foreground(KColorScheme::PositiveText).color();
    case Theme::NeutralTextColor:
        return scheme->foreground(KColorScheme::NeutralText).color();
    case Theme::NegativeTextColor:
        return scheme->foreground(KColorScheme::NegativeText).color();
    case Theme::DisabledTextColor:
        return scheme->foreground(KColorScheme::InactiveText).color();
    }

    return QColor();
}

bool ThemePrivate::findInCache(const QString &key, QPixmap &pix, unsigned int lastModified)
{
    // TODO KF6: Make lastModified non-optional.
    if (lastModified == 0) {
        qCWarning(LOG_KSVG) << "findInCache with a lastModified timestamp of 0 is deprecated";
        return false;
    }

    if (!useCache()) {
        return false;
    }

    if (lastModified > uint(pixmapCache->lastModifiedTime().toSecsSinceEpoch())) {
        return false;
    }

    const QString id = keysToCache.value(key);
    const auto it = pixmapsToCache.constFind(id);
    if (it != pixmapsToCache.constEnd()) {
        pix = *it;
        return !pix.isNull();
    }

    QPixmap temp;
    if (pixmapCache->findPixmap(key, &temp) && !temp.isNull()) {
        pix = temp;
        return true;
    }

    return false;
}

void ThemePrivate::insertIntoCache(const QString &key, const QPixmap &pix)
{
    if (useCache()) {
        pixmapCache->insertPixmap(key, pix);
    }
}

void ThemePrivate::insertIntoCache(const QString &key, const QPixmap &pix, const QString &id)
{
    if (useCache()) {
        pixmapsToCache[id] = pix;
        keysToCache[key] = id;
        idsToCache[id] = key;

        // always start timer in pixmapSaveTimer's thread
        QMetaObject::invokeMethod(pixmapSaveTimer, "start", Qt::QueuedConnection);
    }
}

void ThemePrivate::setThemeName(const QString &tempThemeName, bool writeSettings, bool emitChanged)
{
    QString theme = tempThemeName;
    if (theme.isEmpty() || theme == themeName) {
        // let's try and get the default theme at least
        if (themeName.isEmpty()) {
            theme = QLatin1String(ThemePrivate::defaultTheme);
        } else {
            return;
        }
    }

    // we have one special theme: essentially a dummy theme used to cache things with
    // the system colors.
    bool realTheme = theme != QLatin1String(systemColorsTheme);
    if (realTheme) {
        KPluginMetaData data = metaDataForTheme(basePath, theme);
        if (!data.isValid()) {
            data = metaDataForTheme(basePath, QStringLiteral("default"));
            if (!data.isValid()) {
                return;
            }

            theme = QLatin1String(ThemePrivate::defaultTheme);
        }
    }

    // check again as ThemePrivate::defaultTheme might be empty
    if (themeName == theme) {
        return;
    }

    themeName = theme;

    // load the color scheme config
    const QString colorsFile = realTheme ? QStandardPaths::locate(QStandardPaths::GenericDataLocation, basePath % theme % QLatin1String("/colors")) : QString();

    // qCDebug(LOG_KSVG) << "we're going for..." << colorsFile << "*******************";

    if (colorsFile.isEmpty()) {
        colors = nullptr;
    } else {
        colors = KSharedConfig::openConfig(colorsFile);
    }

    colorScheme = KColorScheme(QPalette::Active, KColorScheme::Window, colors);
    selectionColorScheme = KColorScheme(QPalette::Active, KColorScheme::Selection, colors);
    buttonColorScheme = KColorScheme(QPalette::Active, KColorScheme::Button, colors);
    viewColorScheme = KColorScheme(QPalette::Active, KColorScheme::View, colors);
    complementaryColorScheme = KColorScheme(QPalette::Active, KColorScheme::Complementary, colors);
    headerColorScheme = KColorScheme(QPalette::Active, KColorScheme::Header, colors);
    tooltipColorScheme = KColorScheme(QPalette::Active, KColorScheme::Tooltip, colors);
    palette = KColorScheme::createApplicationPalette(colors);

    if (realTheme) {
        pluginMetaData = metaDataForTheme(basePath, theme);
        KSharedConfigPtr metadata = configForTheme(basePath, theme);

        KConfigGroup cg(metadata, "Settings");
        QString fallback = cg.readEntry("FallbackTheme", QString());

        fallbackThemes.clear();
        while (!fallback.isEmpty() && !fallbackThemes.contains(fallback)) {
            fallbackThemes.append(fallback);

            KSharedConfigPtr metadata = configForTheme(basePath, fallback);
            KConfigGroup cg(metadata, "Settings");
            fallback = cg.readEntry("FallbackTheme", QString());
        }

        if (!fallbackThemes.contains(QLatin1String(ThemePrivate::defaultTheme))) {
            fallbackThemes.append(QLatin1String(ThemePrivate::defaultTheme));
        }

        // Check for what Plasma version the theme has been done
        // There are some behavioral differences between KDE4 Plasma and Plasma 5
        const QString apiVersion = pluginMetaData.value(QStringLiteral("X-Plasma-API"));
        apiMajor = 1;
        apiMinor = 0;
        apiRevision = 0;
        if (!apiVersion.isEmpty()) {
            const QVector<QStringView> parts = QStringView(apiVersion).split(QLatin1Char('.'));
            if (!parts.isEmpty()) {
                apiMajor = parts.value(0).toInt();
            }
            if (parts.count() > 1) {
                apiMinor = parts.value(1).toInt();
            }
            if (parts.count() > 2) {
                apiRevision = parts.value(2).toInt();
            }
        }
    }

    if (realTheme && isDefault && writeSettings) {
        // we're the default theme, let's save our status
        KConfigGroup &cg = config();
        cg.writeEntry("name", themeName);
        cg.sync();
    }

    if (emitChanged) {
        scheduleThemeChangeNotification(PixmapCache | SvgElementsCache);
    }
}

bool ThemePrivate::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == QCoreApplication::instance()) {
        if (event->type() == QEvent::ApplicationPaletteChange) {
            colorsChanged();
        }
    }
    return QObject::eventFilter(watched, event);
}

}

#include "moc_theme_p.cpp"