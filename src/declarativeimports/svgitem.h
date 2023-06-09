/*
    SPDX-FileCopyrightText: 2010 Marco Martin <mart@kde.org>
    SPDX-FileCopyrightText: 2014 David Edmundson <davidedmundson@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef SVGITEM_P
#define SVGITEM_P

#include <QImage>
#include <QQuickItem>

namespace Kirigami
{
class PlatformTheme;
};

namespace KSvg
{
class Svg;

/**
 * @class SvgItem
 * @short Displays an SVG or an element from an SVG file
 */
class SvgItem : public QQuickItem
{
    Q_OBJECT

    /**
     * Theme relative path of the svg, like "widgets/background"
     */
    Q_PROPERTY(QString imagePath READ imagePath WRITE setImagePath NOTIFY imagePathChanged)

    /**
     * The sub element of the svg we want to render. If empty the whole svg document will be painted.
     */
    Q_PROPERTY(QString elementId READ elementId WRITE setElementId NOTIFY elementIdChanged)

    /**
     * The natural, unscaled size of the svg document or the element. useful if a pixel perfect rendering of outlines is needed.
     */
    Q_PROPERTY(QSizeF naturalSize READ naturalSize NOTIFY naturalSizeChanged)

public:
    /// @cond INTERNAL_DOCS

    explicit SvgItem(QQuickItem *parent = nullptr);
    ~SvgItem() override;

    void setImagePath(const QString &path);
    QString imagePath() const;

    void setElementId(const QString &elementID);
    QString elementId() const;

    QSizeF naturalSize() const;

    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *updatePaintNodeData) override;
    /// @endcond

protected:
    void componentComplete() override;

Q_SIGNALS:
    void imagePathChanged();
    void elementIdChanged();
    void naturalSizeChanged();

protected Q_SLOTS:
    /// @cond INTERNAL_DOCS
    void updateNeeded();
    /// @endcond

private:
    void scheduleImageUpdate();
    void updatePolish() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

    KSvg::Svg *m_svg;
    Kirigami::PlatformTheme *m_kirigamiTheme;
    QString m_elementID;
    bool m_textureChanged;
    QImage m_image;
};
}

#endif
