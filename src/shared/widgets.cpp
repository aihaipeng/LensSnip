#include "shared/widgets.h"
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QIcon>
#include <QColor>
#include <QPaintEvent>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QtMath>
#include <QToolButton>
#include <QHash>
#include <QRegion>
#include <QVariant>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <QFrame>
#include <QAbstractItemView>
#include <QSvgRenderer>
#include <qt_windows.h>

namespace {
struct IconVariant { QColor color; QIcon::State state; QList<QIcon::Mode> modes; };

QIcon renderIcon(Icon icon, int size, int style, std::initializer_list<IconVariant> variants)
{
    // Rendering one icon parses the SVG and rasterizes up to 12 variant pixmaps;
    // toolbars ask for the same set on every build, so cache by icon, size and style.
    static QHash<qint64, QIcon> cache;
    const qint64 key = (qint64(icon) * 1000 + size) * 4 + style;
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd()) return *cached;
    static const char *const names[] = {
        "save", "copy", "pin", "long_shot", "icons8-ocr", "cancel", "rect", "ellipse", "arrow", "pen", "marker",
        "text", "number", "mosaic", "eraser", "undo", "redo", "bold", "italic", "minimize", "maximize",
        "restore", "refresh", "fill", "icons8-line-width", "marker_rect", "fit_view", "search",
        "text_format", "split_columns", "split_rows", "chevron_up", "chevron_down", "chevron_right",
        "icons8-wavy-line", "icons8-underline", "icons8-strikethrough", "check", "icons8-data-transfer", "card-view",
        "card-fit", "card-translate", "card-reload", "card-copy",
        "card-zoom-in", "card-zoom-out", "card-actual", "card-pin", "card-minimize", "card-maximize",
        "card-restore", "card-close", "card-search", "card-up", "card-down", "card-right", "card-format",
        "back", "crop", "crop_start", "crop_end", "barcode", "card-link", "round_rect",
        "corner_square", "corner_rounded", "aspect_lock", "warning", "sliders", "clock", "picker",
        "magic_eraser", "auto_mosaic", "watermark", "more", "folder"};
    // A shared stroke grid keeps primary tools and card actions visually aligned.
    const char *path = nullptr;
    const char *markup = nullptr;
    switch (icon) {
    case Icon::Save: path = "M4 3h13l4 4v14H3V3z M7 3v6h10V3 M7 21v-8h10v8"; break;
    case Icon::Copy: case Icon::CardCopy: path = "M9 8h11v13H9z M5 16H3V3h12v2"; break;
    case Icon::Pin: case Icon::CardPin: path = "M8 3h8 M9 3v6l-4 5h14l-4-5V3 M12 14v8"; break;
    case Icon::Rect: path = "M4 5h16v14H4z"; break;
    case Icon::Ellipse: path = "M20 12a8 7 0 1 1-16 0a8 7 0 1 1 16 0"; break;
    case Icon::Arrow: path = "M5 19L19 5 M8 5h11v11"; break;
    case Icon::Pen: path = "M4 20l4-1 12-12-3-3L5 16z M14 7l3 3"; break;
    case Icon::Marker: path = "M5 14l9-10 6 6-10 9z M4 21h9"; break;
    case Icon::Text: path = "M4 5h16 M12 5v15 M8 20h8"; break;
    case Icon::Number: path = "M8 4L6 20 M17 4l-2 16 M3 9h18 M2 15h18"; break;
    case Icon::Mosaic:
        // A framed checkerboard is the clearest pixelation glyph; filled cells avoid a layout-grid read.
        markup = R"(<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><rect x='3' y='3' width='18' height='18' rx='1.5' fill='none' stroke='white' stroke-width='1.6'/><path fill='white' d='M5 5h4v4H5z M13 5h4v4h-4z M9 9h4v4H9z M17 9h4v4h-4z M5 13h4v4H5z M13 13h4v4h-4z M9 17h4v4H9z M17 17h4v4h-4z'/></svg>)";
        break;
    case Icon::AutoMosaic:
        // Checkerboard plus a sparkle: the tool finds identifiers instead of drawing one box.
        markup = R"(<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><rect x='2' y='3' width='15' height='15' rx='1.5' fill='none' stroke='white' stroke-width='1.6'/><path fill='white' d='M4 5h4v4H4z M11 5h4v4h-4z M7.5 9h4v4h-4z M4 13h4v4H4z M11 13h4v4h-4z'/><path fill='white' d='M20 2l1 2.5L23.5 5.5 21 6.5 20 9l-1-2.5L16.5 5.5 19 4.5z'/></svg>)";
        break;
    case Icon::LineWidth:
        // Four strokes from hairline to bold; the box crops the source icon's empty margin.
        markup = R"(<svg xmlns='http://www.w3.org/2000/svg' viewBox='4 8 42 34' fill='white'><path d='M6 10h38v2H6z M6 17h38v3H6z M6 25h38v4H6z M6 34h38v6H6z'/></svg>)";
        break;
    case Icon::MarkerRect:
        // A plain rectangle; the old checkerboard aliased into stripes at button size.
        path = "M4 5h16v14H4z";
        break;
    case Icon::Eraser: path = "M3 14l10-10 8 8-9 9H9z M9 8l8 8 M12 21h10"; break;
    // A smaller eraser plus two sparkles separates the content-aware mode from plain erasing.
    case Icon::MagicEraser: path = "M2 16l8-8 6 6-8 8H6z M7 11l6 6 M12 22h10 M19 2v4 M17 4h4 M21 9v2 M20 10h2"; break;
    case Icon::Watermark:
        // A water drop with a small text bar: the annotation-tool glyph for a text watermark.
        markup = R"(<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='white' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='M12 3C9.2 7.6 6 10.3 6 14a6 6 0 0 0 12 0c0-3.7-3.2-6.4-6-11z'/><path d='M9 13h6 M12 13v4'/></svg>)";
        break;
    case Icon::Undo: path = "M8 6L3 11l5 5 M4 11h10a6 6 0 0 1 6 6"; break;
    case Icon::Redo: path = "M16 6l5 5-5 5 M20 11H10a6 6 0 0 0-6 6"; break;
    case Icon::Cancel: case Icon::CardClose: path = "M6 6l12 12 M18 6L6 18"; break;
    case Icon::Refresh: case Icon::CardReload: path = "M20 8a8 8 0 1 0 1 7 M20 3v6h-6"; break;
    case Icon::Ocr: path = "M3 8V3h5 M16 3h5v5 M21 16v5h-5 M8 21H3v-5 M8 18l3.5-11 3.5 11 M9.4 14.5h4.2"; break;
    case Icon::Barcode:
        // Three finder rings plus data dots; distinct from the framed pixel tiles used by mosaic.
        markup = R"(<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='white'><path fill-rule='evenodd' d='M3 3h7v7H3z M5 5h3v3H5z M6 6h1.5v1.5H6z M14 3h7v7h-7z M16 5h3v3h-3z M17 6h1.5v1.5H17z M3 14h7v7H3z M5 16h3v3H5z M6 17h1.5v1.5H6z M13 13h2v2h-2z M17 13h2v2h-2z M20 13h1v1h-1z M13 17h2v2h-2z M17 17h2v2h-2z M20 17h1v4h-1z M13 20h2v1h-2z M17 20h2v1h-2z'/></svg>)";
        break;
    case Icon::Translate: case Icon::CardTranslate: path = "M3 5h11 M9 2v3 M5 5c0 6 5 10 9 11 M12 5c-1 7-4 11-9 13 M14 21l4-11 4 11 M16 17h4"; break;
    case Icon::LongShot: path = "M5 3h14v18H5z M9 8l3-3 3 3 M12 5v14 M9 16l3 3 3-3"; break;
    case Icon::RoundRect: path = "M4 14V9a5 5 0 0 1 5-5h5 M4 18v2h2 M18 4h2v2 M20 18v2h-2"; break;
    case Icon::CornerSquare: path = "M4 20V4h16"; break;
    case Icon::CornerRounded: path = "M4 20V10a6 6 0 0 1 6-6h10"; break;
    case Icon::AspectLock: path = "M8 10V7a4 4 0 0 1 8 0v3 M5 10h14v10H5z"; break;
    case Icon::Search: case Icon::CardSearch: path = "M16 10a6 6 0 1 1-12 0a6 6 0 1 1 12 0 M15 15l6 6"; break;
    case Icon::Back: path = "M14 5l-7 7 7 7 M7 12h14"; break;
    case Icon::Minimize: case Icon::CardMinimize: path = "M5 12h14"; break;
    case Icon::Maximize: case Icon::CardMaximize: path = "M5 5h14v14H5z"; break;
    case Icon::View: path = "M2 12c6-9 14-9 20 0-6 9-14 9-20 0 M15 12a3 3 0 1 1-6 0a3 3 0 1 1 6 0"; break;
    case Icon::Warning: path = "M12 3L2 21h20z M12 9v6 M12 18v.1"; break;
    case Icon::Check: path = "M4 12l5 5L20 6"; break;
    case Icon::Clock: path = "M21 12a9 9 0 1 1-18 0a9 9 0 1 1 18 0 M12 7v5l3 2"; break;
    case Icon::Picker: path = "M14 3l7 7 M17 3l4 4 M13 6l5 5-9 9H4v-5z"; break;
    case Icon::Folder: path = "M3 7h6l2 2h10v10H3z M3 7v12"; break;
    default: break;
    }
    QSvgRenderer svg;
    if (markup) svg.load(QByteArray(markup));
    else if (path) svg.load(QByteArray("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='white' stroke-width='1.6' stroke-linecap='round' stroke-linejoin='round'><path d='") + path + "'/></svg>");
    else svg.load(QStringLiteral(":/icons/%1.svg").arg(QLatin1String(names[int(icon)])));
    QIcon result;
    for (const auto &variant : variants)
    for (int px : {size, size * 3 / 2, size * 2}) {   // 1x / 1.5x / 2x variants of one logical size.
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(px / qreal(size));
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        svg.render(&p, QRectF(0, 0, size, size));
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);   // Recolor the glyph, keep its alpha.
        p.fillRect(QRectF(0, 0, size, size), variant.color);
        p.end();
        for (QIcon::Mode mode : variant.modes) result.addPixmap(pm, mode, variant.state);
    }
    cache.insert(key, result);
    return result;
}
}

QIcon makeIcon(Icon icon, int size, bool dark)
{
    // Light bar language: dark glyphs, grey when disabled, white on the checked theme fill.
    const QColor ink = dark ? Bar::ink : QColor(0x32, 0x32, 0x32);
    const QColor off = dark ? Bar::dim : QColor(0x7b, 0x7b, 0x7b);
    return renderIcon(icon, size, dark, {
        {ink, QIcon::Off, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
        {off, QIcon::Off, {QIcon::Disabled}},
        {off, QIcon::On, {QIcon::Disabled}},
        {QColor(Qt::white), QIcon::On, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
    });
}

QIcon cardIcon(Icon icon, int size)
{
    // Card glyphs share the chrome palette and tint on hover or selection.
    const QColor muted = Bar::muted, accent = Bar::accent, off = Bar::dim;
    return renderIcon(icon, size, 2, {
        {muted, QIcon::Off, {QIcon::Normal, QIcon::Selected}},
        {accent, QIcon::Off, {QIcon::Active}},
        {off, QIcon::Off, {QIcon::Disabled}},
        {off, QIcon::On, {QIcon::Disabled}},
        {accent, QIcon::On, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
    });
}

QIcon subtleIcon(Icon icon, int size)
{
    const QColor muted = Bar::muted, ink = Bar::ink, off = Bar::dim;
    return renderIcon(icon, size, 5, {
        {muted, QIcon::Off, {QIcon::Normal, QIcon::Selected}},
        {ink, QIcon::Off, {QIcon::Active}},
        {off, QIcon::Off, {QIcon::Disabled}},
        {off, QIcon::On, {QIcon::Disabled}},
    });
}

QToolButton *iconButton(Icon icon, const QString &tip)
{
    auto *button = new QToolButton;
    button->setIcon(makeIcon(icon));
    button->setIconSize(QSize(20, 20));
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QIcon barIcon(Icon icon, int size)
{
    return renderIcon(icon, size, 3, {
        {Bar::ink, QIcon::Off, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
        {Bar::dim, QIcon::Off, {QIcon::Disabled}},
        {Bar::dim, QIcon::On, {QIcon::Disabled}},
        {Bar::accent, QIcon::On, {QIcon::Normal, QIcon::Active, QIcon::Selected}},
    });
}

QToolButton *barButton(Icon icon, const QString &tip)
{
    auto *button = iconButton(icon, tip);
    button->setIcon(barIcon(icon));
    button->setIconSize(QSize(Bar::icon, Bar::icon));
    button->setFixedSize(Bar::button, Bar::button);
    return button;
}

void prepareComboPopup(QComboBox *combo)
{
    if (!combo || !combo->view()) return;
    const bool compact = combo->count() <= 10;
    combo->view()->setVerticalScrollBarPolicy(compact ? Qt::ScrollBarAlwaysOff
                                                      : Qt::ScrollBarAsNeeded);
    combo->view()->setFrameShape(QFrame::NoFrame);
    // Short lists fit without the popup frame; long lists keep the slim scrollbar.
    combo->view()->setStyleSheet(compact
        ? QStringLiteral("QAbstractItemView{background:%1;color:%2;border:none;outline:0;padding:0px;}"
                         "QAbstractItemView::item{min-height:26px;}")
              .arg(Bar::surface.name(), Bar::ink.name())
        : QStringLiteral("QAbstractItemView::item{min-height:26px;}"));
    // Rows start where the closed combo's text starts, so the two columns line up.
    if (auto *delegate = dynamic_cast<PopupItemDelegate *>(combo->itemDelegate())) {
        QStyleOptionComboBox opt;
        opt.initFrom(combo);
        opt.editable = combo->isEditable();
        delegate->setContentInset(qMax(2, combo->style()->subControlRect(
            QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, combo).left()));
    }
}

void PopupItemDelegate::paintRow(QPainter *painter, const QStyleOptionViewItem &option)
{
    if (!(option.state & (QStyle::State_MouseOver | QStyle::State_Selected))) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(option.state & QStyle::State_Selected ? Bar::accent : QColor(0x3c, 0x3c, 0x3c));
    painter->drawRoundedRect(QRectF(option.rect).adjusted(1, 1, -1, -1), 4, 4);
    painter->restore();
}

void PopupItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    // Honor subclass initStyleOption(): glyph-only rows clear option.text there.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    paintRow(painter, opt);
    // Draw the row content directly so popup rows share the closed combo's
    // content inset instead of the style's smaller default item margin.
    const QRect row = opt.rect.adjusted(m_contentInset, 0, -10, 0);
    int textLeft = row.left();
    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    if (!icon.isNull()) {
        const int side = opt.decorationSize.width();
        icon.paint(painter, QRect(QPoint(textLeft, row.center().y() - side / 2), QSize(side, side)));
        textLeft += side + 4;
    }
    if (opt.text.isEmpty()) return;
    const QRect textRect(textLeft, row.top(), row.right() + 1 - textLeft, row.height());
    painter->setFont(opt.font);
    painter->setPen(opt.palette.color(QPalette::Text));
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                      opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, textRect.width()));
}

const QString darkSliderQss = QStringLiteral(
    "QSlider{margin:0px 1px 0px 1px;}"
    "QSlider::groove:horizontal{border:none;background:#2b2d33;height:4px;border-radius:2px;}"
    "QSlider::sub-page:horizontal{background:#5e6ad2;border-radius:2px;}"
    "QSlider::handle:horizontal{background:#ffffff;width:10px;margin:-3px 0px;border-radius:5px;}");

const QString darkMenuQss = QStringLiteral(
    "QMenu{background:#18191d;color:#f4f5f8;border:1px solid #2b2d33;padding:4px;border-radius:8px;}"
    "QMenu::item{min-height:22px;padding:6px 24px 6px 20px;border-radius:6px;}"
    "QMenu::item:selected{background:#5e6ad2;color:#ffffff;}"
    "QMenu::item:disabled{color:#73757f;}"
    "QMenu::separator{height:1px;background:#2b2d33;margin:4px 8px;}");

QToolButton *colorButton(const QColor &color, const QString &tip, int size)
{
    auto *button = iconButton(Icon::Copy, tip);   // Same button setup as the toolbar tools.
    button->setFixedSize(size, size);
    QPixmap chip(size, size);
    chip.fill(Qt::transparent);
    QPainter painter(&chip);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box(2.5, 2.5, size - 5, size - 5);
    painter.setPen(QPen(QColor("#777980"), 1));
    painter.setBrush(color);
    painter.drawRect(box);
    painter.end();
    button->setIcon(QIcon(chip));
    button->setIconSize(chip.size());
    return button;
}

QWidget *barSeparator(bool dark)
{
    auto *line = new QFrame;
    line->setObjectName(QStringLiteral("barSeparator"));
    if (dark) line->setFixedSize(13, 16);              // 1px line plus 6px margins each side.
    else line->setFixedSize(5, 14);                    // 1px line plus 2px margins each side.
    return line;
}

void applyRoundedMask(QWidget *widget, int radius)
{
    if (!widget || widget->size().isEmpty()) return;
    const QVariant last = widget->property("lenssnipMaskSize");
    if (last.isValid() && last.toSize() == widget->size()) return;
    QPainterPath path;
    path.addRoundedRect(QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    widget->setMask(QRegion(path.toFillPolygon().toPolygon()));
    widget->setProperty("lenssnipMaskSize", widget->size());
}

// The classic 20-color paint palette, dark row first.
static const QColor kPresets[ColorBoard::PresetCount] = {
    "#000000", "#7f7f7f", "#880015", "#ed1c24", "#ff7f27", "#fff200", "#22b14c", "#00a2e8", "#3f48cc", "#a349a4",
    "#ffffff", "#c3c3c3", "#b97a57", "#ffaec9", "#ffc90e", "#efe4b0", "#b5e61d", "#99d9ea", "#7092be", "#c8bfe7"};

const QColor &ColorBoard::preset(int index) { return kPresets[qBound(0, index, PresetCount - 1)]; }

ColorBoard::ColorBoard(const QColor &current, QWidget *parent, int commonCount)
    : QWidget(parent), m_current(current), m_commonCount(qBound(1, commonCount, 8))
{
    setObjectName(QStringLiteral("colorBoard"));
    setFixedSize(31 + 26 * m_commonCount, 28);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("颜色"));
    setAccessibleDescription(tr("当前颜色，点击或按 Enter 打开调色板；方向键选择预设，Enter 应用"));
    setToolTip(tr("当前颜色，点击打开调色板"));
}

QRect ColorBoard::swatchRect(int index, int commonCount)
{
    if (index < 0 || index >= commonCount) return {};
    return QRect(35 + 26 * index, 4, 20, 20);   // After the current chip and 1px divider; 6px gaps.
}

const QColor &ColorBoard::swatchColor(int index) const
{
    static constexpr int common[] = {0, 10, 3, 5, 6, 7, 4, 9};   // Black, white, red, yellow, green, blue, orange, purple.
    return preset(common[qBound(0, index, m_commonCount - 1)]);
}

void ColorBoard::setCurrentColor(const QColor &color)
{
    if (!color.isValid() || color == m_current) return;
    m_current = color;
    update(currentRect());
}

int ColorBoard::swatchAt(const QPoint &pos) const
{
    for (int i = 0; i < m_commonCount; ++i)
        if (swatchRect(i, m_commonCount).contains(pos)) return i;   // Gaps between cells pick nothing.
    return -1;
}

void ColorBoard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Same chip size as the presets; a rainbow ring marks the entry to the full palette.
    const QRectF big = currentRect();
    QConicalGradient rainbow(big.center(), 90);
    for (int k = 0; k <= 6; ++k) rainbow.setColorAt(k / 6.0, QColor::fromHsv((360 - 60 * k) % 360, 255, 255));
    p.setPen(QPen(QBrush(rainbow), 2));
    p.setBrush(m_current);
    p.drawRect(big.adjusted(1, 1, -1, -1));
    p.setPen(QPen(QColor("#3a3c42"), 1));
    p.drawLine(QPointF(28.5, 6), QPointF(28.5, 21));
    for (int i = 0; i < m_commonCount; ++i) {
        const QColor &c = swatchColor(i);
        const bool hovered = i == m_hover;
        const QRectF r = swatchRect(i, m_commonCount);
        p.setPen(QPen(hovered ? Bar::muted : QColor("#777980"), hovered ? 2 : 1));
        p.setBrush(c);
        p.drawRect(hovered ? r.adjusted(1, 1, -1, -1) : r.adjusted(0.5, 0.5, -0.5, -0.5));
    }
    if (hasFocus()) {
        const QRectF focusRect = m_keyIndex == 0
            ? currentRect() : swatchRect(m_keyIndex - 1, m_commonCount);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor("#828fff"), 2));
        p.drawRect(focusRect.adjusted(-1, -1, 1, 1));
    }
}

void ColorBoard::mouseMoveEvent(QMouseEvent *e)
{
    const int hover = swatchAt(e->pos());
    if (hover == m_hover) return;
    m_hover = hover;
    update();
}

void ColorBoard::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    const int index = swatchAt(e->pos());
    if (index >= 0) {
        m_keyIndex = index + 1;
        emit presetPicked(swatchColor(index));
    } else if (currentRect().contains(e->pos())) {
        m_keyIndex = 0;
        emit customRequested();
    }
    update();
}

void ColorBoard::leaveEvent(QEvent *)
{
    if (m_hover < 0) return;
    m_hover = -1;
    update();
}

void ColorBoard::keyPressEvent(QKeyEvent *e)
{
    const int count = m_commonCount + 1;
    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Up:
        m_keyIndex = (m_keyIndex + count - 1) % count;
        update();
        e->accept();
        return;
    case Qt::Key_Right:
    case Qt::Key_Down:
        m_keyIndex = (m_keyIndex + 1) % count;
        update();
        e->accept();
        return;
    case Qt::Key_Home:
        m_keyIndex = 0;
        update();
        e->accept();
        return;
    case Qt::Key_End:
        m_keyIndex = m_commonCount;
        update();
        e->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (m_keyIndex == 0) emit customRequested();
        else emit presetPicked(swatchColor(m_keyIndex - 1));
        e->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}

void ColorBoard::focusInEvent(QFocusEvent *e)
{
    m_keyIndex = 0;
    update();
    QWidget::focusInEvent(e);
}

void ColorBoard::focusOutEvent(QFocusEvent *e)
{
    update();
    QWidget::focusOutEvent(e);
}
