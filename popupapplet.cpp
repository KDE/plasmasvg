/***************************************************************************
 *   Copyright (C) 2008 by Montel Laurent <montel@kde.org>                 *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA          *
 ***************************************************************************/

#include "popupapplet.h"

#include <QGraphicsProxyWidget>
#include <QGraphicsLinearLayout>
#include <QVBoxLayout>
#include <QTimer>

#include <KIcon>
#include <KIconLoader>

#include <plasma/dialog.h>
#include <plasma/widgets/icon.h>

namespace Plasma
{

class PopupAppletPrivate
{
public:
    PopupAppletPrivate(PopupApplet *applet)
        : q(applet),
          icon(0),
          dialog(0),
          layout(0),
          proxy(0),
          savedAspectRatio(Plasma::InvalidAspectRatioMode),
          timer(0)
    {
    }

    ~PopupAppletPrivate()
    {
        if (proxy) {
            proxy->setWidget(0);
        }

        delete dialog;
        delete icon;
    }

    void togglePopup();
    void hideTimedPopup();

    PopupApplet *q;
    Plasma::Icon *icon;
    Plasma::Dialog *dialog;
    QGraphicsLinearLayout *layout;
    QGraphicsProxyWidget *proxy;
    Plasma::AspectRatioMode savedAspectRatio;
    QTimer *timer;
};

PopupApplet::PopupApplet(QObject *parent, const QVariantList &args)
    : Plasma::Applet(parent, args),
      d(new PopupAppletPrivate(this))
{
    int iconSize = IconSize(KIconLoader::Desktop);
    resize(iconSize, iconSize);
}

PopupApplet::~PopupApplet()
{
    delete d;
}

void PopupApplet::setIcon(const QIcon &icon)
{
    if (!d->icon) {
        d->icon = new Plasma::Icon(icon, QString(), this);
    } else {
        d->icon->setIcon(icon);
    }
}

void PopupApplet::setIcon(const QString &iconName)
{
    if (!d->icon) {
        d->icon = new Plasma::Icon(KIcon(iconName), QString(), this);
    } else {
        d->icon->setIcon(iconName);
    }
}

QIcon PopupApplet::icon() const
{
    return d->icon->icon();
}

QWidget *PopupApplet::widget()
{
    return 0;
}

QGraphicsWidget *PopupApplet::graphicsWidget()
{
    return 0;
}

void PopupApplet::constraintsEvent(Plasma::Constraints constraints)
{
    if (constraints & Plasma::StartupCompletedConstraint) {
        if (!d->icon) {
            d->icon = new Plasma::Icon(KIcon("icons"), QString(), this);
        }

        setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
        setMaximumSize(INT_MAX, INT_MAX);
        d->layout = new QGraphicsLinearLayout(this);
        d->layout->setContentsMargins(0, 0, 0, 0);
        d->layout->setSpacing(0);
        d->layout->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
        d->layout->setMaximumSize(INT_MAX, INT_MAX);
        d->layout->setOrientation(Qt::Horizontal);
        setLayout(d->layout);

        connect(d->icon, SIGNAL(clicked()), this, SLOT(togglePopup()));
    }

    if (constraints & Plasma::FormFactorConstraint) {
        d->layout->removeAt(0);

        switch (formFactor()) {
        case Plasma::Planar:
        case Plasma::MediaCenter: {
            if (d->savedAspectRatio != Plasma::InvalidAspectRatioMode) {
                setAspectRatioMode(d->savedAspectRatio);
            }

            if (d->dialog) {
                if (d->dialog->layout() && widget()) {
                    //we dont want to delete Widget inside the dialog layout
                    d->dialog->layout()->removeWidget(widget());
                }

                delete d->dialog;
                d->dialog = 0;
            }

            //get the margins
            QSizeF marginSize = size() - contentsRect().size();

            if (graphicsWidget()) {
                d->layout->addItem(graphicsWidget());
                setMinimumSize(graphicsWidget()->minimumSize() + marginSize);
            }
            else {
                if (!d->proxy) {
                    d->proxy = new QGraphicsProxyWidget(this);
                    d->proxy->setWidget(widget());
                    d->proxy->show();
                }

                d->layout->addItem(d->proxy);
                setMinimumSize(widget() ? widget()->minimumSize() + marginSize : QSizeF(300, 200));
            }

            break;
        }
        case Plasma::Horizontal:
        case Plasma::Vertical:
            //save the aspect ratio mode in case we drag'n drop in the Desktop later
            d->savedAspectRatio = aspectRatioMode();
            setAspectRatioMode(Plasma::ConstrainedSquare);

            if (d->proxy) {
                d->proxy->setWidget(0); // prevent it from deleting our widget!
                delete d->proxy;
                d->proxy = 0;
            }

            if (!d->dialog) {
                d->dialog = new Plasma::Dialog();
                d->dialog->setWindowFlags(Qt::Popup);

                QVBoxLayout *l_layout = new QVBoxLayout(d->dialog);
                l_layout->setSpacing(0);
                l_layout->setMargin(0);

                if (graphicsWidget()) {
                    QGraphicsScene *scene = new QGraphicsScene(d->dialog);
                    QGraphicsView *view = new QGraphicsView(scene, d->dialog);

                    scene->addItem(graphicsWidget());
                    l_layout->addWidget(view);
                    view->show();
                } else {
                    l_layout->addWidget(widget());
                }
            }

            d->dialog->adjustSize();
            d->layout->addItem(d->icon);
            break;
        }
    }
}

void PopupApplet::showPopup(uint popupDuration)
{
    if (d->dialog && (formFactor() == Horizontal || formFactor() == Vertical)) {
        d->dialog->move(popupPosition(d->dialog->sizeHint()));
        d->dialog->show();

        if (d->timer) {
            d->timer->stop();
        }

        if (popupDuration > 0) {
            if (!d->timer) {
                d->timer = new QTimer(this);
                connect(d->timer, SIGNAL(timeout()), this, SLOT(hideTimedPopup()));
            }

            d->timer->start(popupDuration);
        }
    }
}

void PopupApplet::hidePopup()
{
    if (d->dialog && (formFactor() == Horizontal || formFactor() == Vertical)) {
        d->dialog->hide();
    }
}

void PopupAppletPrivate::togglePopup()
{
   if (dialog) {
        if (dialog->isVisible()) {
            dialog->hide();
        } else {
            dialog->move(q->popupPosition(dialog->sizeHint()));
            dialog->show();
        }

        dialog->clearFocus();
    }
}

void PopupAppletPrivate::hideTimedPopup()
{
    timer->stop();
    q->hidePopup();
}

} // Plasma namespace

#include "popupapplet.moc"

