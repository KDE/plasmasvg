/*
 *   Copyright (C) 2006 Aaron Seigo <aseigo@kde.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License version 2 as
 *   published by the Free Software Foundation
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "svg.h"

#include <QDir>
#include <QMatrix>
#include <QPainter>
#include <QPixmapCache>
#include <QSharedData>

#include <KDebug>
#include <KSharedPtr>
#include <KSvgRenderer>

#include "theme.h"

namespace Plasma
{

class SharedSvgRenderer : public KSvgRenderer, public QSharedData
{
    public:
        typedef KSharedPtr<SharedSvgRenderer> Ptr;

        SharedSvgRenderer(QObject *parent = 0)
            : KSvgRenderer(parent)
        {}

        SharedSvgRenderer(const QString &filename, QObject *parent = 0)
            : KSvgRenderer(filename, parent)
        {}

        SharedSvgRenderer(const QByteArray &contents, QObject *parent = 0)
            : KSvgRenderer(contents, parent)
        {}

        ~SharedSvgRenderer()
        {
            kDebug() << "leaving this world for a better one." << endl;
        }
};

class Svg::Private
{
    public:
        Private( const QString& imagePath )
            : renderer( 0 )
        {
            if (QDir::isAbsolutePath(themePath)) {
                path = imagePath;
                themed = false;
            } else {
                themePath = imagePath;
                themed = true;
            }
        }

        ~Private()
        {
            if (renderer.count() == 2) {
                // this and the cache reference it; and boy is this not thread safe ;)
                renderers.erase(renderers.find(themePath));
            }

            renderer = 0;
        }

        void removeFromCache()
        {
            if ( id.isEmpty() ) {
                return;
            }

            QPixmapCache::remove( id );
            id.clear();
        }

        void findInCache(QPixmap& p, const QString& elementId)
        {
            createRenderer();
            id = QString::fromLatin1("%3_%2_%1")
                                    .arg(size.width())
                                    .arg(size.height())
                                    .arg(path);
            if (!elementId.isEmpty()) {
                id.append(elementId);
            }

            if (QPixmapCache::find(id, p)) {
                //kDebug() << "found cached version of " << id << endl;
                return;
            } else {
                //kDebug() << "didn't find cached version of " << id << ", so re-rendering" << endl;
            }

            // we have to re-render this puppy
            QSize s;
            if (elementId.isEmpty()) {
                s = size.toSize();
            } else {
                s = elementSize(elementId);
            }
            //kDebug() << "size for " << elementId << " is " << s << endl;

            p = QPixmap(s);
            p.fill(Qt::transparent);
            QPainter renderPainter(&p);

            if (elementId.isEmpty()) {
                renderer->render(&renderPainter);
            } else {
                renderer->render(&renderPainter, elementId);
            }
            renderPainter.end();
            QPixmapCache::insert( id, p );
        }

        void createRenderer()
        {
            if (renderer) {
                return;
            }

            if (themed && path.isNull()) {
                path = Plasma::Theme::self()->image(themePath);
            }

            //TODO: connect the renderer's repaintNeeded to the Plasma::Svg signal
            //      take into consideration for cache, e.g. don't cache if svg is animated
            QHash<QString, SharedSvgRenderer::Ptr>::const_iterator it = renderers.find(path);

            if (it != renderers.end()) {
                kDebug() << "gots us an existing one!" << endl;
                renderer = it.value();
            } else {
                renderer = new SharedSvgRenderer(path);
                renderers[path] = renderer;
            }
        }

        QSize elementSize( const QString& elementId )
        {
            createRenderer();
            QSizeF elementSize = renderer->boundsOnElement(elementId).size();
            QSizeF naturalSize = renderer->defaultSize();
            qreal dx = size.width() / naturalSize.width();
            qreal dy = size.height() / naturalSize.height();
            elementSize.scale( elementSize.width() * dx, elementSize.height() * dy, Qt::IgnoreAspectRatio );

            return elementSize.toSize();
        }

        static QHash<QString, SharedSvgRenderer::Ptr> renderers;
        SharedSvgRenderer::Ptr renderer;
        QString themePath;
        QString path;
        QString id;
        QSizeF size;
        bool themed;
};

QHash<QString, SharedSvgRenderer::Ptr> Svg::Private:: renderers;

Svg::Svg( const QString& imagePath, QObject* parent )
    : QObject( parent ),
      d( new Private( imagePath ) )
{
    if (d->themed) {
        connect(Plasma::Theme::self(), SIGNAL(changed()), this, SLOT(themeChanged()));
    }
}

Svg::~Svg()
{

}

void Svg::paint(QPainter* painter, const QPointF& point, const QString& elementID)
{
    QPixmap pix;
    d->findInCache(pix, elementID);
    painter->drawPixmap(point.toPoint(), pix);
}

void Svg::paint(QPainter* painter, int x, int y, const QString& elementID)
{
    paint(painter, QPointF( x, y ), elementID);
}

void Svg::paint(QPainter* painter, const QRectF& rect, const QString& elementID)
{
    QPixmap pix;
    d->findInCache(pix, elementID);
    painter->drawPixmap(rect, pix, QRectF(QPointF(0,0), pix.size()));
}

void Svg::resize( int width, int height )
{
    resize( QSize( width, height ) );
}

void Svg::resize( const QSizeF& size )
{
    d->size = size;
}

void Svg::resize()
{
    d->createRenderer();
    d->size = d->renderer->defaultSize();
}

QSize Svg::elementSize(const QString& elementId) const
{
    return d->elementSize(elementId);
}

QSize Svg::size() const
{
    return d->size.toSize();
}

void Svg::themeChanged()
{
    d->removeFromCache();
    d->path.clear();
    //delete d->renderer; we're a KSharedPtr
    d->renderer = 0;
    emit repaintNeeded();
}

} // Plasma namespace

#include "svg.moc"

