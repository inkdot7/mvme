#include "mvme_qwt.h"
#include <QtMath>
#include <qwt_painter.h>

/* Note (flueke): Code based on qwt_plot_textlabel.{h,cpp} from qwt-6.1.3 */

namespace mvme_qwt
{

//
// TextLabelItem
//

namespace
{

QRect qwtItemRect(int renderFlags, const QRectF &rect, const QSizeF &itemSize)
{
    int x;
    if ( renderFlags & Qt::AlignLeft )
    {
        x = rect.left();
    }
    else if ( renderFlags & Qt::AlignRight )
    {
        x = rect.right() - itemSize.width();
    }
    else
    {
        x = rect.center().x() - 0.5 * itemSize.width();
    }

    int y;
    if ( renderFlags & Qt::AlignTop )
    {
        y = rect.top();
    }
    else if ( renderFlags & Qt::AlignBottom )
    {
        y = rect.bottom() - itemSize.height();
    }
    else
    {
        y = rect.center().y() - 0.5 * itemSize.height();
    }

    return QRect( x, y, itemSize.width(), itemSize.height() );
}

} // end anon namespace

struct TextLabelItem::Private
{
    QwtText m_text;
    QPixmap m_cachedPixmap;
    TextLabelRowLayout *m_parentLayout = nullptr;
};

TextLabelItem::TextLabelItem(const QwtText &title)
    : QwtPlotItem(title)
    , m_d(std::make_unique<Private>())
{
}

TextLabelItem::~TextLabelItem()
{
}

void TextLabelItem::setText(const QwtText &text)
{
    m_d->m_text = text;
    invalidateCache();
}

QwtText TextLabelItem::text() const
{
    return m_d->m_text;
}

void TextLabelItem::setParentLayout(TextLabelRowLayout *layout)
{
    m_d->m_parentLayout = layout;
}

void TextLabelItem::invalidateCache()
{
    m_d->m_cachedPixmap = {};
}

void TextLabelItem::draw(QPainter *painter,
                         const QwtScaleMap &, const QwtScaleMap &,
                         const QRectF &canvasRect) const
{
    QRectF rect;

    if (m_d->m_parentLayout)
    {
        rect = m_d->m_parentLayout->getPaintArea(this, painter, canvasRect);
    }
    else
    {
        rect = qwtItemRect(m_d->m_text.renderFlags(), canvasRect,
                           m_d->m_text.textSize(painter->font()));
    }

    bool doCache = QwtPainter::roundingAlignment( painter );
    if ( doCache )
    {
        switch( painter->paintEngine()->type() )
        {
            case QPaintEngine::Picture:
            case QPaintEngine::User: // usually QwtGraphic
            {
                // don't use a cache for record/replay devices
                doCache = false;
                break;
            }
            default:;
        }
    }

    if (doCache)
    {
        // when the paint device is aligning it is not one
        // where scalability matters ( PDF, SVG ).
        // As rendering a text label is an expensive operation
        // we use a cache.

        int pw = 0;
        if (m_d->m_text.borderPen().style() != Qt::NoPen )
            pw = qMax( m_d->m_text.borderPen().width(), 1 );

        QRect pixmapRect;
        pixmapRect.setLeft( qFloor( rect.left() ) - pw );
        pixmapRect.setTop( qFloor( rect.top() ) - pw );
        pixmapRect.setRight( qCeil( rect.right() ) + pw );
        pixmapRect.setBottom( qCeil( rect.bottom() ) + pw );

#define QWT_HIGH_DPI 1

#if QT_VERSION >= 0x050100 && QWT_HIGH_DPI
        const qreal pixelRatio = painter->device()->devicePixelRatio();
        const QSize scaledSize = pixmapRect.size() * pixelRatio;
#else
        const QSize scaledSize = pixmapRect.size();
#endif
        if ( m_d->m_cachedPixmap.isNull() ||
            ( scaledSize != m_d->m_cachedPixmap.size() )  )
        {
            m_d->m_cachedPixmap = QPixmap( scaledSize );
#if QT_VERSION >= 0x050100 && QWT_HIGH_DPI
            m_d->m_cachedPixmap.setDevicePixelRatio( pixelRatio );
#endif
            m_d->m_cachedPixmap.fill( Qt::transparent );

            const QRect r( pw, pw,
                pixmapRect.width() - 2 * pw, pixmapRect.height() - 2 * pw );

            QPainter pmPainter( &m_d->m_cachedPixmap );
            m_d->m_text.draw( &pmPainter, r );
        }

        painter->drawPixmap( pixmapRect, m_d->m_cachedPixmap );
    }
    else
    {
        m_d->m_text.draw( painter, rect );
    }
}

//
// Layout
//

struct TextLabelRowLayout::Private
{
    QVector<TextLabelItem *> m_labels;
    int m_marginTop = 18;
    int m_marginRight = 18;
    int m_spacing = 10;
};

TextLabelRowLayout::TextLabelRowLayout()
    : m_d(std::make_unique<Private>())
{
}

TextLabelRowLayout::~TextLabelRowLayout()
{
}

TextLabelRowLayout *TextLabelItem::getParentLayout() const
{
    return m_d->m_parentLayout;
}

void TextLabelRowLayout::addTextLabel(TextLabelItem *label)
{
    m_d->m_labels.push_back(label);

    if (label->getParentLayout())
        label->getParentLayout()->removeTextLabel(label);

    label->setParentLayout(this);
}

QVector<TextLabelItem *> TextLabelRowLayout::getTextLabels() const
{
    return m_d->m_labels;
}

int TextLabelRowLayout::size() const
{
    return m_d->m_labels.size();
}

void TextLabelRowLayout::removeTextLabel(TextLabelItem *label)
{
    removeTextLabel(m_d->m_labels.indexOf(label));
}

void TextLabelRowLayout::removeTextLabel(int index)
{
    if (0 <= index && index < size())
        m_d->m_labels.remove(index);
}

QRectF TextLabelRowLayout::getPaintArea(const TextLabelItem *label,
                                        QPainter *painter,
                                        const QRectF &canvasRect) const
{
    int yOffset = getMarginTop();
    int xOffset = getMarginRight();

    for (auto l: m_d->m_labels)
    {
        if (l == label) break;
        if (!l->isVisible()) continue;

        auto rect = qwtItemRect(l->text().renderFlags(), canvasRect,
                                l->text().textSize(painter->font()));

        xOffset += rect.width() + getSpacing();
    }

    auto result = qwtItemRect(label->text().renderFlags(), canvasRect,
                              label->text().textSize(painter->font()));

    result.moveRight(result.right() - xOffset);
    result.moveTop(result.top() + yOffset);

    return result;
}

void TextLabelRowLayout::attachAll(QwtPlot *plot)
{
    for (auto label: m_d->m_labels)
        label->attach(plot);
}

void TextLabelRowLayout::setMarginTop(int margin)
{
    m_d->m_marginTop = margin;
}

int TextLabelRowLayout::getMarginTop() const
{
    return m_d->m_marginTop;
}

void TextLabelRowLayout::setMarginRight(int margin)
{
    m_d->m_marginRight = margin;
}

int TextLabelRowLayout::getMarginRight() const
{
    return m_d->m_marginRight;
}

void TextLabelRowLayout::setSpacing(int spacing)
{
    m_d->m_spacing = spacing;
}

int TextLabelRowLayout::getSpacing() const
{
    return m_d->m_spacing;
}

} // end namespace mvme_qwt
