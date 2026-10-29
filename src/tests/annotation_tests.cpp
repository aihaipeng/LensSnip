#include "capture/capture_overlay.h"
#include <QAbstractItemView>
#include "platform/output.h"
#include "shared/settings.h"
#include "tests/test_support.h"
#include "shared/widgets.h"
#include "shared/dialog_chrome.h"
#include "pin/pin_window.h"
#include "longshot/long_capture.h"
#include "text/cards/ocr_dialog.h"
#include <QGuiApplication>
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QColor>
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QTextCursor>
#include <QInputMethodEvent>
#include <QDialog>
#include <QPushButton>
#include <QTimer>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QSlider>
#include <QBoxLayout>
#include <QDoubleValidator>
#include <QLineEdit>
#include <QFontComboBox>
#include <QButtonGroup>
#include <QFocusEvent>
#include <QLabel>
#include <QFontInfo>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <functional>
#include <qt_windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>

int CaptureOverlay::testMosaic(const QString &previewPath)
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    QImage kernelSource(13, 7, QImage::Format_ARGB32);
    kernelSource.fill(qRgba(20, 30, 40, 77));
    const QImage uniformTiles = mosaicTiles(kernelSource, 4, 7);
    check(uniformTiles.size() == QSize(4, 2) && uniformTiles.pixel(0, 0) == qRgba(20, 30, 40, 77)
              && uniformTiles.pixel(3, 1) == qRgba(20, 30, 40, 77)
              && mosaicTiles(QImage(), 4, 7).isNull() && mosaicTiles(kernelSource, 0, 7).isNull(),
          "mosaic kernel uses ceiling dimensions, clamps edges and preserves averaged alpha");
    for (int y = 0; y < kernelSource.height(); ++y)
        for (int x = 0; x < kernelSource.width(); ++x)
            kernelSource.setPixel(x, y, qRgb(x * 18, y * 36, (x + y) * 12));
    check(mosaicTiles(kernelSource, 4, 7) == mosaicTiles(kernelSource, 4, 7)
              && mosaicTiles(kernelSource, 4, 7) != mosaicTiles(kernelSource, 4, 9),
          "seeded mosaic sampling is repeatable and distinct for different seeds");
    const int multipliers[] = {512,512,456,512,328,456,335,512,405,328,271,456,388,
                               335,292,512,454,405,364,328,298,271,496,456,420};
    const int shifts[] = {9,11,12,13,13,14,14,15,15,15,15,16,16,16,16,17,17,17,
                         17,17,17,17,18,18,18};
    QImage blurSource(7, 5, QImage::Format_ARGB32);
    for (int y = 0; y < blurSource.height(); ++y)
        for (int x = 0; x < blurSource.width(); ++x)
            blurSource.setPixel(x, y, qRgba((x * 91 + y * 31) % 256, (y * 57 + x * 11) % 256,
                                          (x * 41 + y * 23) % 256, 60 + (x + y) * 17));
    for (int radius = 3; radius <= 24; ++radius) {
        QImage expected = blurSource;
        for (bool vertical : {false, true}) {
            const QImage input = expected;
            for (int y = 0; y < input.height(); ++y)
                for (int x = 0; x < input.width(); ++x) {
                    std::array<int, 4> sum{};
                    for (int i = -radius; i <= radius; ++i) {
                        const QRgb color = input.pixel(qBound(0, x + (vertical ? 0 : i), input.width() - 1),
                                                       qBound(0, y + (vertical ? i : 0), input.height() - 1));
                        const int weight = radius + 1 - qAbs(i);
                        sum[0] += qRed(color) * weight; sum[1] += qGreen(color) * weight;
                        sum[2] += qBlue(color) * weight; sum[3] += qAlpha(color) * weight;
                    }
                    expected.setPixel(x, y, qRgba((sum[0] * multipliers[radius]) >> shifts[radius],
                        (sum[1] * multipliers[radius]) >> shifts[radius],
                        (sum[2] * multipliers[radius]) >> shifts[radius],
                        (sum[3] * multipliers[radius]) >> shifts[radius]));
                }
        }
        check(mosaicBlurred(blurSource, radius) == expected,
              "stack blur matches two-pass integer RGBA weights, table normalization and replicated edges");
    }
    check(mosaicBlurred(QImage(), 6).isNull() && mosaicBlurred(blurSource, 25).isNull(),
          "blur rejects missing pixels and unsupported radii");
    for (qreal dpr : {1.0, 1.5, 2.0}) {
        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        QImage image(qRound(600 * dpr), qRound(360 * dpr), QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
                row[x] = qRgb((x * 13 + y * 7) & 255, (x * 3 + y * 11) & 255, (x * 17 + y * 5) & 255);
        }
        CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(), QSize(600, 360)),
                                            QRect(0, 0, image.width(), image.height()), image}});
        const QPoint origin = overlay.mapFromGlobal(screen.topLeft());
        overlay.setAttribute(Qt::WA_DeleteOnClose, false);
        overlay.m_mode = Adjusting;
        overlay.m_sel = QRect(origin + QPoint(40, 40), QSize(500, 280));
        overlay.placeToolBars();
        overlay.showToolBars();
        auto mouse = [&overlay, origin](QEvent::Type type, const QPoint &at) {
            const QPoint pos = origin + at;
            QMouseEvent event(type, pos, overlay.mapToGlobal(pos),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&overlay, &event);
        };
        auto wheel = [&overlay](QWidget *target, const QPoint &delta) {
            QWheelEvent event(QPointF(10, 10), QPointF(target->mapToGlobal(QPoint(10, 10))),
                              QPoint(), delta, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(target, &event);
        };
        auto mode = [&overlay, check](bool brush) {
            auto *button = overlay.m_toolOptions->findChild<QToolButton *>(brush
                ? QStringLiteral("mosaicBrush") : QStringLiteral("mosaicRectangle"));
            check(button && !button->toolTip().isEmpty(), "mosaic modes have named icon buttons");
            if (overlay.mosaicBrush() != brush) button->click();
            check(!overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("mosaicEllipse")),
                  "mosaic exposes no ellipse-region button");
        };
        const QImage original = overlay.cropped().toImage();
        overlay.m_toolBtn[int(ATool::Mosaic)]->click();
        check(overlay.mosaicBrush() && overlay.toolSize() == 15 && overlay.mosaicStrength() == 6,
              "mosaic starts as a brush with width 15 and strength 6");
        check(!overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("mosaicFit")),
              "the mosaic strip no longer exposes the text-fitting mode");
        const auto *mosaicStrip = static_cast<QBoxLayout *>(overlay.m_toolOptions->layout());
        int mosaicBrushAt = -1, mosaicRectAt = -1;
        for (int i = 0; i < mosaicStrip->count(); ++i) {
            const QString name = mosaicStrip->itemAt(i)->widget()->objectName();
            if (name == QStringLiteral("mosaicBrush")) mosaicBrushAt = i;
            if (name == QStringLiteral("mosaicRectangle")) mosaicRectAt = i;
        }
        check(mosaicBrushAt >= 0 && mosaicRectAt >= 0 && mosaicBrushAt < mosaicRectAt,
              "the mosaic strip puts the brush before the rectangle");
        auto *brushButton = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("mosaicBrush"));
        check(brushButton && brushButton->isChecked()
                  && brushButton->icon().pixmap(QSize(16, 16)).toImage()
                     == barIcon(Icon::Pen).pixmap(QSize(16, 16)).toImage(),
              "mosaic brush has an explicit pencil icon and selected state");
        brushButton->click();
        check(overlay.mosaicBrush(), "clicking the active brush keeps brush mode");
        if (!previewPath.isEmpty() && dpr == 1)
            check(overlay.grab(overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry()))
                      .save(previewPath + QStringLiteral(".brush-toolbar.png")), "save explicit brush-mode toolbar preview");
        check(!overlay.m_sizeInput && overlay.m_sizeButton
                  && overlay.m_toolOptions->findChildren<QLineEdit *>().isEmpty()
                  && overlay.m_toolOptions->findChildren<QComboBox *>().isEmpty(),
              "mosaic brush width is the shared width button, not a text input or dropdown");
        QKeyEvent typeWidth(QEvent::KeyPress, Qt::Key_9, Qt::NoModifier, QStringLiteral("9"));
        QApplication::sendEvent(overlay.m_sizeButton, &typeWidth);
        check(overlay.toolSize() == 15 && overlay.m_sizeButton->text() == QStringLiteral("15"),
              "typing cannot edit the brush-width counter");
        const QPoint cursorPoint = origin + QPoint(280, 200);
        overlay.updateAnnotationCursor(cursorPoint);
        const QImage initialCursor = overlay.cursor().pixmap().toImage();
        if (!previewPath.isEmpty() && dpr == 1)
            check(initialCursor.save(previewPath + QStringLiteral(".cursor.png")), "save circular brush cursor preview");
        const QColor cursorCenter = initialCursor.pixelColor(initialCursor.width() / 2, initialCursor.height() / 2);
        check(!initialCursor.isNull() && initialCursor.pixelColor(0, 0).alpha() == 0
                  && cursorCenter.red() < 160 && cursorCenter.red() == cursorCenter.green()
                  && cursorCenter.green() == cursorCenter.blue()
                  && cursorCenter.alpha() > 0 && cursorCenter.alpha() < 160,
              "brush cursor is a dark soft blurred disc with transparent corners");
        const auto scrollBrush = [&](int delta) {
            QWheelEvent event(cursorPoint, overlay.mapToGlobal(cursorPoint), QPoint(), QPoint(0, delta),
                              Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(&overlay, &event);
        };
        scrollBrush(120); scrollBrush(120);
        check(overlay.toolSize() == 17 && overlay.m_sizeButton->text() == QStringLiteral("17")
                  && overlay.cursor().pixmap().width() > initialCursor.width() && overlay.mosaicStrength() == 6,
              "canvas wheel grows the circular cursor and its read-only width count, not strength");
        scrollBrush(-120); scrollBrush(-120);
        check(overlay.cursor().pixmap().toImage() == initialCursor && overlay.toolSize() == 15,
              "scrolling back restores the original brush cursor and count");
        for (int width : {4, 14, 50}) {
            const QPixmap cursor = mosaicBrushCursor(width, dpr).pixmap();
            check(cursor.width() == qRound((qCeil(width) | 1) * dpr)
                      && cursor.devicePixelRatio() == dpr,
                  "circle cursor scales its native pixels with DPR while keeping logical brush size");
            const QImage dot = cursor.toImage();
            int maximumAlpha = 0;
            QColor rim;
            for (int y = 0; y < dot.height(); ++y)
                for (int x = 0; x < dot.width(); ++x) {
                    const QColor color = dot.pixelColor(x, y);
                    if (color.alpha() > maximumAlpha) {
                        maximumAlpha = color.alpha();
                        rim = color;
                    }
                }
            check(maximumAlpha >= 80 && maximumAlpha < 200
                      && rim.red() < 220 && rim.green() < 220 && rim.blue() < 220,
                  "soft brush cursors keep a muted grey boundary without a bright rim");
        }
        mode(false);
        check(overlay.m_toolOptions->isVisible() && overlay.m_mosaicSlider && !overlay.mosaicBrush()
                  && overlay.m_mosaicSlider->minimum() == 3 && overlay.m_mosaicSlider->maximum() == 24,
              "mosaic opens rectangle and brush modes with a 3-24 strength slider");
        wheel(&overlay, QPoint(0, 120));
        check(overlay.mosaicStrength() == 7 && overlay.m_mosaicSlider->value() == 7
                  && !overlay.m_sizeInput && overlay.m_mosaicStrengthLabel->text().toInt() == 7
                  && overlay.m_mosaicWidth == 15,
              "rectangle wheel changes strength, slider and number without changing brush width");
        wheel(overlay.m_mosaicStrengthLabel, QPoint(0, -120));
        check(overlay.mosaicStrength() == 6 && overlay.m_mosaicSlider->value() == 6,
              "rectangle strength-label wheel shares strength synchronization");
        overlay.setToolSize(99);
        wheel(&overlay, QPoint(0, 120));
        check(overlay.mosaicStrength() == 24 && overlay.m_mosaicStrengthLabel->text().toInt() == 24,
              "rectangle strength stays at its upper limit");
        overlay.setToolSize(0);
        wheel(&overlay, QPoint(0, -120));
        check(overlay.mosaicStrength() == 3 && overlay.m_mosaicSlider->value() == 3,
              "rectangle strength stays at its lower limit");
        QSlider *slider = overlay.m_mosaicSlider;
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
            const QPoint point(type == QEvent::MouseButtonPress ? 5 : 47, slider->height() / 2);
            QMouseEvent drag(type, point, slider->mapToGlobal(point),
                             type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                             type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(slider, &drag);
        }
        check(overlay.mosaicStrength() > 3 && overlay.mosaicStrength() == slider->value()
                  && overlay.m_mosaicStrengthLabel->text().toInt() == slider->value(),
              "mouse dragging the actual slider updates strength and its numeric control");
        overlay.m_mosaicSlider->setValue(10);
        mouse(QEvent::MouseButtonPress, QPoint(80, 80));
        mouse(QEvent::MouseMove, QPoint(200, 160));
        const QRect liveView = overlay.m_ann.rect.adjusted(-8, -8, 8, 8);
        const QImage liveBorder = overlay.grab(liveView).toImage();
        if (!previewPath.isEmpty() && dpr == 1)
            check(liveBorder.save(previewPath + QStringLiteral(".live-border.png")), "save live mosaic border preview");
        mouse(QEvent::MouseButtonRelease, QPoint(200, 160));
        check(overlay.m_anns.size() == 1 && !overlay.m_anns.first().mosaicBrush
                  && overlay.m_anns.first().mosaicStrength == 10 && overlay.cropped().toImage() != original,
              "rectangle mosaic commits its independent strength and changes output pixels");
        const int liveSelection = overlay.m_selAnn;
        overlay.m_selAnn = -1;
        const QImage inactiveBorder = overlay.grab(liveView).toImage();
        overlay.m_selAnn = liveSelection;
        check(liveBorder != inactiveBorder && overlay.grab(liveView).toImage() != inactiveBorder,
              "both drawing and selected states show mosaic boundaries; inactive regions show none");
        const QImage rectangle = overlay.cropped().toImage();
        const QRect nativeRectangle(qRound(40 * dpr), qRound(40 * dpr), qCeil(121 * dpr), qCeil(81 * dpr));
        for (int y = 0; y < rectangle.height(); ++y)
            for (int x = 0; x < rectangle.width(); ++x)
                if (!nativeRectangle.contains(x, y))
                    check(rectangle.pixel(x, y) == original.pixel(x, y),
                          "rectangle mosaic leaves pixels outside its selected area untouched");
        const int undoBefore = overlay.m_undoStack.size();
        overlay.m_mosaicSlider->setSliderDown(true);
        overlay.m_mosaicSlider->setValue(11);
        const QImage firstDrag = overlay.cropped().toImage();
        overlay.m_mosaicSlider->setValue(14);
        overlay.m_mosaicSlider->setSliderDown(false);
        check(overlay.m_undoStack.size() == undoBefore + 1 && overlay.m_anns.first().mosaicStrength == 14
                  && overlay.cropped().toImage() != firstDrag && overlay.toolSize() == 14,
              "slider dragging previews strength immediately and creates one undo step");
        overlay.undoAnnotation();
        check(overlay.m_anns.first().mosaicStrength == 10, "undo restores the strength before slider dragging");
        overlay.redoAnnotation();
        check(overlay.m_anns.first().mosaicStrength == 14, "redo restores the final dragged strength");
        mode(true);
        check(overlay.toolSize() == 15, "rectangle strength changes preserve the default brush width");
        overlay.setToolSize(15);
        check(overlay.mosaicBrush() && overlay.toolSize() == 15 && overlay.m_mosaicSlider->value() == 14
                  && overlay.m_sizeButton->text().toInt() == 15
                  && overlay.m_mosaicStrengthLabel->text().toInt() == 14
                  && overlay.m_mosaicStrengthLabel->x() > overlay.m_mosaicSlider->x(),
              "brush mode keeps separate remembered width and strength");
        wheel(&overlay, QPoint(0, 120));
        wheel(overlay.m_sizeButton, QPoint(0, 120));
        wheel(overlay.m_mosaicSlider, QPoint(0, 120));
        check(overlay.toolSize() == 17 && overlay.m_sizeButton->text().toInt() == 17
                  && overlay.m_mosaicSlider->value() == 15 && overlay.m_mosaicStrengthLabel->text().toInt() == 15,
              "canvas and width-field wheels change width; strength-slider wheel changes only strength");
        wheel(overlay.m_mosaicStrengthLabel, QPoint(0, -120));
        check(overlay.toolSize() == 17 && overlay.m_sizeButton->text().toInt() == 17
                  && overlay.m_mosaicStrengthLabel->text().toInt() == 14,
              "strength-label wheel does not overwrite the brush-width number");
        wheel(overlay.m_sizeButton, QPoint(120, 0));
        wheel(overlay.m_mosaicSlider, QPoint(120, 0));
        check(overlay.toolSize() == 17 && overlay.mosaicStrength() == 14,
              "horizontal wheel input changes neither mosaic parameter");
        overlay.m_mosaicSlider->setValue(8);
        check(overlay.m_anns.first().mosaicStrength == 14 && overlay.toolSize() == 17
                  && overlay.m_sizeButton->text().toInt() == 17
                  && overlay.m_mosaicStrengthLabel->text().toInt() == 8,
              "switching modes and tuning a new brush leaves the older rectangle unchanged");
        const QImage beforeBrush = overlay.cropped().toImage();
        mouse(QEvent::MouseButtonPress, QPoint(280, 100));
        wheel(overlay.m_sizeButton, QPoint(0, 120));
        overlay.m_mosaicSlider->setValue(9);
        mouse(QEvent::MouseMove, QPoint(360, 190));
        mouse(QEvent::MouseMove, QPoint(460, 100));
        check(overlay.m_live && overlay.m_ann.pen == 18 && overlay.m_ann.mosaicStrength == 9
                  && overlay.m_tools->isVisible() && overlay.m_toolOptions->isVisible(),
              "live brush width and strength update while both bars remain visible");
        mouse(QEvent::MouseButtonRelease, QPoint(460, 100));
        const Annotation brush = overlay.m_anns.last();
        const QImage painted = overlay.cropped().toImage();
        check(overlay.m_anns.size() == 2 && brush.mosaicBrush && brush.stroke.size() == 3
                  && overlay.m_selAnn == -1 && overlay.annotationAt(origin + QPoint(360, 190)) == -1
                  && painted != beforeBrush && overlay.annAnchors(brush).isEmpty()
                  && annotationHit(brush, origin + QPoint(360, 190))
                  && !annotationHit(brush, origin + QPoint(370, 100)),
              "brush mosaic follows a round polyline rather than its bounding rectangle");
        int changed = 0;
        for (int y = 0; y < painted.height(); ++y)
            for (int x = 0; x < painted.width(); ++x) {
                if (painted.pixel(x, y) == beforeBrush.pixel(x, y)) continue;
                ++changed;
                const QPointF point = QPointF(overlay.m_sel.topLeft()) + QPointF((x + 0.5) / dpr, (y + 0.5) / dpr);
                check(distToSegment(point, brush.stroke[0], brush.stroke[1]) <= brush.pen / 2.0 + 1
                          || distToSegment(point, brush.stroke[1], brush.stroke[2]) <= brush.pen / 2.0 + 1,
                      "brush pixels outside the stroked mask stay untouched");
            }
        check(changed > 0, "brush mask contains pixelated pixels");
        const QRect brushPreview = annotationRect(brush).adjusted(-30, -30, 30, 30);
        overlay.m_selAnn = 1;
        const QImage selectedBrush = overlay.grab(brushPreview).toImage();
        overlay.m_selAnn = -1;
        check(overlay.grab(brushPreview).toImage() == selectedBrush
                  && overlay.annHandleAt(brush, QPoint(annotationRect(brush).center().x(), annotationRect(brush).bottom() + 22)) == H_None,
              "selected mosaic brushes show no dashed bounds or invisible rotation handles");
        overlay.m_selAnn = 0;
        const QRect rectanglePreview = annotationRect(overlay.m_anns.first()).adjusted(-30, -30, 30, 30);
        const QImage selectedRectangle = overlay.grab(rectanglePreview).toImage();
        overlay.m_selAnn = -1;
        check(overlay.grab(rectanglePreview).toImage() != selectedRectangle,
              "a selected rectangle mosaic keeps its controls with the brush armed");
        overlay.m_atool = ATool::None;
        overlay.m_selAnn = 0;
        const QImage editingRectangle = overlay.grab(rectanglePreview).toImage();
        overlay.m_selAnn = -1;
        check(overlay.grab(rectanglePreview).toImage() != editingRectangle,
              "rectangle mosaic selection controls remain available during explicit editing");
        overlay.m_atool = ATool::Mosaic;
        overlay.m_selAnn = -1;
        overlay.undoAnnotation();
        check(overlay.cropped().toImage() == beforeBrush, "one undo removes the entire brush stroke");
        overlay.redoAnnotation();
        check(overlay.cropped().toImage() == painted, "redo restores identical brush output pixels");
        mode(false);
        check(overlay.toolSize() == 9 && overlay.m_mosaicSlider->value() == 9,
              "switching back to rectangle shows the remembered strength instead of brush thickness");
        mode(true);
        check(overlay.toolSize() == 18, "switching back to brush restores its thickness");
        overlay.setToolSize(99);
        check(overlay.toolSize() == 50 && overlay.m_mosaicSlider->value() == 9,
              "brush width has its own upper bound without changing strength");
        overlay.setToolSize(0);
        check(overlay.toolSize() == 4 && overlay.m_mosaicSlider->value() == 9,
              "brush width stays at its independent minimum without changing strength");
        overlay.setToolSize(18);
        wheel(&overlay, QPoint(0, -120));
        check(overlay.toolSize() == 17 && overlay.m_sizeButton->text().toInt() == 17,
              "downward brush wheel reduces width and updates its number");
        wheel(&overlay, QPoint(0, 120));
        overlay.m_mosaicSlider->setValue(12);
        check(overlay.m_anns[1].pen == 18 && overlay.m_anns[1].mosaicStrength == 9
                  && overlay.cropped().toImage() == painted,
              "completed brushes are not selected or modified by subsequent parameter changes");
        const QImage beforeDot = overlay.cropped().toImage();
        mouse(QEvent::MouseButtonPress, QPoint(300, 250));
        mouse(QEvent::MouseButtonRelease, QPoint(300, 250));
        check(overlay.m_anns.size() == 3 && overlay.m_anns.last().stroke.size() == 1
                  && overlay.cropped().toImage() != beforeDot,
              "single-click brush strokes pixelate a round dab");
        mouse(QEvent::MouseButtonPress, QPoint(520, 280));
        mouse(QEvent::MouseMove, QPoint(580, 340));
        mouse(QEvent::MouseButtonRelease, QPoint(580, 340));
        check(overlay.cropped().size() == original.size(), "brushes crossing the selection do not expand output");
        const int oldWidth = overlay.m_mosaicWidth;
        for (int blurPass = 0; blurPass < 2; ++blurPass) {
            QImage glyph(24, 24, QImage::Format_RGB32);    // As seen on the bar background.
            glyph.fill(Bar::surface);
            QPainter onBar(&glyph);
            onBar.drawPixmap(0, 0, overlay.m_mosaicPreview->icon().pixmap(24, 24));
            onBar.end();
            int darkest = 0;   // Brightest pixel: the glyph must stand out from the dark bar.
            for (int y = 0; y < glyph.height(); ++y)
                for (int x = 0; x < glyph.width(); ++x) darkest = qMax(darkest, qGray(glyph.pixel(x, y)));
            check(darkest >= 0x80, "effect preview glyph keeps >=3:1 contrast on the dark bar");
            overlay.m_mosaicPreview->click();
        }
        check(!overlay.mosaicBlur() && overlay.mosaicStrength() == 12 && overlay.toolSize() == oldWidth,
              "effect preview cycles pixelate and blur without changing width");
        overlay.m_mosaicPreview->click();
        overlay.m_mosaicSlider->setValue(11);
        check(overlay.mosaicBlur() && overlay.mosaicStrength() == 11 && overlay.toolSize() == oldWidth,
              "switching to blur takes its own strength independently of width");
        overlay.m_mosaicPreview->click();
        check(!overlay.mosaicBlur() && overlay.mosaicStrength() == 12,
              "the effect preview returns to the remembered pixelate strength");
        overlay.m_anns.clear();
        overlay.m_selAnn = -1;
        const QImage empty = overlay.cropped().toImage();
        mode(false);
        mouse(QEvent::MouseButtonPress, QPoint(80, 80));
        mouse(QEvent::MouseMove, QPoint(200, 160));
        mouse(QEvent::MouseButtonRelease, QPoint(200, 160));
        const QImage regionOutput = overlay.cropped().toImage();
        check(overlay.m_anns.size() == 1 && !overlay.m_anns[0].mosaicBrush && regionOutput != empty
                   && regionOutput.pixel(0, 0) == empty.pixel(0, 0)
                   && annotationHit(overlay.m_anns[0], origin + QPoint(140, 120)),
               "rectangle regions redact their interior while preserving pixels outside the mask");
        overlay.m_mosaicPreview->click();
        const QImage blurred = overlay.cropped().toImage();
        check(overlay.m_anns[0].mosaicBlur && blurred != regionOutput,
               "selected rectangle switches effect and renders blur through the same mask");
        overlay.undoAnnotation();
        check(!overlay.m_anns[0].mosaicBlur && overlay.cropped().toImage() == regionOutput,
              "undo restores the selected region's effect and exact output");
        overlay.redoAnnotation();
        check(overlay.m_anns[0].mosaicBlur && overlay.cropped().toImage() == blurred,
              "redo restores blur output");
        overlay.selectMosaicMode(false);
        overlay.m_anns.clear();
        Annotation ink;
        ink.tool = ATool::Pen; ink.color = Qt::red; ink.pen = 20;
        ink.stroke << origin + QPoint(110, 120) << origin + QPoint(170, 120);
        overlay.m_anns.append(ink);
        mouse(QEvent::MouseButtonPress, QPoint(80, 80));
        mouse(QEvent::MouseMove, QPoint(200, 160));
        mouse(QEvent::MouseButtonRelease, QPoint(200, 160));
        overlay.cropped();
        const QPoint sample = (origin + QPoint(140, 120)) * dpr;
        check(overlay.m_anns[1].mosaicSource.pixelColor(sample) == QColor(Qt::red),
              "mosaic samples lower annotations instead of only the frozen desktop");
        const QImage redacted = overlay.m_anns[1].mosaicCache;
        ink.color = Qt::green;
        overlay.m_anns.append(ink);
        overlay.cropped();
        check(overlay.m_anns[1].mosaicCache == redacted
                  && overlay.m_anns[1].mosaicSource.pixelColor(sample) == QColor(Qt::red),
              "upper annotations never leak into the mosaic's sampled source");
        overlay.m_anns[0].color = Qt::blue;
        overlay.cropped();
        check(overlay.m_anns[1].mosaicSource.pixelColor(sample) == QColor(Qt::blue)
                  && overlay.m_anns[1].mosaicCache != redacted,
              "editing lower annotations invalidates the effect cache");
        overlay.m_anns.removeLast();
        check(overlay.annAnchors(overlay.m_anns[1]).size() == 8,
              "mosaic regions retain resize handles but expose no rotation handle");
        const QRect beforeMove = overlay.m_anns[1].rect;
        const int countBeforeMove = overlay.m_anns.size();
        const QPoint startMove = beforeMove.center() - origin;
        mouse(QEvent::MouseButtonPress, startMove);
        mouse(QEvent::MouseMove, startMove + QPoint(22, 13));
        mouse(QEvent::MouseButtonRelease, startMove + QPoint(22, 13));
        check(overlay.m_anns.size() == countBeforeMove && overlay.m_selAnn == 1
                  && overlay.m_anns[1].rect == beforeMove.translated(22, 13) && !overlay.m_live,
              "clicking a region with the mosaic tool still active moves it instead of drawing a new object");
        for (Handle handle : {H_L, H_R, H_T, H_B}) {
            const QRect beforeResize = overlay.m_anns[1].rect;
            const QPoint edge = handle == H_L ? QPoint(beforeResize.left(), beforeResize.center().y())
                : handle == H_R ? QPoint(beforeResize.right(), beforeResize.center().y())
                : handle == H_T ? QPoint(beforeResize.center().x(), beforeResize.top())
                : QPoint(beforeResize.center().x(), beforeResize.bottom());
            const QPoint delta = handle == H_L ? QPoint(-12, 0) : handle == H_R ? QPoint(12, 0)
                : handle == H_T ? QPoint(0, -12) : QPoint(0, 12);
            mouse(QEvent::MouseButtonPress, edge - origin);
            check(overlay.m_itemDrag == handle, "each mosaic boundary is a resize target");
            mouse(QEvent::MouseMove, edge - origin + delta);
            mouse(QEvent::MouseButtonRelease, edge - origin + delta);
            check(overlay.m_anns.size() == countBeforeMove && overlay.m_anns[1].rect != beforeResize,
                  "dragging a side stretches the existing mosaic without creating another object");
        }
        const QImage beforeDeselect = overlay.cropped().toImage();
        const QRect view = overlay.m_anns[1].rect.adjusted(-8, -8, 8, 8);
        const QImage editingBorder = overlay.grab(view).toImage();
        if (!previewPath.isEmpty() && dpr == 1)
            check(editingBorder.save(previewPath + QStringLiteral(".editing-border.png")), "save editable mosaic border preview");
        mouse(QEvent::MouseButtonPress, QPoint(380, 230));
        mouse(QEvent::MouseButtonRelease, QPoint(380, 230));
        check(overlay.m_selAnn == -1 && overlay.m_itemDrag == H_None && overlay.m_anns.size() == countBeforeMove
                  && overlay.cropped().toImage() == beforeDeselect && overlay.grab(view).toImage() != editingBorder,
              "clicking outside ends region editing and removes the border without changing captured pixels");
        if (!previewPath.isEmpty() && dpr == 1)
            check(overlay.grab(view).save(previewPath + QStringLiteral(".locked.png")), "save inactive mosaic region preview");
        const QRect lockedRect = overlay.m_anns[1].rect;
        mouse(QEvent::MouseMove, QPoint(400, 250));
        check(overlay.m_anns[1].rect == lockedRect, "an unselected mosaic no longer follows mouse movement");
        mouse(QEvent::MouseButtonPress, lockedRect.center() - origin);
        mouse(QEvent::MouseButtonRelease, lockedRect.center() - origin);
        check(overlay.m_selAnn == -1 && overlay.m_anns[1].rect == lockedRect && overlay.m_anns.size() == countBeforeMove,
              "a deselected region is locked: clicking it again does not reopen or move it");
        if (!previewPath.isEmpty()) {
            check(overlay.cropped().save(previewPath + QString(".%1.png").arg(dpr)), "save mosaic pixel preview");
            if (dpr == 1)
                check(overlay.grab(overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry()))
                          .save(previewPath + QStringLiteral(".toolbar.png")), "save mosaic toolbar preview");
        }
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("mosaic.png"));
        const QImage output = overlay.cropped().toImage();
        overlay.pushUndo();
        check(overlay.m_undoStack.last()[1].mosaicCache.isNull()
                  && overlay.m_undoStack.last()[1].mosaicSource.isNull(),
              "undo snapshots retain semantic annotations without desktop-sized effect caches");
        overlay.m_anns[0].color = Qt::green;
        const QImage changedLayers = overlay.cropped().toImage();
        overlay.undoAnnotation();
        check(overlay.cropped().toImage() == output, "undo rebuilds effect caches with identical pixels");
        check(overlay.m_redoStack.last()[1].mosaicCache.isNull()
                  && overlay.m_redoStack.last()[1].mosaicSource.isNull(),
              "redo snapshots also discard rebuildable effect caches");
        overlay.redoAnnotation();
        check(overlay.cropped().toImage() == changedLayers, "redo rebuilds the changed lower-layer effect");
        overlay.undoAnnotation();
        check(overlay.saveCaptureTo(path) && QImage(path) == output,
              "saved mosaic output retains all pixels and source resolution");
    }
    std::puts("PASS: rectangle/brush mosaic, independent strength/width, slider/wheel sync, undo, mask and output at DPR 1/1.5/2");
    return 0;
}

int CaptureOverlay::testTextAnnotation()
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    QImage image(1350, 750, QImage::Format_RGB32);
    image.fill(QColor(30, 40, 50));
    CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(), QSize(900, 500)),
                                        QRect(0, 0, 1350, 750), image}});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    const QPoint origin = screen.topLeft() - overlay.geometry().topLeft() + QPoint(60, 60);
    overlay.m_mode = Adjusting;
    overlay.m_sel = QRect(origin, QSize(800, 300));
    overlay.placeToolBars();
    overlay.showToolBars();
    overlay.selectTool(ATool::Text);
    QLineEdit *fontEdit = overlay.m_sizeInput->findChild<QLineEdit *>();
    fontEdit->setText(QStringLiteral("10.5"));
    QKeyEvent fontEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(fontEdit, &fontEnter);
    check(overlay.m_afont == 10.5 && overlay.m_sizeInput->currentText().toDouble() == 10.5
              && static_cast<const QDoubleValidator *>(overlay.m_sizeInput->validator())->top() == 1638
              && overlay.m_sizeInput->findText(QStringLiteral("12")) >= 0 && overlay.isVisible(),
          "the editable font control accepts fractional point sizes without pinning the capture");
    QComboBox *fontBeforeEdit = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("annotationFont"));
    if (fontBeforeEdit) fontBeforeEdit->setProperty("reuseProbe", true);
    overlay.beginTextEdit(origin + QPoint(20, 20));
    QApplication::processEvents();
    QComboBox *fontAfterEdit = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("annotationFont"));
    check(fontBeforeEdit && fontAfterEdit && fontAfterEdit->property("reuseProbe").toBool(),
          "re-entering text reuses the existing option strip instead of rebuilding it");
    {   // Editing shows only controls: no outer frame or native placeholder dashes.
        overlay.hideToolBars();
        QImage edit(overlay.size(), QImage::Format_ARGB32);
        overlay.render(&edit);
        overlay.showToolBars();
        const QColor bg(30, 40, 50);
        const QRect box = overlay.m_editor->mapRectToScene(overlay.m_editor->boundingRect())
                              .toAlignedRect();
        const QPoint corners[] = {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()};
        const auto nearCorner = [&](int x, int y) {
            for (const QPoint &c : corners)
                if (qAbs(x - c.x()) + qAbs(y - c.y()) <= 7) return true;   // Anchor dots (4.5px radius).
            return false;
        };
        const QRect frameBand = box.adjusted(-1, -1, 1, 1);
        int outside = 0, frame = 0, interior = 0;
        const QRect scan = box.adjusted(-5, -5, 5, 5) & edit.rect();
        for (int y = scan.top(); y <= scan.bottom(); ++y)
            for (int x = scan.left(); x <= scan.right(); ++x) {
                if (edit.pixelColor(x, y) == bg) continue;
                if (nearCorner(x, y)) continue;
                const bool onFrame = (frameBand.contains(x, y)
                        && (qAbs(x - box.left()) <= 1 || qAbs(x - box.right()) <= 1
                            || qAbs(y - box.top()) <= 1 || qAbs(y - box.bottom()) <= 1));
                if (onFrame) ++frame;
                else if (!box.contains(x, y)) ++outside;
                else if (x > box.left() + 8) ++interior;  // Beyond the blinking cursor column.
            }
        check(outside == 0 && interior == 0 && frame == 0,
              "text editing draws anchor dots without a hairline frame or extra dashed boxes");
    }
    check(overlay.m_editor->font().pointSizeF() == 10.5 && overlay.m_editor->font().pixelSize() == -1
              && qAbs(QFontInfo(overlay.m_editor->font()).pixelSize() - 10.5 * overlay.logicalDpiY() / 72.0) <= 1,
          "font sizes use standard points and the screen DPI, not pixels mislabeled as points");
    auto key = [&overlay](int code, Qt::KeyboardModifiers mods = Qt::NoModifier,
                          const QString &text = QString()) {
        QKeyEvent ev(QEvent::KeyPress, code, mods, text);
        QApplication::sendEvent(overlay.m_textView, &ev);
    };
    QInputMethodEvent ime;
    ime.setCommitString(QStringLiteral("中文标注"));
    QApplication::sendEvent(overlay.m_textView, &ime);
    check(overlay.m_editor->toPlainText() == QStringLiteral("中文标注"),
          "native scene input accepts a committed Chinese composition");
    const qreal singleHeight = overlay.m_editor->boundingRect().height();
    key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    key(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    check(overlay.editorActive() && overlay.m_editor->toPlainText() == QStringLiteral("中文标注\nx")
              && overlay.m_editor->boundingRect().height() > singleHeight,
          "Enter inserts a new line and grows the text item without committing or pinning");
    const QString typed = overlay.m_editor->toPlainText();
    key(Qt::Key_Z, Qt::ControlModifier);
    check(overlay.editorActive() && overlay.m_anns.isEmpty() && overlay.m_editor->toPlainText() != typed,
          "Ctrl+Z first undoes typing inside the text document");
    key(Qt::Key_Y, Qt::ControlModifier);
    check(overlay.m_editor->toPlainText() == typed, "Ctrl+Y restores local typing without committing");
    key(Qt::Key_C, Qt::ControlModifier);
    check(overlay.editorActive() && overlay.isVisible(),
          "copy with no text selection cannot copy and close the whole capture");
    QTextCursor cur = overlay.m_editor->textCursor();
    cur.setPosition(1);
    cur.setPosition(3, QTextCursor::KeepAnchor);
    overlay.m_editor->setTextCursor(cur);
    const int anchor = cur.anchor(), position = cur.position();
    QComboBox *fontFamily = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("annotationFont"));
    check(fontFamily && fontFamily->focusPolicy() == Qt::NoFocus,
          "the text options include a font-family selector without stealing input focus");
    fontFamily->setCurrentText(QStringLiteral("Arial"));
    check(fontFamily->currentText() == QStringLiteral("Arial") && fontFamily->findText(QStringLiteral("Arial")) >= 0
              && fontFamily->width() == 140 && fontFamily->view()->minimumWidth() >= 200
              && fontFamily->toolTip().contains(QStringLiteral("Arial")),
          "the font selector names the selected family in its closed state and tooltip");
    fontEdit = overlay.m_sizeInput->findChild<QLineEdit *>();
    fontEdit->setFocus();
    fontEdit->setText(QStringLiteral("12"));
    QApplication::sendEvent(fontEdit, &fontEnter);
    check(overlay.editorActive() && overlay.m_editor->font().pointSizeF() == 12
              && overlay.m_editor->font().family() == QStringLiteral("Arial")
              && overlay.m_editor->textCursor().anchor() == anchor
              && overlay.m_editor->textCursor().position() == position
              && overlay.m_textView->hasFocus(),
          "editing the size keeps live text and its selection, then returns keyboard focus");
    for (QToolButton *button : overlay.m_toolOptions->findChildren<QToolButton *>()) {
        check(button->focusPolicy() == Qt::NoFocus, "text options do not steal keyboard focus");
        if (button->toolTip() == QStringLiteral("粗体") || button->toolTip() == QStringLiteral("斜体")
            || button->toolTip() == QStringLiteral("下划线"))
            button->click();
    }
    check(overlay.m_colorBoard && overlay.m_colorBoard->focusPolicy() == Qt::StrongFocus,
          "the text color board accepts keyboard focus");
    {
        const QPoint at = ColorBoard::swatchRect(5).center();
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, QPointF(at), QPointF(overlay.m_colorBoard->mapToGlobal(at)), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(overlay.m_colorBoard, &event);
        }
    }
    overlay.editorFontStep(1);
    check(overlay.editorActive() && overlay.m_editor->font().bold() && overlay.m_editor->font().italic()
              && overlay.m_editor->font().underline()
              && overlay.m_editFont == 12.5 && overlay.m_sizeInput->currentText().toDouble() == 12.5
              && overlay.m_editor->defaultTextColor() == QColor("#00a2e8")
              && overlay.m_editor->textCursor().anchor() == anchor
              && overlay.m_editor->textCursor().position() == position,
          "live color, bold, italic, and font size preserve input and selection");
    overlay.m_editor->setPlainText(QString(80, QLatin1Char('W')));
    check(overlay.m_editor->boundingRect().width() > 420
              && overlay.m_editor->document()->blockCount() == 1,
          "long text grows naturally without a fixed-width wrapping box");
    overlay.m_editor->setPlainText(QStringLiteral("中文标注\nsecond line"));
    const QPointF beforeMove = overlay.m_editor->pos();
    const QPoint border = overlay.m_textView->mapFromScene(
        overlay.m_editor->mapToScene(QPointF(1, overlay.m_editor->boundingRect().height() / 2)));
    const QPoint delta(24, 12);
    auto editorMouse = [&overlay](QEvent::Type type, const QPoint &at) {
        QWidget *viewport = overlay.m_textView->viewport();
        QMouseEvent ev(type, at, viewport->mapToGlobal(at),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
        QApplication::sendEvent(viewport, &ev);
    };
    editorMouse(QEvent::MouseButtonPress, border);
    editorMouse(QEvent::MouseMove, border + delta);
    editorMouse(QEvent::MouseButtonRelease, border + delta);
    check(overlay.editorActive() && overlay.m_editor->pos() == beforeMove + delta,
          "dragging the text margin moves the item and keeps text editing active");
    const QImage liveCapture = overlay.cropped().toImage();
    overlay.m_editor->clearFocus();
    const qreal dpr = overlay.m_shot.devicePixelRatioF();
    const QRect crop(qRound(overlay.m_sel.x() * dpr), qRound(overlay.m_sel.y() * dpr),
                     qRound(overlay.m_sel.width() * dpr), qRound(overlay.m_sel.height() * dpr));
    QPixmap nativeCapture = overlay.m_shot.copy(crop);
    nativeCapture.setDevicePixelRatio(1);
    {
        QPainter painter(&nativeCapture);
        overlay.m_textView->scene()->render(&painter, QRectF(nativeCapture.rect()), QRectF(overlay.m_sel));
    }
    overlay.m_editor->setFocus();
    check(nativeCapture.toImage() == liveCapture,
          "native scene text and capture text have identical pixels at 150 percent DPI");
    key(Qt::Key_Escape);
    check(!overlay.editorActive() && overlay.isVisible() && overlay.m_anns.size() == 1
              && overlay.m_atool == ATool::Text && overlay.m_anns[0].bold && overlay.m_anns[0].italic
              && overlay.m_anns[0].underline && overlay.m_selAnn == -1
              && overlay.m_anns[0].family == QStringLiteral("Arial")
              && overlay.m_anns[0].color == QColor("#00a2e8"),
          "Escape finishes text input, leaves the tool armed, and drops the selection handles");
    check(overlay.cropped().toImage() == liveCapture,
          "text pixels do not jump or wrap differently when editing ends at 150 percent DPI");
    const Annotation saved = overlay.m_anns[0];
    check(overlay.annHandleAt(saved, QPoint(saved.rect.center().x(), saved.rect.top() - 12)) == H_None
              && overlay.annHandleAt(saved, QPoint(saved.rect.center().x(), saved.rect.bottom() + 22)) == H_None,
          "text has no rotation hit target above or below its bounds");
    // The control ring selects for adjustment without opening the editor.
    const QPoint ring = saved.rect.topLeft() + QPoint(2, saved.rect.height() / 2);
    QMouseEvent ringPress(QEvent::MouseButtonPress, ring, overlay.mapToGlobal(ring),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent ringRelease(QEvent::MouseButtonRelease, ring, overlay.mapToGlobal(ring),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &ringPress);
    QApplication::sendEvent(&overlay, &ringRelease);
    QToolButton *boldButton = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("textBold"));
    QToolButton *italicButton = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("textItalic"));
    QToolButton *underlineButton = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("textUnderline"));
    check(!overlay.editorActive() && overlay.m_selAnn == 0
              && boldButton && boldButton->isChecked()
              && italicButton && italicButton->isChecked()
              && underlineButton && underlineButton->isChecked(),
          "clicking the text frame selects it for adjustment and syncs B/I/U state");
    const int undoCount = overlay.m_undoStack.size();
    overlay.beginTextEdit(saved.rect.center(), 0);
    overlay.commitTextEdit();
    check(overlay.m_undoStack.size() == undoCount && overlay.m_anns[0].rect == saved.rect,
          "opening and closing unchanged text adds no phantom undo step or geometry change");
    const QPoint click = saved.rect.topLeft() + QPoint(12, 12);
    QMouseEvent press(QEvent::MouseButtonPress, click, overlay.mapToGlobal(click),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, click, overlay.mapToGlobal(click),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &press);
    QApplication::sendEvent(&overlay, &release);
    check(overlay.editorActive() && overlay.m_editAnn == 0 && overlay.m_anns.size() == 1
              && overlay.m_textView->hasFocus()
              && overlay.m_textView->geometry() != overlay.rect(),
          "clicking locked text opens the focused editor without creating a duplicate");
    overlay.commitTextEdit();
    overlay.selectTool(ATool::None);
    check(!overlay.m_toolOptions->isVisible(), "disarming the text tool hides its option strip");
    QMouseEvent hiddenPress(QEvent::MouseButtonPress, click, overlay.mapToGlobal(click),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent hiddenRelease(QEvent::MouseButtonRelease, click, overlay.mapToGlobal(click),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &hiddenPress);
    QApplication::sendEvent(&overlay, &hiddenRelease);
    check(overlay.editorActive() && overlay.m_toolOptions->isVisible(),
          "clicking text with no tool reopens the editor and its option strip");
    cur = overlay.m_editor->textCursor();
    cur.setPosition(1);
    overlay.m_editor->setTextCursor(cur);
    key(Qt::Key_Return, Qt::ControlModifier);
    check(overlay.m_editor->toPlainText().startsWith(QStringLiteral("中文标注\n\n")),
          "Ctrl+Enter starts a new line at the end of the current paragraph");
    overlay.commitTextEdit();
    overlay.undoAnnotation();
    check(overlay.m_anns[0].text == saved.text, "one undo restores the entire text-edit transaction");
    overlay.redoAnnotation();
    check(overlay.m_anns[0].text != saved.text, "redo restores the edited multiline text");
    overlay.beginTextEdit(overlay.m_anns[0].rect.center(), 0);
    const QPoint editCorner = overlay.annAnchors(overlay.editorAnnotation()).last();
    QMouseEvent cornerPress(QEvent::MouseButtonPress, editCorner, overlay.mapToGlobal(editCorner),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &cornerPress);
    check(!overlay.editorActive() && overlay.m_selAnn == 0 && overlay.m_itemDrag == H_BR
              && overlay.m_pressPos == editCorner && !overlay.m_dragStarted,
          "corner resizing still works while the text editor is open");
    QMouseEvent cornerRelease(QEvent::MouseButtonRelease, editCorner, overlay.mapToGlobal(editCorner),
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &cornerRelease);
    const Annotation beforeResize = overlay.m_anns[0];
    overlay.pushUndo();
    overlay.m_selAnn = 0;
    overlay.m_pressAnn = beforeResize;
    overlay.m_itemDrag = H_BR;
    overlay.applyItemDrag(beforeResize.rect.bottomRight() + QPoint(50, 20));
    Annotation fitted = overlay.m_anns[0];
    fitTextAnnotation(fitted);
    check(overlay.m_anns[0].font > beforeResize.font && overlay.m_anns[0].text == beforeResize.text
              && overlay.m_anns[0].rect.size() == fitted.rect.size(),
          "text corner resizing scales the letters and keeps content-sized bounds");
    overlay.m_itemDrag = H_None;
    overlay.undoAnnotation();
    overlay.m_anns[0].rot = 30;
    const QPoint rotatedOrigin = mapFromLocal(overlay.m_anns[0], overlay.m_anns[0].rect.topLeft());
    overlay.beginTextEdit(mapFromLocal(overlay.m_anns[0], overlay.m_anns[0].rect.center()), 0);
    overlay.m_editor->setPlainText(QStringLiteral("旋转文字"));
    check(overlay.m_editor->rotation() == 30,
          "re-editing rotated text retains its native scene rotation");
    check(overlay.m_textView->mask().isEmpty(),
          "the text editor no longer leaves a click-through hole for a rotation knob");
    overlay.commitTextEdit();
    check(QLineF(rotatedOrigin, mapFromLocal(overlay.m_anns[0], overlay.m_anns[0].rect.topLeft())).length() < 2,
          "changing rotated text keeps its scene origin stable");
    overlay.beginTextEdit(mapFromLocal(overlay.m_anns[0], overlay.m_anns[0].rect.center()), 0);
    overlay.m_editor->setPlainText(QString());
    overlay.commitTextEdit();
    check(overlay.m_anns.isEmpty(), "clearing an existing text item deletes it");
    overlay.undoAnnotation();
    check(overlay.m_anns.size() == 1 && overlay.m_anns[0].text == QStringLiteral("旋转文字"),
          "undo restores deleted text with its formatting and rotation");
    Annotation topText;
    topText.tool = ATool::Text;
    topText.text = QStringLiteral("顶边");
    topText.rect = QRect(QPoint(origin.x() + 600, -6), QSize(80, 30));
    check(overlay.annHandleAt(topText, QPoint(topText.rect.center().x(), topText.rect.bottom() + 22)) == H_None,
          "text at the screen edge does not regain a rotation knob below its bounds");
    {   // Text input is fully visible outside the frame; committed content stays clipped.
        const QRect savedSel = overlay.m_sel;
        overlay.m_sel = QRect(origin + QPoint(100, 100), QSize(200, 100));
        overlay.selectTool(ATool::Text);
        overlay.beginTextEdit(QPoint(overlay.m_sel.right() - 20, overlay.m_sel.center().y()));
        overlay.m_editor->setPlainText(QStringLiteral("Boundary text extends outside"));
        QApplication::processEvents();
        overlay.syncTextEditorGeometry();
        check(overlay.m_textView->geometry().right() > overlay.m_sel.right(),
              "the text editor remains fully visible outside the capture frame");
        overlay.commitTextEdit();
        const int expectedWidth = qRound(overlay.m_sel.width() * overlay.m_shot.devicePixelRatioF());
        check(!overlay.m_anns.isEmpty() && overlay.cropped().width() == expectedWidth,
              "committed text is still clipped to the capture output");
        overlay.undoAnnotation();
        overlay.setToolNone();
        overlay.m_sel = savedSel;
    }
    Annotation cover;
    cover.tool = ATool::Marker;
    cover.fill = true;
    cover.rect = QRect(overlay.m_anns[0].rect.center() - QPoint(30, 20), QSize(60, 40));
    overlay.m_anns.append(cover);
    check(overlay.textAnnotationAt(overlay.m_anns[0].rect.center()) < 0,
          "text under a later annotation is not opened through it");
    overlay.m_anns.removeLast();
    for (ATool tool : {ATool::Arrow, ATool::Marker}) {
        Annotation previous;
        previous.tool = tool;
        previous.color = tool == ATool::Arrow ? QColor("#34c759") : QColor("#ffcc00");
        previous.a = origin + QPoint(450, 200);
        previous.b = origin + QPoint(520, 220);
        previous.stroke << previous.a << previous.b;
        overlay.m_anns.append(previous);
    }
    QVector<QColor> previousColors;
    for (const Annotation &ann : overlay.m_anns) previousColors.append(ann.color);
    overlay.selectTool(ATool::Text);
    overlay.beginTextEdit(origin + QPoint(300, 40));
    overlay.m_editor->setPlainText(QStringLiteral("未结束也能输出"));
    cur = overlay.m_editor->textCursor();
    cur.setPosition(1);
    cur.setPosition(3, QTextCursor::KeepAnchor);
    overlay.m_editor->setTextCursor(cur);
    const int paletteUndoCount = overlay.m_undoStack.size();
    const QPoint customAt = overlay.m_colorBoard->currentRect().center();
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, QPointF(customAt), QPointF(overlay.m_colorBoard->mapToGlobal(customAt)), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(overlay.m_colorBoard, &event);
    }
    QApplication::processEvents();
    QColorDialog *colorDialog = overlay.m_colorDialog;
    check(colorDialog && colorDialog->isVisible() && overlay.editorActive(),
          "opening the color dialog keeps native text editing live");
    const QColor textBefore = overlay.m_editor->defaultTextColor();
    colorDialog->setCurrentColor(QColor("#123abc"));
    check(overlay.m_editor->defaultTextColor() == QColor("#123abc")
              && overlay.m_editor->textCursor().anchor() == cur.anchor()
              && overlay.m_editor->textCursor().position() == cur.position(),
          "dialog color changes preview on the live text and keep its selection");
    colorDialog->reject();
    QApplication::processEvents();
    check(overlay.editorActive() && overlay.m_editor->defaultTextColor() == textBefore,
          "cancelling the dialog restores the live text color");
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, QPointF(customAt), QPointF(overlay.m_colorBoard->mapToGlobal(customAt)), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(overlay.m_colorBoard, &event);
    }
    QApplication::processEvents();
    colorDialog = overlay.m_colorDialog;
    check(colorDialog != nullptr, "the color dialog reopens during text editing");
    colorDialog->setCurrentColor(QColor("#123abc"));
    colorDialog->accept();
    QApplication::processEvents();
    check(overlay.editorActive() && overlay.isVisible() && overlay.m_editor->defaultTextColor() == QColor("#123abc")
              && overlay.m_textView->hasFocus(),
          "accepting the dialog keeps text editing and returns keyboard focus");
    for (int i = 0; i < previousColors.size(); ++i)
        check(overlay.m_anns[i].color == previousColors[i],
              "palette changes leave earlier text, arrows, and marker strokes unchanged");
    check(overlay.m_undoStack.size() == paletteUndoCount, "palette changes stay inside the current text transaction");
    QPixmap pinned;
    QRect pinnedRegion;
    QObject::connect(&overlay, &CaptureOverlay::grabbed,
                     [&pinned, &pinnedRegion](const QPixmap &pix, const QRect &region) {
        pinned = pix;
        pinnedRegion = region;
    });
    const QImage expected = overlay.cropped().toImage();
    const QRect expectedRegion = overlay.globalScreenRect(overlay.m_sel);
    overlay.pinCapture();
    check(!pinned.isNull() && pinned.toImage() == expected && overlay.m_anns.size() == 4,
          "capture actions commit the live text and output it exactly once");
    check(pinnedRegion == expectedRegion, "pinning preserves the annotated selection's original global geometry");
    std::puts("PASS: native text input, multiline layout, live formatting, re-editing, rotation, undo, and output");
    return 0;
}

int CaptureOverlay::testArrowComment(const QString &previewPath)
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    QImage image(720, 320, QImage::Format_RGB32);
    image.fill(QColor(20, 30, 40));
    CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(), QSize(720, 320)),
                                        QRect(0, 0, 720, 320), image}});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    const QPoint origin = overlay.mapFromGlobal(screen.topLeft());
    overlay.m_mode = Adjusting;
    overlay.m_sel = QRect(origin, QSize(720, 320));
    overlay.placeToolBars();
    overlay.showToolBars();
    overlay.selectTool(ATool::None);
    Annotation arrow;
    arrow.tool = ATool::Arrow;
    arrow.color = QColor(255, 0, 0);
    arrow.pen = 10;
    arrow.a = origin + QPoint(80, 160);
    arrow.b = origin + QPoint(640, 160);
    overlay.m_anns = {arrow};
    overlay.m_selAnn = -1;
    const QPoint cropOrigin = overlay.m_sel.topLeft();
    const QPoint mid = origin + QPoint(360, 160);
    const QImage plain = overlay.cropped().toImage();
    check(plain.pixelColor(mid - cropOrigin) == arrow.color,
          "the uncut arrow paints its shaft at the midpoint");
    check(overlay.m_anns[0].commentStyle.direction == 0
              && overlay.m_anns[0].commentStyle.font == 0,
          "new arrows start with inline text and the default style");

    auto openComment = [&](const QString &value, bool accept,
                           const std::function<void(QDialog *)> &inspect = {}) {
        QTimer poll;
        poll.setInterval(10);
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, [&] { check(false, "insert-text dialog did not appear"); });
        QObject::connect(&poll, &QTimer::timeout, [&] {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(w);
                if (!dialog || !dialog->isVisible()
                    || dialog->windowTitle() != QStringLiteral("插入文本")) continue;
                poll.stop();
                if (inspect) inspect(dialog);
                if (accept) {
                    if (auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("commentText")))
                        edit->setText(value);
                    for (auto *button : dialog->findChildren<QPushButton *>())
                        if (button->text() == QStringLiteral("确定")) {
                            poll.stop();
                            button->click();
                            return;
                        }
                }
                poll.stop();
                dialog->reject();
                return;
            }
        });
        timeout.start(5000);
        poll.start();
        QMouseEvent event(QEvent::MouseButtonDblClick, mid, overlay.mapToGlobal(mid),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &event);
        poll.stop();
        timeout.stop();
        QApplication::processEvents();
    };

    bool sawEmpty = false;
    openComment(QStringLiteral("I"), true, [&](QDialog *dialog) {
        auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("commentText"));
        sawEmpty = edit && edit->text().isEmpty();
        auto *surface = dialog->findChild<QWidget *>(QStringLiteral("dialogSurface"));
        auto *font = dialog->findChild<QFontComboBox *>(QStringLiteral("commentFont"));
        auto *size = dialog->findChild<QComboBox *>(QStringLiteral("commentSize"));
        auto *bold = dialog->findChild<QToolButton *>(QStringLiteral("commentBold"));
        auto *italic = dialog->findChild<QToolButton *>(QStringLiteral("commentItalic"));
        auto *color = dialog->findChild<QToolButton *>(QStringLiteral("commentColor"));
        auto *style = dialog->findChild<QToolButton *>(QStringLiteral("commentStyle"));
        auto *directions = dialog->findChild<QWidget *>(QStringLiteral("commentDirections"));
        auto *group = dialog->findChild<QButtonGroup *>(QStringLiteral("commentDirection"));
        auto *fontLabel = dialog->findChild<QLabel *>(QStringLiteral("commentFontLabel"));
        auto *directionLabel = dialog->findChild<QLabel *>(QStringLiteral("commentDirectionLabel"));
        check(surface && edit && font && size && bold && italic && color && !style && directions && group
                  && fontLabel && directionLabel, "the card exposes all prototype controls");
        const auto position = [surface](QWidget *widget) { return widget->mapTo(surface, QPoint()); };
        check(dialog->windowFlags().testFlag(Qt::FramelessWindowHint) && surface->width() == 500
                  && edit->placeholderText().isEmpty() && edit->height() == 48
                  && edit->width() == 458 && size->width() == 52 && size->height() == 32,
              "the custom card matches the 500px prototype and blank input");
        check(position(font).x() == position(directions).x()
                  && position(color).x() + color->width() == position(edit).x() + edit->width()
                  && position(color).x() == position(italic).x() + italic->width() + 8
                  && position(directions).x() + directions->width() == position(edit).x() + edit->width()
                  && position(font).y() == position(italic).y()
                  && position(font).y() == position(size).y(),
              "font and direction rows fill and align with the content edges");
        check(fontLabel->palette().color(QPalette::WindowText) == QColor(Qt::white)
                  && directionLabel->palette().color(QPalette::WindowText) == QColor(Qt::white)
                  && fontLabel->font().weight() >= QFont::DemiBold
                  && directionLabel->font().weight() >= QFont::DemiBold,
              "font and direction labels are white and semibold");
        check(group->buttons().size() == 4 && color->accessibleName() == QStringLiteral("文字颜色")
                  && italic->nextInFocusChain() == color,
              "four directions and the text color button retain accessible names and keyboard order");
        check(edit->font().pixelSize() == 16 && !edit->font().bold() && !edit->font().italic(),
              "the input is a fixed-size editor, not the formatting preview");
        edit->setText(QStringLiteral("Draft"));
        check(overlay.m_anns[0].comment == QStringLiteral("Draft") && overlay.cropped().toImage() != plain
                  && overlay.m_undoStack.isEmpty(),
              "typing on a fresh arrow immediately displays its first text without an undo step");
        edit->clear();
        check(overlay.cropped().toImage() == plain, "clearing fresh preview text immediately restores the shaft");
        QImage fontRow(260, 32, QImage::Format_ARGB32_Premultiplied);
        fontRow.fill(QColor("#18191d"));
        QStyleOptionViewItem rowOption;
        rowOption.initFrom(font->view());
        rowOption.font = font->font();
        rowOption.rect = fontRow.rect();
        rowOption.displayAlignment = Qt::AlignLeft;
        QPainter rowPainter(&fontRow);
        rowPainter.setPen(rowOption.palette.color(QPalette::Text));
        font->itemDelegate()->paint(&rowPainter, rowOption,
                                    font->model()->index(font->findText(QStringLiteral("Arial")), 0));
        rowPainter.end();
        int inkTop = fontRow.height(), inkBottom = -1;
        for (int y = 0; y < fontRow.height(); ++y)
            for (int x = 24; x < fontRow.width(); ++x) {
                const QColor color = fontRow.pixelColor(x, y);
                if (color.red() > 150 && color.green() > 150 && color.blue() > 150) {
                    inkTop = qMin(inkTop, y);
                    inkBottom = qMax(inkBottom, y);
                }
            }
        check(inkBottom >= inkTop && inkBottom - inkTop + 1 <= 14,
              "the actual font delegate no longer enlarges the list text");
        if (!previewPath.isEmpty()) {
            font->view()->scrollTo(font->model()->index(0, 0), QAbstractItemView::PositionAtTop);
            font->showPopup();
            font->view()->scrollTo(font->model()->index(0, 0), QAbstractItemView::PositionAtTop);
            QApplication::processEvents();
            font->view()->window()->grab().save(previewPath + QStringLiteral(".font-list.png"));
            font->hidePopup();
        }
        for (auto *button : group->buttons())
            check(qAbs(button->width() - group->button(0)->width()) <= 1,
                  "direction segments are equal width");
        QFocusEvent keyboardFocus(QEvent::FocusIn, Qt::TabFocusReason);
        QApplication::sendEvent(bold, &keyboardFocus);
        check(bold->property("kbdFocus").toBool(), "keyboard focus keeps a neutral focus indicator");
        const QImage focusPixels = bold->grab().toImage();
        const QColor focusEdge = focusPixels.pixelColor(focusPixels.width() / 2,
                                                       qRound(focusPixels.devicePixelRatio()));
        check(qAbs(focusEdge.red() - 197) < 20 && qAbs(focusEdge.green() - 199) < 20
                  && qAbs(focusEdge.blue() - 206) < 20,
              "keyboard focus paints the neutral gray outline");
        check(bold->nextInFocusChain() == italic, "the keyboard focus chain follows the font-row order");
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(16, 16), bold->mapToGlobal(QPoint(16, 16)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bold, &press);
        check(!bold->property("kbdFocus").toBool(), "pointer clicks remove the focus indicator");
        bold->setDown(false);
        const QImage buttonPixels = bold->grab().toImage();
        for (int y = 0; y < buttonPixels.height(); ++y)
            for (int x = 0; x < buttonPixels.width(); ++x) {
                const QColor color = buttonPixels.pixelColor(x, y);
                check(color.blue() - color.red() < 40, "button clicks do not paint a colored halo");
            }
        if (!previewPath.isEmpty()) {
            surface->grab().save(previewPath + QStringLiteral(".card.png"));
            size->setCurrentIndex(size->findData(40));
            surface->grab().save(previewPath + QStringLiteral(".card-size40.png"));
            size->setCurrentIndex(0);
        }
        const QImage sizeImage = size->grab().toImage();
        int inkLeft = sizeImage.width(), inkRight = -1;
        for (int y = qRound(6 * sizeImage.devicePixelRatio()); y < sizeImage.height() - qRound(6 * sizeImage.devicePixelRatio()); ++y)
            for (int x = qRound(4 * sizeImage.devicePixelRatio()); x < sizeImage.width() - qRound(4 * sizeImage.devicePixelRatio()); ++x) {
                const QColor color = sizeImage.pixelColor(x, y);
                if (color.red() > 170 && color.green() > 170 && color.blue() > 170) {
                    inkLeft = qMin(inkLeft, x);
                    inkRight = qMax(inkRight, x);
                }
            }
        check(inkRight >= inkLeft && qAbs((inkLeft + inkRight) * 0.5 - sizeImage.width() * 0.5)
                  <= 3 * sizeImage.devicePixelRatio(), "the closed size value is horizontally centered");
        edit->setFocus(Qt::OtherFocusReason);
    });
    check(sawEmpty && overlay.m_anns.size() == 1
              && overlay.m_anns[0].comment == QStringLiteral("I") && overlay.m_undoStack.size() == 1,
          "double-click opens an empty comment dialog and commits one undo step");
    const QImage withComment = overlay.cropped().toImage();
    QPainterPath textPath, holePath;
    commentPaths(overlay.m_anns[0], textPath, holePath);
    check(!textPath.isEmpty() && !holePath.isEmpty(), "the comment builds glyph and cutout paths");
    const QRectF holeBox = holePath.boundingRect();
    const QRectF glyphBox = textPath.boundingRect();
    const QPoint hole(qRound(qMin(holeBox.right() - 2.0, glyphBox.right() + 2.0)) - cropOrigin.x(),
                      qRound(holeBox.center().y()) - cropOrigin.y());
    check(plain.pixelColor(hole) == arrow.color && withComment.pixelColor(hole) == QColor(20, 30, 40),
          "the comment cuts the arrow shaft down to the captured pixels");
    int white = 0;
    const QRect scan = QRect(mid - QPoint(30, 20) - cropOrigin, QSize(60, 40)).intersected(withComment.rect());
    for (int y = scan.top(); y <= scan.bottom(); ++y)
        for (int x = scan.left(); x <= scan.right(); ++x) {
            const QColor c = withComment.pixelColor(x, y);
            if (c.red() > 200 && c.green() > 200 && c.blue() > 200) ++white;
        }
    check(white == 0, "inserted text has no white outline");

    QPoint stroke;
    for (int y = qCeil(glyphBox.top()); y <= qFloor(glyphBox.bottom()) && stroke.isNull(); ++y)
        for (int x = qCeil(glyphBox.left()); x <= qFloor(glyphBox.right()); ++x)
            if (textPath.contains(QPointF(x, y))) {
                stroke = QPoint(x, y) - cropOrigin;
                break;
            }
    check(!stroke.isNull(), "the glyph path has a fillable interior");
    check(withComment.pixelColor(stroke).red() > 200 && withComment.pixelColor(stroke).green() < 80,
          "inserted text is filled with the arrow color");

    bool sawExisting = false, controlsOk = false;
    openComment(QString(), false, [&](QDialog *dialog) {
        auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("commentText"));
        auto *direction = dialog->findChild<QButtonGroup *>(QStringLiteral("commentDirection"));
        auto *size = dialog->findChild<QComboBox *>(QStringLiteral("commentSize"));
        auto *font = dialog->findChild<QFontComboBox *>(QStringLiteral("commentFont"));
        auto *bold = dialog->findChild<QToolButton *>(QStringLiteral("commentBold"));
        auto *italic = dialog->findChild<QToolButton *>(QStringLiteral("commentItalic"));
        sawExisting = edit && edit->text() == QStringLiteral("I");
        controlsOk = direction && direction->checkedId() == 0
            && size && size->currentData().toInt() == 0
            && font && font->currentFont().family() == overlay.m_anns[0].commentStyle.family
            && bold && !bold->isChecked() && italic && !italic->isChecked();
        if (direction) direction->button(1)->click();
    });
    check(sawExisting && controlsOk && overlay.m_anns[0].comment == QStringLiteral("I")
              && overlay.m_undoStack.size() == 1,
          "double-click reopens text, direction, font, size and B/I; cancel keeps it");
    auto chooseTextColor = [&](QDialog *dialog, const QColor &value, bool accept,
                               const std::function<void(ColorDialog *)> &inspect = {}) {
        QTimer poll, timeout;
        QEventLoop waiting;
        poll.setInterval(10);
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, [&] { check(false, "text color palette did not finish"); });
        QObject::connect(&poll, &QTimer::timeout, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *palette = qobject_cast<ColorDialog *>(widget);
                if (!palette || !palette->isVisible()) continue;
                poll.stop();
                check(palette->parentWidget() == dialog, "text color uses the shared palette owned by the text card");
                if (value.isValid()) {
                    palette->setCurrentColor(value);
                    check(overlay.m_anns[0].commentStyle.color == value
                              && overlay.m_anns[0].color == arrow.color,
                          "palette edits preview the text color without recoloring the shaft");
                }
                if (inspect) inspect(palette);
                if (accept) palette->accept(); else palette->reject();
                waiting.quit();
                return;
            }
        });
        timeout.start(5000);
        poll.start();
        dialog->findChild<QToolButton *>(QStringLiteral("commentColor"))->click();
        waiting.exec();
        timeout.stop();
    };
    openComment(QString(), false, [&](QDialog *dialog) {
        auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("commentText"));
        auto *font = dialog->findChild<QFontComboBox *>(QStringLiteral("commentFont"));
        auto *size = dialog->findChild<QComboBox *>(QStringLiteral("commentSize"));
        auto *bold = dialog->findChild<QToolButton *>(QStringLiteral("commentBold"));
        auto *italic = dialog->findChild<QToolButton *>(QStringLiteral("commentItalic"));
        auto *surface = dialog->findChild<QWidget *>(QStringLiteral("dialogSurface"));
        const int compactHeight = dialog->height();
        const int undoBefore = overlay.m_undoStack.size();
        const CommentStyle defaultsBefore = defaultCommentStyle();
        const QFont editorFont = edit->font();
        const QImage before = overlay.cropped().toImage();
        edit->clear();
        check(overlay.m_anns[0].comment.isEmpty() && overlay.cropped().toImage() == plain,
              "clearing the editor immediately restores the uncut arrow");
        edit->setText(QStringLiteral("Hello 文本"));
        edit->setCursorPosition(3);
        check(overlay.m_anns[0].comment == edit->text() && overlay.cropped().toImage() != before,
              "typing immediately draws the new text on the screenshot arrow");
        QApplication::processEvents();
        const QImage paintedText = overlay.grab().toImage();
        const QImage initialPixels = overlay.cropped().toImage();
        font->setCurrentFont(QFont(QStringLiteral("Arial")));
        check(overlay.m_anns[0].commentStyle.family == QStringLiteral("Arial") && edit->cursorPosition() == 3,
              "changing family immediately updates the arrow without resetting the editor cursor");
        size->setCurrentIndex(size->findData(20));
        check(qAbs(commentFont(overlay.m_anns[0]).pointSizeF() - 20) < 0.01,
              "changing point size immediately updates the arrow glyph font");
        const QImage sizePixels = overlay.cropped().toImage();
        check(sizePixels != initialPixels, "point size changes repaint the arrow text");
        bold->click();
        check(overlay.m_anns[0].commentStyle.bold && overlay.cropped().toImage() != sizePixels,
              "bold immediately repaints the screenshot arrow with bold glyphs");
        QApplication::processEvents();
        check(overlay.grab().toImage() != paintedText,
              "the visible overlay paint changes while the text card remains open");
        const QImage boldPixels = overlay.cropped().toImage();
        italic->click();
        check(overlay.m_anns[0].commentStyle.italic && overlay.cropped().toImage() != boldPixels,
              "italic immediately repaints the screenshot arrow with italic glyphs");
        size->setCurrentIndex(size->findData(40));
        edit->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        check(qAbs(commentFont(overlay.m_anns[0]).pointSizeF() - 40) < 0.01
                  && edit->font() == editorFont && edit->height() == 48 && dialog->height() == compactHeight,
              "large formatting changes the arrow, not the editor font or card size");
        if (!previewPath.isEmpty()) {
            surface->grab().save(previewPath + QStringLiteral(".card-live.png"));
            overlay.cropped().save(previewPath + QStringLiteral(".arrow-live40.png"));
            const QRect visible = QRect(overlay.mapToGlobal(overlay.m_sel.topLeft()), overlay.m_sel.size())
                                      .united(dialog->geometry()).adjusted(-10, -10, 10, 10)
                                      .intersected(dialog->screen()->geometry());
            dialog->screen()->grabWindow(0, visible.x(), visible.y(), visible.width(), visible.height())
                .save(previewPath + QStringLiteral(".live-screen.png"));
        }
        const QRect available = dialog->screen()->availableGeometry();
        size->setCurrentIndex(size->findData(71));
        QApplication::processEvents();
        check(qAbs(commentFont(overlay.m_anns[0]).pointSizeF() - 71) < 0.01
                  && edit->font() == editorFont && available.contains(dialog->geometry()),
              "the maximum arrow size preserves the fixed editor and on-screen card");
        size->setCurrentIndex(0);
        bold->click();
        italic->click();
        QApplication::processEvents();
        check(qAbs(commentFont(overlay.m_anns[0]).pointSizeF() - (6.0 + arrow.pen * 0.382)) < 0.01
                  && edit->font() == editorFont && edit->text() == QStringLiteral("Hello 文本"),
              "automatic size restores the arrow-derived glyph size without changing the editor");
        auto *direction = dialog->findChild<QButtonGroup *>(QStringLiteral("commentDirection"));
        for (int i = 0; i < 4; ++i) {
            direction->button(i)->click();
            check(overlay.m_anns[0].commentStyle.direction == i, "direction changes immediately reach the arrow");
        }
        direction->button(0)->click();
        size->setCurrentIndex(size->findData(20));
        chooseTextColor(dialog, QColor("#00ff00"), false, [&](ColorDialog *palette) {
            const QImage output = overlay.cropped().toImage();
            int green = 0;
            for (int y = 0; y < output.height(); ++y)
                for (int x = 0; x < output.width(); ++x) {
                    const QColor c = output.pixelColor(x, y);
                    if (c.green() > 180 && c.red() < 40 && c.blue() < 40) ++green;
                }
            check(green > 20, "palette changes visibly recolor the arrow text before either dialog is confirmed");
            if (!previewPath.isEmpty()) {
                QApplication::processEvents();
                palette->findChild<QWidget *>(QStringLiteral("dialogSurface"))->grab()
                    .save(previewPath + QStringLiteral(".palette.png"));
                overlay.cropped().save(previewPath + QStringLiteral(".green-text.png"));
            }
        });
        check(!overlay.m_anns[0].commentStyle.color.isValid(),
              "cancelling the palette restores the inherited arrow color");
        chooseTextColor(dialog, QColor("#00ff00"), true);
        const QImage opaqueColor = overlay.cropped().toImage();
        chooseTextColor(dialog, QColor(0, 255, 0, 100), false, [&](ColorDialog *palette) {
            check(overlay.cropped().toImage() != opaqueColor && palette->currentColor().alpha() == 100,
                  "text alpha previews in the arrow output");
        });
        chooseTextColor(dialog, QColor("#0000ff"), false);
        check(overlay.m_anns[0].commentStyle.color == QColor("#00ff00"),
              "cancelling a later palette restores the previous text color");
        chooseTextColor(dialog, QColor(), false, [&](ColorDialog *palette) {
            check(palette->currentColor() == QColor("#00ff00"), "reopening the palette uses the live text color");
            QTimer pickerPoll, pickerTimeout;
            pickerPoll.setInterval(10);
            pickerTimeout.setSingleShot(true);
            QObject::connect(&pickerTimeout, &QTimer::timeout, [&] { check(false, "text screen picker did not finish"); });
            bool sampled = false;
            QObject::connect(&pickerPoll, &QTimer::timeout, [&] {
                for (QWidget *widget : QApplication::topLevelWidgets()) {
                    auto *picker = qobject_cast<ScreenColorPicker *>(widget);
                    if (!picker || !picker->isVisible()) continue;
                    pickerPoll.stop();
                    const QPoint local = picker->mapFromGlobal(screen.topLeft() + QPoint(20, 20));
                    QMouseEvent click(QEvent::MouseButtonPress, local, picker->mapToGlobal(local),
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(picker, &click);
                    sampled = true;
                    return;
                }
            });
            pickerTimeout.start(5000);
            pickerPoll.start();
            palette->findChild<QPushButton *>(QStringLiteral("colorPickScreen"))->click();
            pickerTimeout.stop();
            check(sampled && palette->isVisible() && palette->currentColor() == QColor(20, 30, 40)
                      && overlay.m_anns[0].commentStyle.color == QColor(20, 30, 40),
                  "screen sampling returns to the palette and previews the sampled text color");
        });
        check(overlay.m_anns[0].commentStyle.color == QColor("#00ff00"),
              "cancelling the palette after screen sampling restores its previous text color");
        const CommentStyle defaultsAfter = defaultCommentStyle();
        check(overlay.m_undoStack.size() == undoBefore
                  && defaultsAfter.family == defaultsBefore.family && defaultsAfter.font == defaultsBefore.font
                  && defaultsAfter.bold == defaultsBefore.bold && defaultsAfter.italic == defaultsBefore.italic
                  && defaultsAfter.color == defaultsBefore.color,
              "live preview leaves the undo stack and persisted defaults unchanged before confirmation");
        check(!dialog->geometry().contains(overlay.mapToGlobal(mid)), "the preview card does not cover the arrow midpoint");
    });
    check(overlay.m_anns[0].comment == QStringLiteral("I") && overlay.m_undoStack.size() == 1
              && overlay.m_anns[0].commentStyle.family == CommentStyle{}.family
              && !overlay.m_anns[0].commentStyle.color.isValid() && overlay.cropped().toImage() == withComment,
          "cancelling live preview preserves the arrow text and style");
    {
        QTimer keys;
        keys.setInterval(10);
        QTimer keyTimeout;
        keyTimeout.setSingleShot(true);
        QObject::connect(&keyTimeout, &QTimer::timeout, [&] { check(false, "keyboard edit did not finish"); });
        keyTimeout.start(5000);
        bool sawKeys = false;
        QObject::connect(&keys, &QTimer::timeout, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (!dialog || !dialog->isVisible() || dialog->windowTitle() != QStringLiteral("插入文本")) continue;
                keys.stop();
                auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("commentText"));
                check(edit && edit->selectedText() == QStringLiteral("I"), "reopening selects the existing text");
                edit->setText(QStringLiteral("Cancelled"));
                check(overlay.m_anns[0].comment == QStringLiteral("Cancelled"),
                      "Escape cancels an already visible arrow preview, not only editor text");
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(edit, &escape);
                sawKeys = true;
                return;
            }
        });
        keys.start();
        overlay.editArrowComment(0);
        check(sawKeys && overlay.m_anns[0].comment == QStringLiteral("I"), "Escape cancels text changes");
        keys.disconnect();
        QObject::connect(&keys, &QTimer::timeout, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (!dialog || !dialog->isVisible() || dialog->windowTitle() != QStringLiteral("插入文本")) continue;
                keys.stop();
                auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("commentText"));
                edit->setText(QStringLiteral("I"));
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(edit, &enter);
                return;
            }
        });
        keys.start();
        overlay.editArrowComment(0);
        check(overlay.m_undoStack.size() == 1, "Enter accepts unchanged text without another undo step");
        keyTimeout.stop();
    }

    const int undoBeforeStyle = overlay.m_undoStack.size();
    openComment(QStringLiteral("Styled"), true, [&](QDialog *dialog) {
        auto *direction = dialog->findChild<QButtonGroup *>(QStringLiteral("commentDirection"));
        auto *size = dialog->findChild<QComboBox *>(QStringLiteral("commentSize"));
        auto *font = dialog->findChild<QFontComboBox *>(QStringLiteral("commentFont"));
        if (direction) direction->button(1)->click();
        if (size) size->setCurrentIndex(size->findData(20));
        if (font) font->setCurrentFont(QFont(QStringLiteral("Arial")));
        if (auto *bold = dialog->findChild<QToolButton *>(QStringLiteral("commentBold"))) bold->click();
        if (auto *italic = dialog->findChild<QToolButton *>(QStringLiteral("commentItalic"))) italic->click();
        chooseTextColor(dialog, QColor("#00ff00"), true);
    });
    const CommentStyle styled = overlay.m_anns[0].commentStyle;
    check(overlay.m_anns[0].comment == QStringLiteral("Styled")
              && styled.direction == 1 && qRound(styled.font) == 20 && styled.bold && styled.italic
              && styled.family == QStringLiteral("Arial") && styled.color == QColor("#00ff00")
              && overlay.m_undoStack.size() == undoBeforeStyle + 1,
          "confirming applies direction, font, size and B/I in one undo step");
    overlay.undoAnnotation();
    check(overlay.m_anns[0].comment == QStringLiteral("I") && !overlay.m_anns[0].commentStyle.color.isValid()
              && overlay.cropped().toImage() == withComment,
          "one undo restores the pre-dialog text, style, color and output pixels");
    overlay.redoAnnotation();
    check(overlay.m_anns[0].comment == QStringLiteral("Styled")
              && overlay.m_anns[0].commentStyle.color == QColor("#00ff00"),
          "one redo restores the committed live-preview result including its text color");
    openComment(QString(), false, [&](QDialog *dialog) {
        chooseTextColor(dialog, QColor(), false, [&](ColorDialog *palette) {
            check(palette->currentColor() == QColor("#00ff00"), "re-editing restores the committed custom text color");
        });
    });
    overlay.undoAnnotation();
    const int redoBeforeCancel = overlay.m_redoStack.size();
    const CommentStyle defaultsBeforeCancel = defaultCommentStyle();
    openComment(QString(), false, [&](QDialog *dialog) {
        dialog->findChild<QLineEdit *>(QStringLiteral("commentText"))->setText(QStringLiteral("Discard"));
        dialog->findChild<QComboBox *>(QStringLiteral("commentSize"))->setCurrentIndex(10);
        check(overlay.m_redoStack.size() == redoBeforeCancel, "preview does not discard an existing redo branch");
        dialog->findChild<QToolButton *>(QStringLiteral("commentClose"))->click();
    });
    check(overlay.m_anns[0].comment == QStringLiteral("I") && overlay.m_redoStack.size() == redoBeforeCancel
              && defaultCommentStyle().color == defaultsBeforeCancel.color
              && overlay.cropped().toImage() == withComment,
          "closing the card restores the pre-preview arrow and preserves redo and defaults");
    overlay.redoAnnotation();

    CommentStyle &flat = overlay.m_anns[0].commentStyle;
    flat.direction = 1;
    check(qFuzzyIsNull(commentAngle(overlay.m_anns[0])), "flat comments keep a zero angle");
    flat.direction = 0;
    overlay.m_anns[0].a = origin + QPoint(600, 260);
    overlay.m_anns[0].b = origin + QPoint(120, 60);
    check(commentAngle(overlay.m_anns[0]) >= -95.0 && commentAngle(overlay.m_anns[0]) <= 95.0,
          "follow keeps an up-left shaft label upright");
    overlay.m_anns[0].a = origin + QPoint(80, 160);
    overlay.m_anns[0].b = origin + QPoint(640, 160);

    flat.family = QStringLiteral("Arial");
    flat.font = 20;
    flat.bold = true;
    flat.italic = true;
    const QFont styledFont = commentFont(overlay.m_anns[0]);
    check(styledFont.family() == QStringLiteral("Arial") && qRound(styledFont.pointSizeF()) == 20
              && styledFont.bold() && styledFont.italic(),
          "manual family, size, bold and italic reach the glyph font");
    flat.font = 0;
    const QFont autoFont = commentFont(overlay.m_anns[0]);
    check(qAbs(autoFont.pointSizeF() - (6.0 + arrow.pen * 0.382)) < 0.01,
          "automatic size follows the arrow pen width");

    for (int direction = 2; direction <= 3; ++direction) {
        const int undoBefore = overlay.m_undoStack.size();
        const int previousDirection = overlay.m_anns[0].commentStyle.direction;
        openComment(QStringLiteral("Side"), true, [&](QDialog *dialog) {
            dialog->findChild<QButtonGroup *>(QStringLiteral("commentDirection"))->button(direction)->click();
        });
        check(overlay.m_anns[0].commentStyle.direction == direction
                  && overlay.m_undoStack.size() == undoBefore + 1,
              "above and below direction changes commit one undo step");
        overlay.undoAnnotation();
        check(overlay.m_anns[0].commentStyle.direction == previousDirection,
              "undo restores the previous text direction");
        overlay.redoAnnotation();
        check(overlay.m_anns[0].commentStyle.direction == direction,
              "redo restores the side text direction");
        openComment(QString(), false, [&](QDialog *dialog) {
            check(dialog->findChild<QButtonGroup *>(QStringLiteral("commentDirection"))->checkedId() == direction,
                  "re-editing restores the side direction");
            check(dialog->findChild<QLineEdit *>(QStringLiteral("commentText"))->font().pixelSize() == 16,
                  "re-editing keeps the editor font fixed while the arrow retains its style");
            dialog->findChild<QButtonGroup *>(QStringLiteral("commentDirection"))->button(0)->click();
        });
        check(overlay.m_anns[0].commentStyle.direction == direction,
              "cancelling a direction change preserves the saved direction");
    }
    for (int direction = 0; direction < 4; ++direction) {
        for (const QPoint &delta : {QPoint(400, 0), QPoint(-400, -140), QPoint(0, 200), QPoint(320, -120)}) {
            Annotation positioned = arrow;
            positioned.comment = QStringLiteral("Parallel");
            positioned.commentStyle.direction = direction;
            positioned.commentStyle.font = 20;
            positioned.a = mid - delta / 2;
            positioned.b = mid + delta / 2;
            QPainterPath glyphs, hole;
            commentPaths(positioned, glyphs, hole);
            const QRectF labelBox = hole.boundingRect();
            const qreal shaftHalf = positioned.pen * 0.5;
            check(direction == 2 ? labelBox.bottom() < mid.y() - shaftHalf
                      : direction == 3 ? labelBox.top() > mid.y() + shaftHalf
                      : qAbs(labelBox.center().y() - mid.y()) < 0.01,
                  "side labels clear the shaft and inline labels stay centered");
            Annotation inlineArrow = positioned;
            inlineArrow.commentStyle.direction = 0;
            check(direction == 1 ? qFuzzyIsNull(commentAngle(positioned))
                      : qFuzzyCompare(commentAngle(positioned) + 360, commentAngle(inlineArrow) + 360),
                  "above and below remain parallel and upright for horizontal, reversed, vertical and diagonal arrows");
            QImage output(image.size(), QImage::Format_RGB32);
            output.fill(QColor(20, 30, 40));
            QPainter painter(&output);
            painter.translate(-cropOrigin);
            overlay.drawAnnotation(painter, positioned, QImage());
            painter.end();
            check(direction < 2 || output.pixelColor(mid - cropOrigin) == arrow.color,
                  "side labels preserve the continuous arrow shaft in output");
            if (!previewPath.isEmpty() && delta == QPoint(320, -120))
                output.save(previewPath + QStringLiteral(".direction%1.png").arg(direction));
        }
    }

    CommentStyle saved;
    saved.family = QStringLiteral("Arial");
    saved.font = 20;
    saved.direction = 3;
    saved.bold = true;
    saved.italic = true;
    saved.color = QColor(1, 2, 3, 128);
    saveCommentStyle(saved);
    const CommentStyle loaded = defaultCommentStyle();
    check(loaded.family == saved.family && qFuzzyCompare(loaded.font + 1.0, saved.font + 1.0)
              && loaded.direction == saved.direction && loaded.bold == saved.bold
              && loaded.italic == saved.italic && loaded.color == saved.color,
          "comment style round-trips through the settings file");
    saveCommentStyle(CommentStyle{});                    // Keep later tests on the defaults.
    {
        LensSnipSettings settings;
        settings.remove(QStringLiteral("Capture/EmbedTextDirection"));
        settings.setValue(QStringLiteral("Capture/EmbedTextColor"), QStringLiteral("not-a-color"));
        check(!defaultCommentStyle().color.isValid(), "invalid persisted text colors fall back to the arrow");
        settings.setValue(QStringLiteral("Capture/EmbedTextFollowArrow"), false);
        check(defaultCommentStyle().direction == 1, "legacy horizontal defaults remain compatible");
        settings.setValue(QStringLiteral("Capture/EmbedTextDirection"), 99);
        check(defaultCommentStyle().direction == 3, "invalid persisted directions are bounded");
        saveCommentStyle(CommentStyle{});
    }

    {
        LensSnipSettings settings;
        settings.setValue(QStringLiteral("Capture/DisableArrowDoubleClickComment"), true);
        bool sawDialog = false;
        QTimer guard;
        guard.setInterval(10);
        QObject::connect(&guard, &QTimer::timeout, [&] {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(w);
                if (dialog && dialog->isVisible()
                    && dialog->windowTitle() == QStringLiteral("插入文本")) {
                    sawDialog = true;
                    dialog->reject();
                }
            }
        });
        guard.start();
        QMouseEvent blocked(QEvent::MouseButtonDblClick, mid, overlay.mapToGlobal(mid),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &blocked);
        guard.stop();
        QApplication::processEvents();
        check(!sawDialog, "the no-double-click-comment setting suppresses the card");
        settings.setValue(QStringLiteral("Capture/DisableArrowDoubleClickComment"), false);
    }

    overlay.m_anns[0].commentStyle = CommentStyle{};
    overlay.m_anns[0].comment = QStringLiteral("I");
    overlay.update();
    const int undoBeforeClear = overlay.m_undoStack.size();
    openComment(QString(), true);
    check(overlay.m_anns.size() == 1 && overlay.m_anns[0].comment.isEmpty()
              && overlay.m_undoStack.size() == undoBeforeClear + 1,
          "clearing the comment removes the label but keeps the arrow");
    check(overlay.cropped().toImage() == plain, "clearing the comment restores the uncut arrow pixels");

    overlay.undoAnnotation();
    check(overlay.m_anns[0].comment == QStringLiteral("I"), "undo restores the comment");
    overlay.redoAnnotation();
    check(overlay.m_anns[0].comment.isEmpty(), "redo removes the comment again");

    if (!previewPath.isEmpty()) {
        plain.save(previewPath + QStringLiteral(".plain.png"));
        overlay.undoAnnotation();                         // Bring the comment back for previews.
        CommentStyle &previewStyle = overlay.m_anns[0].commentStyle;
        previewStyle.direction = 0;
        previewStyle.font = 0;
        overlay.m_anns[0].comment = QStringLiteral("Hello");
        overlay.update();
        const QImage preview = overlay.cropped().toImage();
        preview.save(previewPath + QStringLiteral(".comment.png"));
        const QRect zoom(mid - cropOrigin - QPoint(80, 50), QSize(160, 100));
        preview.copy(zoom).scaled(QSize(zoom.width() * 2, zoom.height() * 2),
                                  Qt::KeepAspectRatio, Qt::FastTransformation)
            .save(previewPath + QStringLiteral(".zoom.png"));
        overlay.m_anns[0].a = origin + QPoint(100, 270);
        overlay.m_anns[0].b = origin + QPoint(620, 60);
        overlay.update();
        overlay.cropped().toImage().save(previewPath + QStringLiteral(".diagonal.png"));
    }
    std::puts("PASS: live arrow text, font, size, B/I, directions, text palette, cancel, undo and output");
    return 0;
}
