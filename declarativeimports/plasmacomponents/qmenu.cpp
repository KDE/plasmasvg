/***************************************************************************
 *   Copyright 2011 Viranch Mehta <viranch.mehta@gmail.com>                *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA .        *
 ***************************************************************************/

#include "qmenu.h"

#include <QApplication>
#include <QDebug>
#include <QDesktopWidget>
#include <QGraphicsObject>
#include <QGraphicsView>
#include <QDeclarativeItem>

QMenuProxy::QMenuProxy (QObject *parent)
    : QObject(parent),
      m_status(DialogStatus::Closed)
{
    m_menu = new QMenu(0);
}

QMenuProxy::~QMenuProxy()
{
    delete m_menu;
}

QDeclarativeListProperty<QMenuItem> QMenuProxy::items()
{
    return QDeclarativeListProperty<QMenuItem>(this, m_items);
}

int QMenuProxy::actionCount() const
{
    return m_items.count();
}

QMenuItem *QMenuProxy::action(int index) const
{
    return m_items.at(index);
}

DialogStatus::Status QMenuProxy::status() const
{
    return m_status;
}

QDeclarativeItem *QMenuProxy::visualParent() const
{
    return m_visualParent.data();
}

void QMenuProxy::setVisualParent(QDeclarativeItem *parent)
{
    if (m_visualParent.data() == parent) {
        return;
    }

    m_visualParent = parent;
    emit visualParentChanged();
}

void QMenuProxy::clearMenuItems()
{
    qDeleteAll(m_items);
    m_items.clear();
}

void QMenuProxy::addMenuItem(const QString &text)
{
    QMenuItem *item = new QMenuItem(this);
    item->setText(text);
    m_items << item;
}

void QMenuProxy::showMenu(int x, int y)
{
    m_menu->clear();
    foreach(QMenuItem* item, m_items) {
        m_menu->addAction (item);
    }

    QPoint screenPos = QApplication::activeWindow()->mapToGlobal(QPoint(x, y));
    m_menu->popup(screenPos);
    m_status = DialogStatus::Open;
    emit statusChanged();
}

void QMenuProxy::open()
{
    m_menu->clear();

    foreach(QMenuItem* item, m_items) {
        m_menu->addAction (item);
    }
    m_menu->updateGeometry();

    QGraphicsObject *parentItem;
    if (m_visualParent) {
        parentItem = qobject_cast<QGraphicsObject *>(parent());
    } else {
        parentItem = m_visualParent.data();
    }

    if (!parentItem || !parentItem->scene()) {
        showMenu(0, 0);
        return;
    }

    QList<QGraphicsView*> views = parentItem->scene()->views();

    if (views.size() < 1) {
        showMenu(0, 0);
        return;
    }

    QGraphicsView *view = 0;
    if (views.size() == 1) {
        view = views[0];
    } else {
        QGraphicsView *found = 0;
        QGraphicsView *possibleFind = 0;
        foreach (QGraphicsView *v, views) {
            if (v->sceneRect().intersects(parentItem->sceneBoundingRect()) ||
                v->sceneRect().contains(parentItem->scenePos())) {
                if (v->isActiveWindow()) {
                    found = v;
                } else {
                    possibleFind = v;
                }
            }
        }
        view = found ? found : possibleFind;
    }

    if (!view) {
        showMenu(0, 0);
        return;
    }

    const QRect avail = QApplication::desktop()->availableGeometry(view);
    QPoint menuPos = view->mapToGlobal(view->mapFromScene(parentItem->scenePos()+QPoint(0, parentItem->boundingRect().height())));

    if (menuPos.y() + m_menu->sizeHint().height() > avail.bottom()) {
        menuPos = view->mapToGlobal(view->mapFromScene(parentItem->scenePos() - QPoint(0, m_menu->sizeHint().height())));
    }

    m_menu->popup(menuPos);
    m_status = DialogStatus::Open;
    emit statusChanged();
}

void QMenuProxy::close()
{
    m_menu->hide();
    m_status = DialogStatus::Closed;
    emit statusChanged();
}

#include "qmenu.moc"

