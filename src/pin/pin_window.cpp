#include <algorithm>
#include "pin/pin_window.h"
#include "text/cards/ocr_dialog.h"
#include "capture/capture_overlay.h"
#include <QPointer>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QColor>
#include <QAction>
#include <QWidgetAction>
#include <QPaintEvent>
#include <QContextMenuEvent>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QSlider>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QButtonGroup>
#include <QWidgetAction>
#include <QTransform>
#include <QtMath>
#include "shared/export_dialog.h"
#include <QStandardPaths>
#include <QCursor>
#include <QMessageBox>
#include <QCoreApplication>
#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <QSizePolicy>
#include <QLayout>
#include <QTimer>
#include <QToolTip>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QThread>
#include <qt_windows.h>

// Pin chrome uses the same dark glyph set as the capture bars, at a compact 28px rhythm.
static QToolButton *pinButton(Icon icon, const QString &tip)
{
    auto *button = barButton(icon, tip);
    button->setIcon(barIcon(icon, 18));
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(28, 28);
    return button;
}

PinWindow::PinWindow(const QPixmap &pix, const QRect &displayRect)
    : m_originalPix(pix), m_pix(pix), m_scale(pix.isNull() ? 1 : qMin(double(displayRect.width()) / pix.width(), double(displayRect.height()) / pix.height()))
{
    if (pix.isNull() || displayRect.isEmpty()) return;
    // Pins use one pixel contract: source pixels are not re-scaled by a DPR.
    m_originalPix.setDevicePixelRatio(1.0);
    m_pix.setDevicePixelRatio(1.0);
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_DeleteOnClose);
    setCursor(Qt::SizeAllCursor);
    setAccessibleName(tr("贴图"));
    setMouseTracking(true);
    m_textStatus = new QLabel(this);
    m_textStatus->setObjectName(QStringLiteral("pinTextStatus"));
    m_textStatus->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_textStatus->setStyleSheet(QStringLiteral(
        "QLabel{background:rgba(24,25,29,232);color:#9c9da1;border:1px solid #2b2d33;"
        "border-radius:6px;padding:4px 8px;font-size:12px;}"));
    m_textStatus->hide();
    m_scaleBadge = new QLabel(this);
    m_scaleBadge->setObjectName(QStringLiteral("pinScaleBadge"));
    m_scaleBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_scaleBadge->setStyleSheet(QStringLiteral(
        "QLabel{background:rgba(24,25,29,232);color:#f4f5f8;border:1px solid #2b2d33;"
        "border-radius:4px;padding:2px 6px;font-size:12px;}"));
    m_scaleBadge->hide();
    m_scaleBadgeTimer = new QTimer(this);
    m_scaleBadgeTimer->setSingleShot(true);
    m_scaleBadgeTimer->setInterval(1000);
    connect(m_scaleBadgeTimer, &QTimer::timeout, m_scaleBadge, &QWidget::hide);
    setAttribute(Qt::WA_TranslucentBackground);   // The shadow band is painted, per-pixel alpha.
    setImageGeometry(displayRect);
    toggleTextSelectable(true);   // Text is selectable by default (user requirement).
    // Every markup tool starts on the first palette color, reference behavior.
    for (QColor &c : m_decoToolColor) c = QColor("#ff909c");
    qApp->installEventFilter(this);   // Clicks outside the markup toolbar close it.
    show();
}

void PinWindow::setShadowEnabled(bool enabled)
{
    // Only a repaint: the band stays, so the pin neither moves nor flickers.
    m_shadowOn = enabled;
    update();
}

void PinWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange) update();   // Active pins glow in the theme color.
    QWidget::changeEvent(e);
}

// A blurred rectangle under the image: alpha = peak * F(dx) * F(dy), d = logical pixels
// past the nearest image edge (1 = first pixel outside, <= 0 inside). Measured per pixel.
static double shadowFalloff(int d)
{
    static const double inside[] = { 0.958, 0.917, 0.854, 0.75, 0.604 };              // d = -4 .. 0
    static const double outside[] = { 49, 35, 25, 17, 11, 7, 5, 3, 2, 1, 1, 1 };      // d = 1 .. 12, /107
    if (d <= -5) return 1.0;
    if (d <= 0) return inside[d + 4];
    if (d <= 12) return outside[d - 1] / 107.0;
    return 0.0;
}

QImage PinWindow::shadowImage(const QSize &window, const QColor &color, qreal dpr)
{
    QImage img((QSizeF(window) * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const double peak = 215;   // Solved from white/dark backdrops: alpha 96 at d = 1 (F = 49/107).
    const double m = kShadowMargin;
    const auto dist = [m, dpr](int px, int extent) {
        const double c = (px + 0.5) / dpr, e = extent / dpr;
        return int(std::ceil(qMax(m - c, c - (e - m))));
    };
    const int band = qCeil(m * dpr);
    for (int py = 0; py < img.height(); ++py) {
        const double fy = shadowFalloff(dist(py, img.height()));
        const bool edgeRow = py < band || py >= img.height() - band;
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(py));
        for (int px = 0; px < img.width(); ++px) {
            if (!edgeRow && px == band) px = qMax(band, img.width() - band);   // The image covers the middle.
            if (px >= img.width()) break;
            const int a = qRound(peak * fy * shadowFalloff(dist(px, img.width())));
            if (a > 0) line[px] = qPremultiply(qRgba(color.red(), color.green(), color.blue(), a));
        }
    }
    img.setDevicePixelRatio(dpr);
    return img;
}

void PinWindow::applyProcessedImage()
{
    if (m_originalPix.isNull()) return;
    if (m_scaleBadge) m_scaleBadge->hide();
    QImage img = m_originalPix.toImage().convertToFormat(QImage::Format_ARGB32);

    // 1. Invert colors if requested
    if (m_inverted) img.invertPixels(QImage::InvertRgb);

    // 2. Brightness adjustment (0%..200%, default 100%)
    if (m_brightnessPercent != 100) {
        const double factor = m_brightnessPercent / 100.0;
        uchar *bits = img.bits();
        const qsizetype totalBytes = img.sizeInBytes();
        for (qsizetype i = 0; i < totalBytes; i += 4) {
            bits[i]   = static_cast<uchar>(qBound(0, qRound(bits[i] * factor), 255));     // B
            bits[i+1] = static_cast<uchar>(qBound(0, qRound(bits[i+1] * factor), 255));   // G
            bits[i+2] = static_cast<uchar>(qBound(0, qRound(bits[i+2] * factor), 255));   // R
            // bits[i+3] is Alpha, preserved
        }
    }

    // 3. Rotation and flipping
    QTransform transform;
    if (m_flipH) transform.scale(-1, 1);
    if (m_flipV) transform.scale(1, -1);
    if (m_rotation != 0) transform.rotate(m_rotation);

    if (!transform.isIdentity()) {
        img = img.transformed(transform, Qt::SmoothTransformation);
    }

    m_pix = QPixmap::fromImage(img);
    m_displayPix = QPixmap();

    // Invalidate OCR cache and text markups on image transformation
    ++m_ocrGen;
    if (m_ocrCancel) m_ocrCancel->store(true);
    m_charBoxes.clear();
    m_ocrLines.clear();
    m_selAnchor = -1;
    m_selLead = -1;
    m_decoHighlight.clear();
    m_decoLine.clear();
    if (m_textSelectable && m_pix.height() <= 1440) {
        runOcrForTextSelection();
    }

    // Recalculate window size to preserve logical scale and aspect ratio
    const QPoint c = imageGeometry().center();
    const QSize newSize(qMax(1, qRound(m_pix.width() * m_scale)),
                        qMax(1, qRound(m_pix.height() * m_scale)));
    QRect r(0, 0, newSize.width(), newSize.height());
    r.moveCenter(c);
    setImageGeometry(r);
    if (m_canvas) {
        m_canvas->setGeometry(imageRect());
        m_canvas->setImage(displayImage());
    }
    update();
}

void PinWindow::rotateBy(int degrees)
{
    bakeAnnotations();   // Marks composite into pixels before the transform.
    m_rotation = (m_rotation + degrees) % 360;
    if (m_rotation < 0) m_rotation += 360;
    applyProcessedImage();
}

void PinWindow::flipHorizontal()
{
    bakeAnnotations();
    m_flipH = !m_flipH;
    applyProcessedImage();
}

void PinWindow::flipVertical()
{
    bakeAnnotations();
    m_flipV = !m_flipV;
    applyProcessedImage();
}

void PinWindow::invertColors()
{
    bakeAnnotations();
    m_inverted = !m_inverted;
    applyProcessedImage();
}

void PinWindow::setOpacityPercent(int percent)
{
    m_opacityPercent = qBound(10, percent, 100);
    setWindowOpacity(m_opacityPercent / 100.0);
}

void PinWindow::setBrightnessPercent(int percent)
{
    bakeAnnotations();
    m_brightnessPercent = qBound(0, percent, 200);
    applyProcessedImage();
}

void PinWindow::resetProcessing()
{
    bakeAnnotations();
    m_rotation = 0;
    m_flipH = false;
    m_flipV = false;
    m_inverted = false;
    m_brightnessPercent = 100;
    setOpacityPercent(100);
    applyProcessedImage();
}

void PinWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (m_shadowOn) {
        // Accent while the pin is active, neutral gray otherwise.
        const QColor c = isActiveWindow() ? Bar::accent : QColor(0x80, 0x80, 0x80);
        const qreal dpr = devicePixelRatioF();
        if (m_shadowCache.deviceIndependentSize().toSize() != size()
                || m_shadowCache.devicePixelRatio() != dpr || m_shadowCache.text(QStringLiteral("c")) != c.name()) {
            m_shadowCache = shadowImage(size(), c, dpr);
            m_shadowCache.setText(QStringLiteral("c"), c.name());
        }
        p.drawImage(0, 0, m_shadowCache);
    }
    const QSize is = imageRect().size();
    const qreal dpr = devicePixelRatioF();
    if (m_displayPix.isNull() || m_displayKey != m_pix.cacheKey()
            || m_displaySize != is || m_displayDpr != dpr) {
        m_displayPix = m_pix.scaled(is, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        m_displayKey = m_pix.cacheKey();
        m_displaySize = is;
        m_displayDpr = dpr;
    }
    p.translate(imageRect().topLeft());
    p.drawPixmap(0, 0, m_displayPix);

    // Draw text selection highlight if text is selectable and selected
    if (m_textSelectable && m_selAnchor >= 0 && m_selLead >= 0
            && !m_charBoxes.isEmpty() && !m_ocrImgSize.isEmpty()) {
        const int start = qMin(m_selAnchor, m_selLead);
        const int end = qMax(m_selAnchor, m_selLead);
        const double scaleX = double(is.width()) / m_ocrImgSize.width();
        const double scaleY = double(is.height()) / m_ocrImgSize.height();

        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        QColor selectionFill = Bar::accent;
        selectionFill.setAlpha(80);
        p.setBrush(selectionFill);
        p.setPen(Qt::NoPen);

        for (int i = start; i <= end && i < m_charBoxes.size(); ++i) {
            const QRectF cr = m_charBoxes.at(i).rect;
            const QRectF vr(cr.x() * scaleX, cr.y() * scaleY, cr.width() * scaleX, cr.height() * scaleY);
            p.drawRect(vr);
        }
        p.restore();
    }

    // Markup is drawn after the selection so lines stay readable over the blue highlight.
    if ((!m_decoHighlight.isEmpty() || !m_decoLine.isEmpty())
            && !m_charBoxes.isEmpty() && !m_ocrImgSize.isEmpty()) {
        const double scaleX = double(is.width()) / m_ocrImgSize.width();
        const double scaleY = double(is.height()) / m_ocrImgSize.height();
        const double dpr = devicePixelRatioF();
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        const int n = m_charBoxes.size();
        // Two passes: highlight bands first (multiply), line marks on top.
        // Neighboring characters carrying the same mark merge into one run per
        // OCR line, so an underline stays straight across the range.
        auto runRect = [this, scaleX, scaleY](int a, int b) {
            QRectF run;
            for (int k = a; k <= b; ++k)
                run = run.isNull() ? m_charBoxes.at(k).rect : run.united(m_charBoxes.at(k).rect);
            const QRectF vr(run.x() * scaleX, run.y() * scaleY,
                            run.width() * scaleX, run.height() * scaleY);
            return vr;
        };
        int i = 0;
        while (i < n && !m_decoHighlight.isEmpty()) {
            if (!m_decoHighlight.contains(i)) { ++i; continue; }
            const QColor c = m_decoHighlight.value(i);
            const int line = m_charBoxes.at(i).lineIdx;
            int j = i;
            while (j + 1 < n && m_charBoxes.at(j + 1).lineIdx == line
                   && m_decoHighlight.contains(j + 1) && m_decoHighlight.value(j + 1) == c)
                ++j;
            // A multiply-blended solid band behaves like a real marker.
            p.save();
            p.setCompositionMode(QPainter::CompositionMode_Multiply);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRect(runRect(i, j));
            p.restore();
            i = j + 1;
        }
        i = 0;
        while (i < n && !m_decoLine.isEmpty()) {
            if (!m_decoLine.contains(i)) { ++i; continue; }
            const QPair<int, QColor> m0 = m_decoLine.value(i);
            const int line = m_charBoxes.at(i).lineIdx;
            int j = i;
            while (j + 1 < n && m_charBoxes.at(j + 1).lineIdx == line
                   && m_decoLine.contains(j + 1) && m_decoLine.value(j + 1) == m0)
                ++j;
            const QRectF vr = runRect(i, j);
            const double w = vr.height() * 0.08;   // Reference pen width: 8% of the line height.
            p.setBrush(Qt::NoBrush);
            if (m0.first == DecoStrike) {
                p.setPen(QPen(m0.second, w, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(vr.left(), vr.center().y()), QPointF(vr.right(), vr.center().y()));
            } else if (m0.first == DecoLine) {
                const double y = vr.bottom() - w * 0.5;
                p.setPen(QPen(m0.second, w, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(vr.left(), y), QPointF(vr.right(), y));
            } else if (m0.first == DecoWavy) {
                // Wavy underline: half-period 6*dpr, control points at
                // +-1*dpr, clipped to a tight band around the baseline.
                const double y = vr.bottom() - dpr - w * 0.5;
                const double half = 6.0 * dpr;
                QPainterPath wave(QPointF(vr.left(), y));
                double x = vr.left();
                bool up = true;
                while (x + half <= vr.right()) {
                    wave.quadTo(QPointF(x + half * 0.5, up ? y - dpr : y + dpr),
                                QPointF(x + half, y));
                    x += half;
                    up = !up;
                }
                if (x < vr.right()) wave.lineTo(vr.right(), y);
                p.save();
                p.setClipRect(QRectF(vr.left(), y - dpr - w, vr.width(), 2 * dpr + w));
                p.setPen(QPen(m0.second, w, Qt::SolidLine, Qt::FlatCap));
                p.drawPath(wave);
                p.restore();
            }
            i = j + 1;
        }
        p.restore();
    }
}

// Flyout row for opacity/brightness: slider and percentage only. The row lives
// inside the flyout that opens when hovering the parent item.
// Place a flyout fully outside its parent. Windows menus overlap the parent by
// a few pixels by default, and a same-width flyout then covers the parent row.
QPoint flyoutOutsideParent(const QMenu *subMenu, const QMenu *parent, const QRect &item)
{
    const int w = qMax(subMenu->width(), subMenu->sizeHint().width());
    const int h = subMenu->actions().size() == 1 ? item.height() : subMenu->sizeHint().height();
    const QRect screen = parent->screen()
        ? parent->screen()->availableGeometry()
        : QRect(0, 0, 1920, 1080);
    const bool toLeft = parent->geometry().right() + w > screen.right()
        && parent->geometry().left() - w >= screen.left();
    const int x = toLeft ? parent->geometry().left() - w - 1 : parent->geometry().right() + 1;
    const QPoint rowTop = parent->mapToGlobal(item.topLeft());
    // A one-row slider flyout is exactly as tall as the parent row and shares its top.
    const int y = subMenu->actions().size() == 1 ? rowTop.y()
        : qBound(screen.top(), rowTop.y(), qMax(screen.top(), screen.bottom() - h));
    return QPoint(x, y);
}

void separateFlyoutEdge(QMenu *menu)
{
    auto *owner = qobject_cast<QMenu *>(menu->parentWidget());
    if (!owner) return;
    const QRect item = owner->actionGeometry(menu->menuAction());
    if (!item.isValid()) return;
    if (menu->actions().size() == 1) {
        // The shared menu padding would push a one-row slider against the top edge.
        menu->setStyleSheet(menu->styleSheet() + QStringLiteral("QMenu{padding:0px;border:none;}"));
        menu->setFixedHeight(item.height());
        if (auto *rowAction = qobject_cast<QWidgetAction *>(menu->actions().first())) {
            if (QWidget *row = rowAction->defaultWidget()) {
                row->setFixedHeight(item.height());
                if (QLayout *lay = row->layout()) lay->activate();
            }
        }
    }
    const QPoint next = flyoutOutsideParent(menu, owner, item);
    if (menu->pos() != next) menu->move(next);
}

class MenuEdgeFix : public QObject {
public:
    explicit MenuEdgeFix(QMenu *menu) : QObject(menu), m_menu(menu)
    {
        menu->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if ((event->type() == QEvent::Show || event->type() == QEvent::Resize) && watched == m_menu)
            separateFlyoutEdge(m_menu);
        return false;
    }

private:
    QMenu *m_menu;
};

class MenuSliderRow : public QWidget {
public:
    MenuSliderRow(int minVal, int maxVal, int initialVal,
                  std::function<void(int)> onValueChanged, QWidget *parent = nullptr)
        : QWidget(parent), m_onValueChanged(std::move(onValueChanged))
    {
        setMouseTracking(true);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 0, 12, 0);
        layout->setSpacing(8);

        m_valLabel = new QLabel(QString::number(initialVal) + QStringLiteral("%"), this);
        m_valLabel->setFixedWidth(40);
        m_valLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_valLabel->setStyleSheet(QStringLiteral("color:#f4f5f8;font-size:13px;"));

        m_slider = new QSlider(Qt::Horizontal, this);
        m_slider->setRange(minVal, maxVal);
        m_slider->setValue(initialVal);
        m_slider->setFixedHeight(22);
        m_slider->setStyleSheet(darkSliderQss);

        layout->addWidget(m_slider, 1, Qt::AlignVCenter);
        layout->addWidget(m_valLabel, 0, Qt::AlignVCenter);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

        connect(m_slider, &QSlider::valueChanged, this, [this](int val) {
            m_valLabel->setText(QString::number(val) + QStringLiteral("%"));
            if (m_onValueChanged) m_onValueChanged(val);
        });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *menu = qobject_cast<QMenu *>(watched);
        auto *flyout = qobject_cast<QMenu *>(parentWidget());
        if (!menu || !flyout) return QWidget::eventFilter(watched, event);
        if (event->type() != QEvent::Wheel) return QWidget::eventFilter(watched, event);
        // The highlighted row is the parent entry; once the flyout itself has
        // the pointer, its own row handles the wheel.
        if (menu != flyout && menu->activeAction() != flyout->menuAction())
            return QWidget::eventFilter(watched, event);
        wheelEvent(static_cast<QWheelEvent *>(event));
        return true;
    }
    void wheelEvent(QWheelEvent *e) override
    {
        if (!e->angleDelta().y()) { QWidget::wheelEvent(e); return; }
        // Match QSlider's own wheel handling: scroll lines * single step.
        const int unit = QApplication::wheelScrollLines() * qMax(1, m_slider->singleStep());
        m_slider->setValue(m_slider->value() + (e->angleDelta().y() > 0 ? unit : -unit));
        e->accept();
    }

private:
    QSlider *m_slider = nullptr;
    QLabel *m_valLabel = nullptr;
    std::function<void(int)> m_onValueChanged;
};

static QMenu *createSliderSubMenu(QMenu *parent, const QString &title,
                                  int minVal, int maxVal, int initialVal,
                                  std::function<void(int)> onValueChanged)
{
    auto *subMenu = parent->addMenu(title);
    subMenu->setStyleSheet(parent->styleSheet());
    auto *row = new MenuSliderRow(minVal, maxVal, initialVal, std::move(onValueChanged), subMenu);
    auto *action = new QWidgetAction(subMenu);
    action->setDefaultWidget(row);
    subMenu->addAction(action);
    // The wheel lands on the highlighted menu row, not on the flyout widget.
    parent->installEventFilter(row);
    subMenu->installEventFilter(row);
    new MenuEdgeFix(subMenu);
    return subMenu;
}

void PinWindow::toggleTextSelectable(bool enabled)
{
    if (enabled && m_canvas) {   // Selection works on pixels: marks bake, the canvas steps aside.
        bakeAnnotations();
        m_canvas->armTool(CaptureOverlay::ATool::None);
        m_canvas->hide();
    }
    m_textSelectable = enabled;
    m_selAnchor = -1;
    m_selLead = -1;
    m_selecting = false;
    if (m_textSelectable) {
        m_textOcrError.clear();
        // The I-beam appears only while the pointer is over copyable text;
        // everywhere else the native cursor is kept.
        setCursor(Qt::ArrowCursor);
        // shortcut: tall pins are recognized on first click. Starting a chunked
        // OCR run for every long pin on creation would load the machine for no reason.
        if (m_charBoxes.isEmpty() && !m_ocrRunning && m_pix.height() <= 1440) {
            runOcrForTextSelection();
        }
    } else {
        m_textOcrError.clear();
        if (m_ocrTipShown) { QToolTip::hideText(); m_ocrTipShown = false; }
        setCursor(Qt::SizeAllCursor);
    }
    updateTextStatus();
    updateSelToolbar();
    update();
}

void PinWindow::runOcrForTextSelection()
{
    if (m_pix.isNull() || m_ocrRunning) return;
    m_ocrRunning = true;
    m_textOcrError.clear();
    updateTextStatus();
    m_ocrCancel = std::make_shared<std::atomic_bool>(false);

    const QImage img = m_pix.toImage();
    // Boxes come back in this raw pixel space (recognize resets DPR), not the pixmap's logical size.
    m_ocrImgSize = QSizeF(img.size());
    const auto cancel = m_ocrCancel;
    const int generation = m_ocrGen;
    QTimer::singleShot(300, this, [this, generation] {
        if (!m_ocrRunning || generation != m_ocrGen || !m_textSelectable
            || !m_textOcrError.isEmpty() || !isVisible()) return;
        QToolTip::showText(mapToGlobal(imageRect().topLeft() + QPoint(8, 8)),
                           tr("文字识别中…"), this);
        m_ocrTipShown = true;
    });

    auto watcher = new QFutureWatcher<OcrResult>(this);
    connect(watcher, &QFutureWatcher<OcrResult>::finished, this, [this, watcher, generation] {
        const OcrResult res = watcher->result();
        watcher->deleteLater();
        m_ocrRunning = false;
        if (m_ocrTipShown) { QToolTip::hideText(); m_ocrTipShown = false; }
        if (generation != m_ocrGen) {
            if (m_textSelectable && m_charBoxes.isEmpty()) runOcrForTextSelection();
            updateTextStatus();
            return;
        }
        if (m_ocrCancel && m_ocrCancel->load()) {
            updateTextStatus();
            return;
        }

        if (!res.error.isEmpty()) {
            m_textOcrError = res.error;
            m_textSelectable = false;
            m_ocrLines.clear();
            m_charBoxes.clear();
            m_decoHighlight.clear();
            m_decoLine.clear();
            m_selAnchor = m_selLead = -1;
            setCursor(Qt::SizeAllCursor);
            updateTextStatus();
            updateSelToolbar();
            update();
            return;
        }
        if (res.lines.isEmpty()) {
            m_textSelectable = false;
            m_ocrLines.clear();
            m_charBoxes.clear();
            m_decoHighlight.clear();
            m_decoLine.clear();
            m_selAnchor = m_selLead = -1;
            setCursor(Qt::SizeAllCursor);
            updateTextStatus();
            updateSelToolbar();
            update();
            return;
        }

        m_textOcrError.clear();
        m_ocrLines = res.lines;
        rebuildCharBoxes();
        updateTextStatus();
        // Boxes were rebuilt; the hover handler decides the cursor from now on.
        update();
    });

    watcher->setFuture(QtConcurrent::run([img, cancel]() -> OcrResult {
        // Same guard as the OCR dialog: an escaped exception must not rethrow
        // from watcher->result() on the GUI thread.
        OcrResult res;
        try {
            res = recognizeChunked(img, cancel);
        } catch (const std::exception &error) {
            res.error = PinWindow::tr("识别失败：%1").arg(QString::fromUtf8(error.what()));
        } catch (...) {
            res.error = PinWindow::tr("识别线程异常");
        }
        return res;
    }));
}

void PinWindow::updateTextStatus()
{
    if (!m_textStatus) return;
    QString text;
    if (!m_textOcrError.isEmpty()) text = tr("识别失败，右键重试");
    if (text.isEmpty()) {
        m_textStatus->hide();
        return;
    }
    m_textStatus->setText(text);
    m_textStatus->adjustSize();
    const QRect image = imageRect();
    m_textStatus->move(image.left() + 8, image.top() + 8);
    m_textStatus->show();
    m_textStatus->raise();
}

void PinWindow::rebuildCharBoxes()
{
    m_charBoxes.clear();
    m_decoHighlight.clear();
    m_decoLine.clear();
    // OCR can split one visual row into several segments (a "——" tends to
    // become its own line); cluster segments by vertical overlap so a row
    // keeps a single index and selected text is not cut apart by newlines.
    struct Seg { QRectF box; QString text; };
    QList<Seg> segs;
    for (const OcrLine &line : m_ocrLines) {
        if (!line.text.isEmpty())
            segs.append(Seg{ line.box.boundingRect(), line.text });
    }
    std::sort(segs.begin(), segs.end(), [](const Seg &a, const Seg &b) {
        return a.box.top() != b.box.top() ? a.box.top() < b.box.top()
                                          : a.box.left() < b.box.left();
    });
    struct Row { qreal top = 0, bottom = 0; QList<Seg> segs; };
    QList<Row> rows;
    for (const Seg &s : segs) {
        const qreal cy = s.box.center().y();
        if (!rows.isEmpty() && cy >= rows.last().top && cy <= rows.last().bottom) {
            rows.last().segs.append(s);
            rows.last().bottom = qMax(rows.last().bottom, s.box.bottom());
        } else {
            rows.append(Row{ s.box.top(), s.box.bottom(), QList<Seg>{ s } });
        }
    }
    for (int r = 0; r < rows.size(); ++r) {
        auto &list = rows[r].segs;
        std::sort(list.begin(), list.end(), [](const Seg &a, const Seg &b) {
            return a.box.left() < b.box.left();
        });
        for (const Seg &s : list) {
            const double charWidth = s.box.width() / qMax(1, s.text.size());
            for (int c = 0; c < s.text.size(); ++c) {
                CharBox cb;
                cb.ch = s.text.at(c);
                cb.rect = QRectF(s.box.left() + c * charWidth, s.box.top(), charWidth, s.box.height());
                cb.lineIdx = r;
                m_charBoxes.append(cb);
            }
        }
    }
}

int PinWindow::charIndexAt(const QPoint &pos, bool proximity) const
{
    if (m_pix.isNull() || m_charBoxes.isEmpty() || m_ocrImgSize.isEmpty()) return -1;
    const double scaleX = m_ocrImgSize.width() / imageRect().width();
    const double scaleY = m_ocrImgSize.height() / imageRect().height();
    const QPoint ip = imagePos(pos);
    const QPointF imgPt(ip.x() * scaleX, ip.y() * scaleY);

    // 1. Direct hit with generous view padding (5px in view coordinates)
    const double padX = 5.0 * scaleX;
    const double padY = 5.0 * scaleY;
    for (int i = 0; i < m_charBoxes.size(); ++i) {
        const QRectF &r = m_charBoxes.at(i).rect;
        const QRectF expanded = r.adjusted(-padX, -padY, padX, padY);
        if (expanded.contains(imgPt)) return i;
    }

    // 2. Proximity search: find the closest character within reasonable distance.
    // Skipped for cursor decisions, where only a real hit counts as text.
    if (!proximity) return -1;
    int bestIdx = -1;
    double bestDistSq = std::numeric_limits<double>::max();
    for (int i = 0; i < m_charBoxes.size(); ++i) {
        const QRectF &r = m_charBoxes.at(i).rect;
        const QPointF c = r.center();
        // Give higher weight to vertical distance to prefer characters on the same line
        const double dy = (c.y() - imgPt.y()) * 1.5;
        const double dx = (c.x() - imgPt.x());
        const double distSq = dx * dx + dy * dy;

        const double maxDist = qMax(30.0 * scaleY, r.height() * 1.5);
        if (distSq < (maxDist * maxDist) && distSq < bestDistSq) {
            bestDistSq = distSq;
            bestIdx = i;
        }
    }

    return bestIdx;
}

QString PinWindow::selectedText() const
{
    if (m_selAnchor < 0 || m_selLead < 0 || m_charBoxes.isEmpty()) return {};
    const int start = qMin(m_selAnchor, m_selLead);
    const int end = qMax(m_selAnchor, m_selLead);
    QString out;
    int lastLine = -1;
    for (int i = start; i <= end && i < m_charBoxes.size(); ++i) {
        if (lastLine != -1 && m_charBoxes.at(i).lineIdx != lastLine) {
            out += QLatin1Char('\n');
        }
        out += m_charBoxes.at(i).ch;
        lastLine = m_charBoxes.at(i).lineIdx;
    }
    return out;
}

// Reference pin's default text-highlight palette, in order. Each tool keeps
// its own current color, initialized to the first entry.
static const QColor kDecoColors[] = {
    QColor("#ff909c"), QColor("#b89fff"), QColor("#74b4ff"),
    QColor("#5ad8a6"), QColor("#f6bd16"),
};

void PinWindow::drawTextDeco(int type, const QColor &color)
{
    if (m_selAnchor < 0 || m_selLead < 0 || m_charBoxes.isEmpty()) return;
    const int start = qMax(0, qMin(m_selAnchor, m_selLead));
    const int end = qMin(m_charBoxes.size() - 1, qMax(m_selAnchor, m_selLead));
    for (int i = start; i <= end; ++i) {
        // Highlight keeps its own slot; a line mark lands in the shared line
        // slot and whatever line was there is replaced (reference model).
        if (type == DecoHighlight) m_decoHighlight.insert(i, color);
        else m_decoLine.insert(i, qMakePair(type, color));
    }
    update();
}

void PinWindow::clearTextDeco(int type)
{
    if (m_selAnchor < 0 || m_selLead < 0) return;
    const int start = qMax(0, qMin(m_selAnchor, m_selLead));
    const int end = qMin(m_charBoxes.size() - 1, qMax(m_selAnchor, m_selLead));
    // Works on the intersection with the selection: selecting more clears all
    // covered characters, selecting fewer clears just the selected part.
    // type < 0 clears every mark; otherwise only that tool's mark.
    for (int i = start; i <= end; ++i) {
        if (type == DecoHighlight || type < 0) m_decoHighlight.remove(i);
        if (type != DecoHighlight && (type < 0 || m_decoLine.value(i).first == type))
            m_decoLine.remove(i);
    }
    update();
}

bool PinWindow::selectionUniformDeco(int type, const QColor &color) const
{
    if (m_selAnchor < 0 || m_selLead < 0 || m_charBoxes.isEmpty()) return false;
    const int start = qMax(0, qMin(m_selAnchor, m_selLead));
    const int end = qMin(m_charBoxes.size() - 1, qMax(m_selAnchor, m_selLead));
    // Uniform = every selected character carries this mark in this color; only
    // then does clicking the tool again toggle it off (reference behavior).
    for (int i = start; i <= end; ++i) {
        if (type == DecoHighlight) {
            if (m_decoHighlight.value(i) != color) return false;
        } else if (m_decoLine.value(i) != qMakePair(type, color)) {
            return false;
        }
    }
    return true;
}

void PinWindow::ensureSelToolbar()
{
    if (m_selToolbar) return;
    // Owned by the pin so it stays above the capture; an unparented Tool loses to it.
    m_selToolbar = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                     | Qt::WindowDoesNotAcceptFocus);
    m_selToolbar->setAccessibleName(tr("选字工具栏"));
    m_selToolbar->setObjectName(QStringLiteral("selToolbar"));
    // Scoped to the window: a bare QWidget selector would frame every child,
    // which is where the stray rounded border around the palette came from.
    m_selToolbar->setStyleSheet(QStringLiteral(
        "#selToolbar { background: #18191d; border: 1px solid #2b2d33; border-radius: 8px; }"
        "QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0; }"
        "QToolButton:hover { background-color: #222326; }"
        "QToolButton:pressed { background-color: #2b2d33; }"
    ));
    auto *root = new QVBoxLayout(m_selToolbar);
    root->setContentsMargins(12, 8, 12, 8);
    root->setSpacing(0);
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    root->addLayout(row);

    auto *copyBtn = pinButton(Icon::Copy, tr("复制"));
    connect(copyBtn, &QToolButton::clicked, this, [this] {
        if (!copyTextToClipboard(selectedText()))
            QMessageBox::warning(this, QStringLiteral("LensSnip"), tr("复制失败，剪贴板可能被占用，请重试"));
        // Reference behavior: copying also dismisses the bar and the selection.
        m_selBarOpen = false;
        m_selAnchor = -1;
        m_selLead = -1;
        update();
        updateSelToolbar();
    });
    row->addWidget(copyBtn);

    // A quiet line between the tools and the colors, instead of a frame around them.
    auto *rule = new QFrame(m_selToolbar);
    rule->setFixedHeight(1);
    rule->setStyleSheet(QStringLiteral("background:#2b2d33;border:none;"));
    root->addSpacing(3);
    root->addWidget(rule);
    root->addSpacing(3);

    auto *strip = new QWidget(m_selToolbar);
    auto *stripLay = new QHBoxLayout(strip);
    stripLay->setContentsMargins(0, 0, 0, 0);
    stripLay->setSpacing(8);
    root->addWidget(strip);

    const struct { int type; Icon icon; QString tip; } markups[] = {
        { DecoHighlight, Icon::Marker, tr("荧光笔") },
        { DecoWavy, Icon::WavyLine, tr("波浪线") },
        { DecoLine, Icon::Underline, tr("直线") },
        { DecoStrike, Icon::Strikethrough, tr("删除线") },
    };
    for (int t = 0; t < 4; ++t) {
        const auto &m = markups[t];
        auto *btn = pinButton(m.icon, m.tip);
        btn->setCheckable(true);
        btn->setAutoExclusive(true);
        btn->setChecked(t == m_decoTool);
        row->addWidget(btn);
        // The checked button is the target of the color row and the next selection.
        connect(btn, &QToolButton::clicked, this, [this, t] {
            // Clicking a tool draws on the selection in its remembered color;
            // clicking again when the whole selection already carries that
            // same mark toggles it off (reference behavior).
            m_decoTool = t;
            if (selectionUniformDeco(t, m_decoToolColor[t])) clearTextDeco(t);
            else drawTextDeco(t, m_decoToolColor[t]);
        });
    }
    auto *closeBtn = pinButton(Icon::Cancel, tr("关闭工具栏"));
    connect(closeBtn, &QToolButton::clicked, this, [this] {
        m_selBarOpen = false;   // This selection only; the next finished drag shows it again.
        updateSelToolbar();
    });
    row->addWidget(closeBtn);

    // Palette of the reference's five defaults plus a clear button; clicking a
    // color re-applies the current tool in that color on the selection.
    for (int i = 0; i < int(sizeof kDecoColors / sizeof kDecoColors[0]); ++i) {
        auto *swatch = colorButton(kDecoColors[i], kDecoColors[i].name(), 28);
        stripLay->addWidget(swatch);
        connect(swatch, &QToolButton::clicked, this, [this, i] {
            m_decoToolColor[m_decoTool] = kDecoColors[i];
            // Pure re-apply on the selected characters: a partial selection
            // recolors only that part, never toggling off (reference behavior).
            drawTextDeco(m_decoTool, kDecoColors[i]);
        });
    }
    auto *clearBtn = pinButton(Icon::Eraser, tr("清除标注"));
    stripLay->addWidget(clearBtn);
    connect(clearBtn, &QToolButton::clicked, this, [this] {
        // User-simplified: one tap erases every mark on the selected
        // characters, over- or under-selection included.
        clearTextDeco();
    });
}

void PinWindow::updateSelToolbar()
{
    // Hidden while the drag is still in progress; only a finished selection opens it.
    const bool has = m_selBarOpen && !m_selecting && m_textSelectable
                     && m_selAnchor >= 0 && m_selLead >= 0
                     && !m_charBoxes.isEmpty() && !m_ocrImgSize.isEmpty();
    if (!has) {
        if (m_selToolbar) m_selToolbar->hide();
        return;
    }
    ensureSelToolbar();
    m_selToolbar->adjustSize();
    applyRoundedMask(m_selToolbar);
    const double scaleX = double(imageRect().width()) / m_ocrImgSize.width();
    const double scaleY = double(imageRect().height()) / m_ocrImgSize.height();
    QRectF sel;
    const int from = qMin(m_selAnchor, m_selLead);
    const int to = qMax(m_selAnchor, m_selLead);
    for (int i = from; i <= to && i < m_charBoxes.size(); ++i)
        sel = sel.isNull() ? m_charBoxes.at(i).rect : sel.united(m_charBoxes.at(i).rect);
    const QRectF view = QRectF(sel.x() * scaleX, sel.y() * scaleY, sel.width() * scaleX, sel.height() * scaleY)
                            .translated(imageRect().topLeft());
    QPoint g = mapToGlobal(QPoint(qRound(view.center().x()), qRound(view.bottom()) + 6));
    const QRect avail = screen() ? screen()->availableGeometry()
                                 : QGuiApplication::primaryScreen()->availableGeometry();
    g.setX(qBound(avail.left(), g.x() - m_selToolbar->width() / 2,
                  qMax(avail.left(), avail.right() - m_selToolbar->width() + 1)));
    if (g.y() + m_selToolbar->height() - 1 > avail.bottom())
        g.setY(qMax(avail.top(), mapToGlobal(QPoint(0, qRound(view.top()))).y() - m_selToolbar->height() - 6));
    m_selToolbar->move(g);
    m_selToolbar->show();
    m_selToolbar->raise();
    // A Tool sibling of the pin is covered by it; topmost keeps the toolbar in front.
    const HWND hwnd = reinterpret_cast<HWND>(m_selToolbar->winId());
    if (hwnd) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PinWindow::toggleToolbar(bool enabled)
{
    m_toolbarOn = enabled;
    if (m_toolbarOn) {
        if (!m_toolbar) {
            // Never activates: clicks keep keyboard focus (Esc, Ctrl+Z) on the pin.
            m_toolbar = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                                 | Qt::WindowDoesNotAcceptFocus);
            m_toolbar->setAttribute(Qt::WA_DeleteOnClose, false);
            m_toolbar->setObjectName(QStringLiteral("pinToolbar"));
            m_toolbar->setStyleSheet(QStringLiteral(
                "#pinToolbar { background: #18191d; border: 1px solid #2b2d33; border-radius: 8px; }"
                "QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0; }"
                "QToolButton:hover { background-color: #222326; }"
                "QToolButton:pressed { background-color: #2b2d33; }"
                "QToolButton:checked { background: transparent; }"
                "QToolButton:checked:hover { background-color: #222326; }"
                "QFrame#barSeparator { background-color: #2b2d33; margin: 0px 6px; border: none; }"
            ));
            auto *lay = new QHBoxLayout(m_toolbar);
            lay->setContentsMargins(12, 7, 12, 7);
            lay->setSpacing(4);

            // The full annotation tool set: any tool arms the in-place canvas
            // inside the pin; reclicking disarms it. Options live on the strip
            // that opens under this toolbar; undo/redo follow the canvas stacks.
            const struct { CaptureOverlay::ATool tool; Icon icon; QString tip; } annTools[] = {
                { CaptureOverlay::ATool::Rect, Icon::Rect, tr("矩形") },
                { CaptureOverlay::ATool::Ellipse, Icon::Ellipse, tr("椭圆") },
                { CaptureOverlay::ATool::Arrow, Icon::Arrow, tr("箭头") },
                { CaptureOverlay::ATool::Pen, Icon::Pen, tr("画笔") },
                { CaptureOverlay::ATool::Marker, Icon::Marker, tr("荧光笔") },
                { CaptureOverlay::ATool::Mosaic, Icon::Mosaic, tr("马赛克") },
                { CaptureOverlay::ATool::Text, Icon::Text, tr("文字") },
                { CaptureOverlay::ATool::Number, Icon::Number, tr("序号") },
                { CaptureOverlay::ATool::Eraser, Icon::Eraser, tr("橡皮") },
            };
            for (const auto &t : annTools) {
                auto *btn = pinButton(t.icon, t.tip);
                btn->setCheckable(true);
                btn->setFocusPolicy(Qt::NoFocus);
                const int tool = int(t.tool);
                btn->setProperty("pinTool", tool);
                connect(btn, &QToolButton::clicked, this, [this, btn, tool] {
                    armPinTool(btn->isChecked() ? tool : -1);
                });
                lay->addWidget(btn);
            }
            auto *sep = new QFrame(m_toolbar);
            sep->setObjectName(QStringLiteral("barSeparator"));
            sep->setFixedSize(1, 22);
            lay->addWidget(sep);

            m_undoBtn = pinButton(Icon::Undo, tr("撤销 (Ctrl+Z)"));
            m_undoBtn->setEnabled(false);
            m_undoBtn->setFocusPolicy(Qt::NoFocus);
            connect(m_undoBtn, &QToolButton::clicked, this, [this] {
                if (m_canvas) m_canvas->undoAnnotation();
            });
            lay->addWidget(m_undoBtn);
            m_redoBtn = pinButton(Icon::Redo, tr("重做 (Ctrl+Y)"));
            m_redoBtn->setEnabled(false);
            m_redoBtn->setFocusPolicy(Qt::NoFocus);
            connect(m_redoBtn, &QToolButton::clicked, this, [this] {
                if (m_canvas) m_canvas->redoAnnotation();
            });
            lay->addWidget(m_redoBtn);

            auto *sep2 = new QFrame(m_toolbar);
            sep2->setObjectName(QStringLiteral("barSeparator"));
            sep2->setFixedSize(1, 22);
            lay->addWidget(sep2);

            auto *copyBtn = pinButton(Icon::Copy, tr("复制图片"));
            connect(copyBtn, &QToolButton::clicked, this, [this] {
                if (!copyImageToClipboard(outputPix().toImage()))
                    QMessageBox::warning(this, QStringLiteral("LensSnip"), tr("复制失败，剪贴板可能被占用，请重试"));
            });
            lay->addWidget(copyBtn);

            auto *saveBtn = pinButton(Icon::Save, tr("保存图片"));
            connect(saveBtn, &QToolButton::clicked, this, &PinWindow::saveImage);
            lay->addWidget(saveBtn);

            auto *ocrBtn = pinButton(Icon::Ocr, tr("文字识别 (OCR)"));
            connect(ocrBtn, &QToolButton::clicked, this, [this] {
                showOcr(outputPix().toImage(), this, imageGeometry());
            });
            lay->addWidget(ocrBtn);

            auto *translateBtn = pinButton(Icon::Translate, tr("识别并翻译文字"));
            connect(translateBtn, &QToolButton::clicked, this, &PinWindow::openTranslate);
            lay->addWidget(translateBtn);

            auto *barcodeBtn = pinButton(Icon::Barcode, tr("识别二维码/条形码"));
            connect(barcodeBtn, &QToolButton::clicked, this, &PinWindow::openBarcode);
            lay->addWidget(barcodeBtn);

            m_backBtn = pinButton(Icon::Back, tr("返回长截图"));
            m_backBtn->setFocusPolicy(Qt::NoFocus);
            connect(m_backBtn, &QToolButton::clicked, this, [this] {
                m_canGoBack = false;
                emit editorLeft(true);
                close();
            });
            lay->addWidget(m_backBtn);

            // Done: hides the bar like the first Esc; the pin and its marks stay.
            auto *doneBtn = pinButton(Icon::Check, tr("完成"));
            doneBtn->setFocusPolicy(Qt::NoFocus);
            connect(doneBtn, &QToolButton::clicked, this, [this] { toggleToolbar(false); });
            lay->addWidget(doneBtn);

        }
        m_backBtn->setVisible(m_canGoBack);
        m_toolbar->adjustSize();
        applyRoundedMask(m_toolbar);
        updateToolbarPosition();
        m_toolbar->show();
    } else {
        if (m_canvas) m_canvas->armTool(CaptureOverlay::ATool::None);   // Marks stay; the strip closes.
        if (m_toolbar) m_toolbar->hide();
        if (m_canGoBack) { m_canGoBack = false; emit editorLeft(false); }
    }
}

void PinWindow::openTranslate() { openTextCard(false); }

void PinWindow::openBarcode() { openTextCard(true); }

void PinWindow::openTextCard(bool barcode)
{
    const QRect pin = imageGeometry();
    OcrDialog *card = barcode ? showBarcode(outputPix().toImage(), nullptr, pin)
                              : showTranslate(outputPix().toImage(), nullptr, pin);
    if (!card) return;
    // The card opens at the screen centre; the pin and its bars step aside until it closes.

    QList<QPointer<QWidget>> hidden;
    for (QWidget *w : {static_cast<QWidget *>(this), m_toolbar, m_selToolbar,
                       m_canvas ? m_canvas->optionsStrip() : nullptr})
        if (w && w->isVisible()) { hidden.append(w); w->hide(); }
    connect(this, &QObject::destroyed, card, &QWidget::close);
    connect(card, &QDialog::finished, this, [hidden] {
        for (const QPointer<QWidget> &w : hidden)
            if (w) w->show();
        if (!hidden.isEmpty() && hidden.first()) { hidden.first()->raise(); hidden.first()->activateWindow(); }
    });
}

void PinWindow::updateToolbarPosition()
{
    if (m_toolbar && m_toolbarOn) {
        const QRect geom = imageGeometry();
        const QSize tSize = m_toolbar->sizeHint();
        // Position toolbar centered below the pinned window, clamped to the
        // screen so the widened annotation bar stays fully visible.
        const QScreen *scr = QGuiApplication::screenAt(geom.center());
        if (!scr) scr = QGuiApplication::primaryScreen();
        const QRect avail = scr->availableGeometry();
        const int x = qBound(avail.left(), geom.left() + (geom.width() - tSize.width()) / 2,
                             qMax(avail.left(), avail.right() + 1 - tSize.width()));
        const int y = (std::clamp)(geom.bottom() + Bar::gap, avail.top(),
                                   (std::max)(avail.top(), avail.bottom() + 1 - tSize.height()));
        m_toolbar->move(x, y);
    }
    updateSelToolbar();   // The selection toolbar follows window moves and zoom too.
    positionStrip();
}

void PinWindow::armPinTool(int tool)
{
    if (m_textSelectable && tool >= 0) toggleTextSelectable(false);
    if (!m_canvas) {
        // The shared annotation engine runs as a child canvas: the pin never
        // changes and the desktop stays live; there is no capture session.
        ScreenFrame pinFrame;
        pinFrame.image = displayImage();
        pinFrame.logicalRect = QRect(QPoint(0, 0), imageRect().size());
        pinFrame.pixelRect = QRect(QPoint(0, 0), pinFrame.image.size());
        m_canvas = new CaptureOverlay({pinFrame}, {}, DetectionOptions(), false, true, this);
        connect(m_canvas.data(), &CaptureOverlay::undoAvailability, this, [this](bool u, bool r) {
            if (m_undoBtn) m_undoBtn->setEnabled(u);
            if (m_redoBtn) m_redoBtn->setEnabled(r);
        });
        connect(m_canvas.data(), &CaptureOverlay::stripRebuilt, this, [this] {
            // Tool buttons follow the canvas, which can disarm itself (Esc, right-click).
            auto armed = m_canvas->armedTool();
            if (m_toolbar)
                for (QToolButton *b : m_toolbar->findChildren<QToolButton *>()) {
                    const QVariant t = b->property("pinTool");
                    if (t.isValid()) b->setChecked(t.toInt() == int(armed));
                }
            positionStrip();
        });
    }
    if (!m_canvas->hasAnnotations()) {   // Pixels as displayed; processing/zoom bake before mutating.
        m_canvas->setGeometry(imageRect());
        m_canvas->setImage(displayImage());
    }
    const int lastTool = int(CaptureOverlay::ATool::Eraser);
    const auto atool = tool >= 0 && tool <= lastTool
        ? static_cast<CaptureOverlay::ATool>(tool) : CaptureOverlay::ATool::None;
    m_canvas->armTool(atool);
    m_canvas->show();
    m_canvas->raise();
    positionStrip();
}

void PinWindow::positionStrip()
{
    if (!m_canvas || !m_toolbar) return;
    QWidget *strip = m_canvas->optionsStrip();
    if (!strip || !strip->isVisible()) return;
    strip->adjustSize();
    applyRoundedMask(strip);
    const QRect avail = screen() ? screen()->availableGeometry()
                                 : QGuiApplication::primaryScreen()->availableGeometry();
    // Left-aligned with the pin toolbar, directly below it.
    const int x = qBound(avail.left(), m_toolbar->x(),
                         qMax(avail.left(), avail.right() + 1 - strip->width()));
    const int y = qMin(m_toolbar->y() + m_toolbar->height() + Bar::gap, avail.bottom() + 1 - strip->height());
    strip->move(x, qMax(avail.top(), y));
    strip->raise();
}

void PinWindow::bakeAnnotations()
{
    if (!m_canvas) return;
    m_canvas->commitText();
    if (!m_canvas->hasAnnotations()) return;
    const QImage baked = m_canvas->composited().toImage();
    m_canvas->clearAll();
    // Baked pixels are display-sized and already carry processing.
    m_originalPix = QPixmap::fromImage(baked);
    m_rotation = 0;
    m_flipH = m_flipV = m_inverted = false;
    m_brightnessPercent = 100;
    m_scale = 1.0;
    applyProcessedImage();   // Rebuilds m_pix, clears OCR cache and text markups.
    m_canvas->setGeometry(imageRect());
    m_canvas->setImage(displayImage());
}

QImage PinWindow::displayImage() const
{
    const QSize is = imageRect().size();
    if (m_pix.size() == is) return m_pix.toImage();
    return m_pix.scaled(is, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).toImage();
}

QPixmap PinWindow::annotatedPix() const
{
    return (m_canvas && m_canvas->hasAnnotations()) ? m_canvas->composited() : m_pix;
}

QPixmap PinWindow::outputPix()
{
    if (m_canvas) m_canvas->commitText();
    return annotatedPix();
}

QMenu *PinWindow::createContextMenu()
{
    auto *m = new QMenu(this);
    m->setStyleSheet(darkMenuQss);

    // Section 1
    QString textLabel = tr("文本可选择");
    if (m_textSelectable && m_ocrRunning) textLabel += tr("（识别中…）");
    else if (!m_textOcrError.isEmpty()) textLabel += tr("（识别失败）");
    QAction *textSelect = m->addAction(textLabel);
    if (!m_textOcrError.isEmpty()) textSelect->setToolTip(m_textOcrError);
    textSelect->setCheckable(true);
    textSelect->setChecked(m_textSelectable);
    connect(textSelect, &QAction::toggled, this, [this](bool checked) { toggleTextSelectable(checked); });

    QAction *translate = m->addAction(tr("翻译"));
    connect(translate, &QAction::triggered, this, &PinWindow::openTranslate);

    QAction *barcode = m->addAction(tr("识别二维码/条形码"));
    connect(barcode, &QAction::triggered, this, &PinWindow::openBarcode);

    m->addSeparator();

    // Section 2: Image processing submenu
    QMenu *imgProc = m->addMenu(tr("图像处理"));
    imgProc->setStyleSheet(m->styleSheet());
    new MenuEdgeFix(imgProc);

    QAction *rotLeft = imgProc->addAction(tr("向左旋转"));
    connect(rotLeft, &QAction::triggered, this, [this] { rotateBy(-90); });

    QAction *rotRight = imgProc->addAction(tr("向右旋转"));
    connect(rotRight, &QAction::triggered, this, [this] { rotateBy(90); });

    QAction *flipH = imgProc->addAction(tr("水平翻转"));
    connect(flipH, &QAction::triggered, this, [this] { flipHorizontal(); });

    QAction *flipV = imgProc->addAction(tr("垂直翻转"));
    connect(flipV, &QAction::triggered, this, [this] { flipVertical(); });

    QAction *invert = imgProc->addAction(tr("反色"));
    invert->setCheckable(true);
    invert->setChecked(m_inverted);
    connect(invert, &QAction::toggled, this, [this](bool on) {
        if (on != m_inverted) invertColors();
    });

    imgProc->addSeparator();

    // Slider flyouts: hovering 不透明度/亮度 opens an aligned sub-column with a
    // progress-bar slider (wheel and drag adjust, value shown in percent).
    createSliderSubMenu(imgProc, tr("不透明度"), 10, 100, m_opacityPercent,
                        [this](int val) { setOpacityPercent(val); });
    createSliderSubMenu(imgProc, tr("亮度"), 0, 200, m_brightnessPercent,
                        [this](int val) { setBrightnessPercent(val); });

    imgProc->addSeparator();

    QAction *resetProc = imgProc->addAction(tr("重置所有处理"));
    connect(resetProc, &QAction::triggered, this, [this] { resetProcessing(); });

    QAction *copy = m->addAction(tr("复制图片"));
    connect(copy, &QAction::triggered, this, [this] {
        if (!copyImageToClipboard(outputPix().toImage()))
            QMessageBox::warning(this, QStringLiteral("LensSnip"), tr("复制失败，剪贴板可能被占用，请重试"));
    });

    QAction *save = m->addAction(tr("保存图片"));
    connect(save, &QAction::triggered, this, &PinWindow::saveImage);

    m->addSeparator();

    // Section 3
    QAction *showTools = m->addAction(tr("显示工具栏"));
    showTools->setCheckable(true);
    showTools->setChecked(m_toolbarOn);
    connect(showTools, &QAction::toggled, this, [this](bool checked) { toggleToolbar(checked); });

    QAction *shadow = m->addAction(tr("窗口阴影"));
    shadow->setCheckable(true);
    shadow->setChecked(m_shadowOn);
    connect(shadow, &QAction::toggled, this, [this](bool checked) { setShadowEnabled(checked); });

    return m;
}

void PinWindow::contextMenuEvent(QContextMenuEvent *e)
{
    // With text selected, right-click offers Copy only, like the reference pin.
    if (m_textSelectable && m_selAnchor >= 0 && m_selLead >= 0 && !selectedText().isEmpty()) {
        QMenu m;
        m.setObjectName(QStringLiteral("selCopyMenu"));
        m.setStyleSheet(darkMenuQss);
        QAction *copyAction = m.addAction(tr("复制"));
        if (m.exec(e->globalPos()) == copyAction) {
            if (!copyTextToClipboard(selectedText()))
                QMessageBox::warning(this, QStringLiteral("LensSnip"), tr("复制失败，剪贴板可能被占用，请重试"));
            m_selBarOpen = false;
            m_selAnchor = -1;
            m_selLead = -1;
            update();
            updateSelToolbar();
        }
        return;
    }
    QMenu *m = createContextMenu();
    m->setAttribute(Qt::WA_DeleteOnClose);
    m->exec(e->globalPos());
}

bool PinWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Any press outside the markup toolbar closes it; the press itself keeps
    // going (e.g. starting the next selection drag), reference behavior.
    if (m_selToolbar && m_selToolbar->isVisible() && event->type() == QEvent::MouseButtonPress) {
        const auto *me = static_cast<const QMouseEvent *>(event);
        const QWidget *w = qobject_cast<QWidget *>(watched);
        if (!me->globalPosition().isNull()) {
            const QPoint g = me->globalPosition().toPoint();
            if (!m_selToolbar->geometry().contains(g) && !(w && m_selToolbar->isAncestorOf(w))) {
                m_selBarOpen = false;
                m_selToolbar->hide();
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PinWindow::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        if (m_textSelectable) {
            if (m_charBoxes.isEmpty() && !m_ocrRunning && !m_pix.isNull()
                    && OcrEngine::assetError(QCoreApplication::applicationDirPath()).isEmpty()) {
                // A press before/after a failed OCR run retries instead of failing silently.
                runOcrForTextSelection();
            }
            const int idx = charIndexAt(e->pos());
            if (idx >= 0) {
                m_selAnchor = idx;
                m_selLead = idx;
                m_selecting = true;
                m_selBarOpen = false;   // The toolbar waits until the drag is released.
                setCursor(Qt::IBeamCursor);
                update();
                updateSelToolbar();
                return;
            } else {
                m_selAnchor = -1;
                m_selLead = -1;
                m_selecting = false;
                m_selBarOpen = false;
                update();
                updateSelToolbar();
            }
        }
        m_dragOff = e->globalPosition().toPoint() - pos();
    }
}

void PinWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton) {
        if (m_textSelectable && m_selecting) {
            const int idx = charIndexAt(e->pos());
            if (idx >= 0 && idx != m_selLead) {
                m_selLead = idx;
                update();
            }
            return;
        }
        move(e->globalPosition().toPoint() - m_dragOff);
        updateToolbarPosition();
    } else {
        if (m_textSelectable) {
            // Static I-beam over copyable text only; native cursor elsewhere.
            setCursor(charIndexAt(e->pos(), false) >= 0 ? Qt::IBeamCursor : Qt::ArrowCursor);
        }
    }
}

void PinWindow::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        m_selBarOpen = m_selAnchor >= 0 && m_selLead >= 0;
        updateSelToolbar();
    }
}

void PinWindow::wheelEvent(QWheelEvent *e)       // Zoom with mouse-wheel input.
{
    if (!e->angleDelta().y()) return;
    bakeAnnotations();   // Marks composite into pixels before resampling.
    // Zoom on a 10% grid: one notch moves to the next or previous 10% step.
    const int unit = e->angleDelta().y() > 0
        ? int(std::floor(m_scale * 10.0 + 1e-6)) + 1
        : int(std::ceil(m_scale * 10.0 - 1e-6)) - 1;
    m_scale = qBound(1.0 / qMax(m_pix.width(), m_pix.height()), unit / 10.0, 32.0);
    const QRect before = imageGeometry();
    const QSize ns(qMax(1, qRound(m_pix.width() * m_scale)), qMax(1, qRound(m_pix.height() * m_scale)));
    setImageGeometry(QRect(before.topLeft(), ns));   // Zoom grows from the top-left corner.
    if (m_canvas) {
        m_canvas->setGeometry(imageRect());
        m_canvas->setImage(displayImage());
    }
    updateToolbarPosition();
    if (m_scaleBadge) {
        m_scaleBadge->setText(tr("大小: %1%").arg(qRound(m_scale * 100.0)));
        m_scaleBadge->adjustSize();
        m_scaleBadge->move(imageRect().topLeft() + QPoint(2, 2));
        m_scaleBadge->show();
        m_scaleBadge->raise();
        if (m_scaleBadgeTimer) m_scaleBadgeTimer->start();
    }
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Double-click is the word-select gesture while text selection is armed;
    // only a double-click off any character closes the pin.
    if (m_textSelectable && charIndexAt(e->pos(), false) >= 0) return;
    close();
}

void PinWindow::saveImage()
{
    const QImage image = outputPix().toImage();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    ExportDialog dialog(image, writableImageFormats(), dir,
                        QStringLiteral("Pin_{{yyyy-MM-dd HH:mm:ss}}"), this);
    if (dialog.exec() != QDialog::Accepted) return;
    const ExportRequest request = dialog.request();
    if (!saveImageAtomic(image, request.path,
                         {request.format, request.quality, request.scalePercent}))
        QMessageBox::warning(this, QStringLiteral("LensSnip"),
                             tr("保存失败，请检查路径或磁盘空间后重试"));
}

PinWindow::~PinWindow()
{
    if (m_ocrCancel) m_ocrCancel->store(true);   // Background text recognition stops with the pin.
    if (m_ocrTipShown) QToolTip::hideText();
    if (m_selToolbar) {
        m_selToolbar->close();
        delete m_selToolbar;
        m_selToolbar = nullptr;
    }
    if (m_toolbar) {
        m_toolbar->close();
        delete m_toolbar;
        m_toolbar = nullptr;
    }
}

void PinWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->matches(QKeySequence::Copy)) {
        if (m_textSelectable && m_selAnchor >= 0 && m_selLead >= 0) {
            const QString text = selectedText();
            if (!text.isEmpty()) {
                copyTextToClipboard(text);
                return;
            }
        }
    }
    if (e->key() == Qt::Key_Escape) {
        if (m_toolbarOn) toggleToolbar(false);   // First Esc hides the bar and disarms; marks stay.
        else close();
    }
}
