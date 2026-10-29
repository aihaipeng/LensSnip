#include "capture/capture_overlay.h"
#include <QWindow>
#include "platform/output.h"
#include "tests/test_support.h"
#include "shared/widgets.h"
#include "pin/pin_window.h"
#include "longshot/long_capture.h"
#include "text/cards/ocr_dialog.h"
#include <QGuiApplication>
#include <QImage>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QIcon>
#include <QColor>
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QtMath>
#include <QToolButton>
#include <QLineEdit>
#include <QClipboard>
#include <QCursor>
#include <QTimer>
#include <QFontMetrics>
#include <QMimeData>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QBoxLayout>
#include <QHelpEvent>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>

int CaptureOverlay::testColorPicker(const QString &previewPath)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    auto *saved = new QMimeData;
    if (const QMimeData *data = clipboard->mimeData())
        for (const QString &format : data->formats()) saved->setData(format, data->data(format));
    auto check = [clipboard, saved](bool ok, const char *message) {
        if (!ok) {
            clipboard->setMimeData(saved);
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    };
    QWidget calibration(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    calibration.setGeometry(QRect(QGuiApplication::primaryScreen()->geometry().center(), QSize(64, 64)));
    calibration.setStyleSheet("background:#010409;");
    calibration.show();
    QEventLoop settle;
    QTimer::singleShot(200, &settle, &QEventLoop::quit);
    settle.exec();
    const QVector<ScreenFrame> liveFrames = freezeScreens();
    POINT native{32, 32};
    check(ClientToScreen(reinterpret_cast<HWND>(calibration.winId()), &native), "native calibration coordinates");
    PixelInspector probe(nullptr);
    bool sampled = false;
    for (const ScreenFrame &live : liveFrames)
        if (probe.setSample(live, QPoint(native.x, native.y))) sampled = true;
    check(sampled && probe.colorText() == QStringLiteral("1 , 4 , 9"), "live desktop capture preserves native calibration pixels");
    {
        CaptureOverlay liveOverlay(liveFrames);
        liveOverlay.setAttribute(Qt::WA_DeleteOnClose, false);
        RECT actual{};
        GetWindowRect(reinterpret_cast<HWND>(liveOverlay.winId()), &actual);
        QRect desktop;
        for (const GdiDisplay &display : gdiDisplays()) desktop |= display.native;
        check(QRect(actual.left, actual.top, actual.right - actual.left, actual.bottom - actual.top) == desktop,
              "the visible capture overlay covers the entire native desktop at every scale");
        const QRect before = liveOverlay.geometry();
        const QImage frozen = liveOverlay.m_shot.toImage();
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        check(liveOverlay.geometry() == before && liveOverlay.m_shot.toImage() == frozen,
              "processing window events cannot move the overlay relative to the frozen pixels");
        for (const ScreenFrame &captured : liveFrames) {
            const QPointF local = liveOverlay.windowHandle()->mapFromGlobal(QPointF(captured.logicalRect.topLeft()));
            const qreal ratio = liveOverlay.m_shot.devicePixelRatioF();
            const QPoint pixel(qRound(local.x() * ratio), qRound(local.y() * ratio));
            check(liveOverlay.rect().contains(local.toPoint()) && pixel.x() >= 0 && pixel.y() >= 0,
                   "every captured screen starts inside the overlay, including negative-origin screens");
        }
        const QPoint savedCursor = QCursor::pos();
        const QPoint center = calibration.geometry().center();
        auto moveCursor = [&check](const QPoint &global) {
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            check(foreground == GetCurrentProcessId(), "native fixture input requires this process to own the foreground");
            QCursor::setPos(global);
        };
        auto mouse = [&liveOverlay, &moveCursor](QEvent::Type type, const QPoint &global) {
            moveCursor(global);
            const QPoint local = liveOverlay.mapFromGlobal(global);
            QMouseEvent event(type, local, global,
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(&liveOverlay, &event);
        };
        if (qApp->arguments().contains(QStringLiteral("--wait-for-foreground"))) {
            liveOverlay.setWindowTitle(QStringLiteral("LensSnip UI regression"));
            QElapsedTimer wait; wait.start();
            while (wait.elapsed() < 10000) {
                DWORD foreground = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
                if (foreground == GetCurrentProcessId()) break;
                QEventLoop settle;
                QTimer::singleShot(50, &settle, &QEventLoop::quit);
                settle.exec(QEventLoop::ExcludeUserInputEvents);
            }
        }
        moveCursor(center);
        const QPoint local = liveOverlay.mapFromGlobal(center);
        liveOverlay.m_sel = QRect(local - QPoint(60, 40), QSize(180, 120));
        liveOverlay.m_mode = Adjusting;
        liveOverlay.updateInspector();
        check(liveOverlay.m_inspector->isVisible() && liveOverlay.m_inspector->colorText() == QStringLiteral("1 , 4 , 9"),
              "a locked selection immediately permits sampling the frozen native pixel");
        QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier);
        QApplication::sendEvent(&liveOverlay, &copy);
        check(clipboard->text() == QStringLiteral("1 , 4 , 9") && liveOverlay.isVisible(),
              "C copies a color after selection without copying or closing the image");
        mouse(QEvent::MouseButtonPress, center);
        check(!liveOverlay.m_inspector->isVisible(), "moving the entire region hides the card on press");
        mouse(QEvent::MouseMove, center + QPoint(20, 15));
        check(!liveOverlay.m_inspector->isVisible(), "a held region move keeps the card hidden, including while stationary");
        mouse(QEvent::MouseButtonRelease, center + QPoint(20, 15));
        check(liveOverlay.m_inspector->isVisible(), "region release restores the card without another mouse move");
        const QPoint corner = liveOverlay.mapToGlobal(liveOverlay.m_sel.bottomRight());
        mouse(QEvent::MouseButtonPress, corner);
        mouse(QEvent::MouseMove, corner + QPoint(30, 20));
        check(liveOverlay.m_inspector->isVisible(), "resizing keeps the magnifier available for pixel precision");
        mouse(QEvent::MouseButtonRelease, corner + QPoint(30, 20));
        check(liveOverlay.m_inspector->isVisible(), "resize release leaves the card visible");
        moveCursor(center + QPoint(400, 300));
        liveOverlay.updateInspector();
        check(!liveOverlay.m_inspector->isVisible(), "idle sampling outside the locked selection is hidden");
        moveCursor(center + QPoint(20, 15));
        liveOverlay.selectTool(ATool::Rect);
        check(!liveOverlay.m_inspector->isVisible(), "arming an annotation tool suppresses automatic sampling");
        moveCursor(center + QPoint(20, 15));
        check(liveOverlay.m_sel.contains(liveOverlay.mapFromGlobal(QCursor::pos())), "tool restoration starts with the cursor inside the region");
        liveOverlay.setToolNone();
        if (!liveOverlay.m_inspector->isVisible())
            std::fprintf(stderr, "inspector restore: cursor=%d,%d local=%d,%d selection=%d,%d,%d,%d drag=%d item=%d tool=%d selected=%d visible=%d\n",
                         QCursor::pos().x(), QCursor::pos().y(), liveOverlay.mapFromGlobal(QCursor::pos()).x(), liveOverlay.mapFromGlobal(QCursor::pos()).y(),
                         liveOverlay.m_sel.x(), liveOverlay.m_sel.y(), liveOverlay.m_sel.width(), liveOverlay.m_sel.height(),
                         int(liveOverlay.m_drag), int(liveOverlay.m_itemDrag), int(liveOverlay.m_atool), liveOverlay.m_selAnn, int(liveOverlay.isVisible()));
        check(liveOverlay.m_inspector->isVisible(), "leaving the annotation tool restores automatic sampling");
        check(liveOverlay.m_shot.toImage() == frozen, "sampling and region adjustments never change frozen pixels");
        moveCursor(savedCursor);
        liveOverlay.close();
    }
    calibration.hide();
    QImage image(240, 180, QImage::Format_RGB32);
    image.fill(QColor(1, 4, 9));
    image.setPixelColor(112, 86, QColor(90, 150, 210));
    ScreenFrame frame{QRect(QGuiApplication::primaryScreen()->geometry().topLeft(), QSize(160, 120)),
                      QRect(-320, -240, 240, 180), image};
    CaptureOverlay overlay({frame});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    check(!overlay.m_undoButton->isEnabled() && !overlay.m_redoButton->isEnabled(),
          "a fresh capture greys out undo and redo before any edit");
    PixelInspector *inspector = overlay.m_inspector;
    const QPoint sample = frame.pixelRect.topLeft() + QPoint(120, 90);
    check(inspector->setSample(frame, sample), "native pixel sample with negative screen origin");
    inspector->show();
    check(inspector->colorText() == QStringLiteral("1 , 4 , 9"), "RGB channel order and formatting");
    for (const QSize logical : {QSize(240, 180), QSize(160, 120), QSize(120, 90)}) {
        frame.logicalRect.setSize(logical);
        check(inspector->setSample(frame, sample) && inspector->colorText() == QStringLiteral("1 , 4 , 9"),
              "100/150/200 percent scaling must not resample native colors");
    }
    QImage panel(inspector->size(), QImage::Format_RGB32);
    panel.fill(Qt::black);
    inspector->render(&panel);
    check(inspector->size() == QSize(152, 225), "the compact card uses a 19 by 13 sample at eightfold magnification");
    check(panel.pixelColor(75, 51) == QColor(1, 4, 9), "highlighted pixel retains its raw color");
    check(panel.pixelColor(11, 19) == QColor(90, 150, 210), "nearest-neighbor magnifier preserves neighboring pixels");
    check(panel.pixelColor(0, 0) == QColor(Qt::white) && panel.pixelColor(151, 0) == QColor(Qt::white)
              && panel.pixelColor(0, 103) == QColor(Qt::white) && panel.pixelColor(151, 103) == QColor(Qt::white),
          "the magnifier carries a 1px white hairline on every side");
    check(panel.pixelColor(1, 20) == QColor(1, 4, 9), "the magnifier starts at the card edge without black margins");
    int inkLeft = -1, inkRight = -1;
    for (int x = 0; x < 152; ++x) {
        if (panel.pixelColor(x, 120).lightness() > 60) {
            if (inkLeft < 0) inkLeft = x;
            inkRight = x;
        }
    }
    check(inkLeft >= 0 && qAbs(inkLeft + inkRight - 151) <= 4, "coordinate text is horizontally centered");
    check(inspector->coordinateText() == QStringLiteral("( -200 , -150 )"), "global native coordinates are displayed");
    QImage translucent(inspector->size(), QImage::Format_ARGB32_Premultiplied);
    translucent.fill(Qt::transparent);
    inspector->render(&translucent, QPoint(), QRegion(), QWidget::DrawChildren);
    const QColor tint = translucent.pixelColor(1, 170);
    check(tint.alpha() > 120 && tint.alpha() < 200 && tint.red() < 16,
          "the information panel is semi-transparent black");
    check(translucent.pixelColor(1, 1) == QColor(1, 4, 9), "no transparent chrome is left around the magnifier");
    check(translucent.pixelColor(75, 51) == QColor(1, 4, 9), "the center color retains its original opaque pixels");
    QKeyEvent toggleP(QEvent::KeyPress, Qt::Key_P, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &toggleP);
    check(inspector->relativeCoordinates() && inspector->coordinateText() == QStringLiteral("( 120 , 90 )"),
          "P switches to screen-relative coordinates");
    QApplication::sendEvent(&overlay, &toggleP);
    check(!inspector->relativeCoordinates() && inspector->coordinateText() == QStringLiteral("( -200 , -150 )"),
          "P switches back to global coordinates");
    check(inspector->setSample(frame, sample, QRectF(110, 84, 30, 20)), "a selection origin is available for relative coordinates");
    QApplication::sendEvent(&overlay, &toggleP);
    check(inspector->coordinateText() == QStringLiteral("( 10 , 6 )"),
          "relative coordinates are measured from the selection origin");
    QApplication::sendEvent(&overlay, &toggleP);
    check(inspector->setSample(frame, sample, QRectF(0, 0, 121, 91)), "a magnifier may preview the selection boundary");
    inspector->render(&panel);
    check(panel.pixelColor(115, 70) != QColor(1, 4, 9)
              && inspector->colorText() == QStringLiteral("1 , 4 , 9"),
          "the magnified mask is visual only and never replaces the raw copied color");
    check(inspector->setSample(frame, sample), "restore the unmasked sample preview");
    inspector->render(&panel);
    if (!previewPath.isEmpty()) check(panel.save(previewPath), "save magnifier preview");
    const QImage frozen = overlay.m_shot.toImage();
    check(overlay.m_shot.devicePixelRatio() == 1.5, "frozen desktop preserves high-DPI capture resolution");
    const QPointF localOrigin = overlay.mapFromGlobal(QPointF(frame.logicalRect.topLeft()));
    const QPoint origin(qRound(localOrigin.x() * 1.5), qRound(localOrigin.y() * 1.5));
    check(frozen.pixelColor(origin + QPoint(112, 86)) == QColor(90, 150, 210), "frozen capture retains native pixel detail");
    QKeyEvent copyRgb(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &copyRgb);
    check(clipboard->text() == QStringLiteral("1 , 4 , 9") && overlay.isVisible(), "C copies RGB without closing capture");
    QKeyEvent shift(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    QApplication::sendEvent(&overlay, &shift);
    check(inspector->colorText() == QStringLiteral("#010409"), "Shift switches to six-digit HEX");
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, QString(), true);
    QApplication::sendEvent(&overlay, &repeat);
    check(inspector->colorText() == QStringLiteral("#010409"), "holding Shift must not repeatedly toggle formats");
    QApplication::sendEvent(&overlay, &copyRgb);
    check(clipboard->text() == QStringLiteral("#010409"), "C copies the displayed HEX format");
    QApplication::sendEvent(&overlay, &shift);
    check(inspector->colorText() == QStringLiteral("1 , 4 , 9"), "Shift switches back to RGB");
    for (const QPoint pixel : {QPoint(0, 0), QPoint(239, 0), QPoint(0, 179), QPoint(239, 179)})
        check(inspector->setSample(frame, frame.pixelRect.topLeft() + pixel), "screen-edge samples remain valid");
    check(!inspector->setSample(frame, frame.pixelRect.topLeft() - QPoint(1, 0)), "outside-screen sample is rejected");
    check(!inspector->setSample(ScreenFrame{}, sample), "empty frame is rejected");
    ScreenFrame mismatch = frame;
    mismatch.pixelRect.setWidth(241);
    check(!inspector->setSample(mismatch, sample), "unexpected native size is rejected");
    const QRect bounds(-640, -100, 640, 480);
    for (const QPoint cursor : {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()}) {
        const QRect positioned(inspectorPosition(cursor, inspector->size(), bounds), inspector->size());
        check(bounds.contains(positioned) && !positioned.contains(cursor), "panel stays on screen and avoids the cursor");
    }
    check(overlay.m_shot.toImage() == frozen, "magnifier and format changes must not alter capture pixels");
    // Creating, moving and resizing a selection track the cursor exactly (no magnetic snapping).
    const QRect screenGeo = QGuiApplication::primaryScreen()->geometry();
    const QPoint screenOrigin = screenGeo.topLeft() - overlay.geometry().topLeft();
    const auto mouseMove = [&overlay](const QPoint &local) {
        QMouseEvent move(QEvent::MouseMove, local, local + overlay.geometry().topLeft(),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &move);
    };
    overlay.m_pressPos = screenOrigin + QPoint(40, 40);
    overlay.m_drag = H_New;
    overlay.m_mode = Selecting;
    mouseMove(screenOrigin + QPoint(screenGeo.width() - 6, 120));
    check(overlay.m_sel == QRect(screenOrigin + QPoint(40, 40), screenOrigin + QPoint(screenGeo.width() - 6, 120)),
          "a new selection follows the cursor without snapping");
    overlay.m_sel = QRect(screenOrigin + QPoint(20, 20), QSize(60, 40));
    overlay.m_pressSel = overlay.m_sel;          // H_Move diffs against the press snapshot.
    overlay.m_pressPos = screenOrigin + QPoint(50, 40);
    overlay.m_drag = H_Move;
    overlay.m_mode = Adjusting;
    mouseMove(screenOrigin + QPoint(38, 40));   // 8px from the screen border: no pull.
    check(overlay.m_sel == QRect(screenOrigin + QPoint(8, 20), QSize(60, 40)),
          "moving the selection follows the cursor without snapping");
    overlay.m_pressSel = overlay.m_sel;
    overlay.m_pressPos = screenOrigin + QPoint(38, 40);
    overlay.m_drag = H_R;
    for (int x : {screenGeo.width() - 3, screenGeo.width() - 9, 200}) {
        mouseMove(screenOrigin + QPoint(x, 40));
        check(overlay.m_sel.right() == screenOrigin.x() + x, "resizing tracks the cursor pixel for pixel");
    }
    // A fresh capture selects a detected window; dragging overrides that window hit.
    overlay.resetSelection();
    overlay.m_detectionOptions.elements = true;
    overlay.m_windowRects = { DetectedWindow{QRect(screenOrigin + QPoint(120, 60), QSize(80, 50)),
                                             {QRect(screenOrigin + QPoint(140, 70), QSize(30, 20))}} };
    const auto mouseClick = [&overlay](const QPoint &local) {
        QMouseEvent press(QEvent::MouseButtonPress, local, local + overlay.geometry().topLeft(),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, local + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &press);
        QApplication::sendEvent(&overlay, &release);
    };
    mouseClick(screenOrigin + QPoint(150, 80));
    check(overlay.m_sel == overlay.m_windowRects.first().nativeChildren.first() && overlay.m_mode == Adjusting,
          "clicking inside a detected child area selects the finer child rect");
    overlay.m_mode = Selecting;                  // Fresh state: a drag overrides the window hit.
    QMouseEvent dragPress(QEvent::MouseButtonPress, screenOrigin + QPoint(150, 80),
                          screenOrigin + QPoint(150, 80) + overlay.geometry().topLeft(),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &dragPress);
    mouseMove(screenOrigin + QPoint(165, 95));   // Drag before release overrides the click.
    QMouseEvent dragRelease(QEvent::MouseButtonRelease, screenOrigin + QPoint(165, 95),
                            screenOrigin + QPoint(165, 95) + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &dragRelease);
    check(overlay.m_sel == QRect(screenOrigin + QPoint(150, 80), QSize(16, 16)),
          "dragging from inside a window starts a new selection");
    overlay.m_sel = QRect(screenOrigin + QPoint(60, 60), QSize(40, 50));
    overlay.m_pressSel = overlay.m_sel;
    overlay.m_pressPos = screenOrigin + QPoint(100, 70);
    overlay.m_drag = H_R;
    overlay.m_mode = Adjusting;
    mouseMove(screenOrigin + QPoint(196, 70));   // 4px short of the window's right edge (200).
    check(overlay.m_sel.right() == screenOrigin.x() + 196,
          "resizing near a window edge is not pulled onto it");
    // Hover: the mask hole follows the detected window before any selection exists.
    overlay.m_sel = QRect();
    overlay.m_mode = Selecting;
    overlay.m_drag = H_None;
    mouseMove(screenOrigin + QPoint(150, 80));   // Inside the child rect.
    check(overlay.m_hover == overlay.m_windowRects.first().nativeChildren.first() && overlay.m_sizeTip->isVisible(),
          "hovering a child area highlights the finer child rect and shows its size");
    mouseMove(screenOrigin + QPoint(125, 100));  // Inside the window, outside any child.
    check(overlay.m_hover == overlay.m_windowRects.first().native,
          "hover falls back to the whole window outside child areas");
    mouseMove(screenOrigin + QPoint(450, 300));  // Empty area away from the window.
    check(overlay.m_hover.isNull() && !overlay.m_sizeTip->isVisible(),
          "hover clears outside detected windows");
    // A committed selection stays put: hover re-detection waits until the selection is
    // cleared, so the highlight never chases the cursor after a click locks a target.
    overlay.m_sel = QRect(screenOrigin + QPoint(300, 200), QSize(50, 40));
    overlay.m_mode = Adjusting;
    overlay.m_drag = H_None;
    mouseMove(screenOrigin + QPoint(150, 80));   // Over the child, outside the selection.
    check(overlay.m_hover.isNull() && overlay.m_sel == QRect(screenOrigin + QPoint(300, 200), QSize(50, 40)),
          "a locked selection does not chase the cursor across detected areas");
    mouseMove(screenOrigin + QPoint(320, 210));  // Back inside the selection.
    check(overlay.m_hover.isNull(), "hover re-detection stays off inside the locked selection too");
    // Both rows stay horizontal and move above the selection when the screen bottom is full.
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry()
                           .translated(-overlay.geometry().topLeft());
    overlay.m_sel = QRect(avail.topLeft() + QPoint(60, 60), QSize(980, 120));
    overlay.m_mode = Adjusting;
    overlay.placeToolBars();
    check(overlay.m_tools->y() >= overlay.m_sel.bottom() + 1
              && overlay.m_tools->x() + overlay.m_tools->width() == overlay.m_sel.right() + 1
              && overlay.m_tools->width() > overlay.m_tools->height(),
          "toolbar docks below the selection, flush with its right edge");
    overlay.m_sel = QRect(avail.left() + 40, avail.bottom() + 1 - 150, 300, 150);
    overlay.placeToolBars();
    check(overlay.m_tools->y() + overlay.m_tools->height() <= overlay.m_sel.top()
              && overlay.m_tools->width() > overlay.m_tools->height(),
          "the horizontal toolbar moves above the selection at the screen bottom");
    // Annotation editing: shortcuts, shift constraints, selection, re-tune, delete,
    // undo/redo, marker, the text editor, sequence numbers, blur, and the eraser.
    auto mouse = [&overlay](QEvent::Type type, const QPoint &local,
                            Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent ev(type, local, local + overlay.geometry().topLeft(), Qt::LeftButton,
                       type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                       mods);
        QApplication::sendEvent(&overlay, &ev);
    };
    const QPoint annOrigin = screenOrigin + QPoint(20, 20);
    overlay.m_sel = QRect(annOrigin, QSize(110, 80));
    overlay.m_mode = Adjusting;
    QKeyEvent keyR(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &keyR);
    check(overlay.m_atool == ATool::Rect, "single-letter shortcuts arm annotation tools");
    const QPoint squareA = annOrigin + QPoint(10, 10);
    mouse(QEvent::MouseButtonPress, squareA);
    mouse(QEvent::MouseMove, squareA + QPoint(40, 20), Qt::ShiftModifier);
    mouse(QEvent::MouseButtonRelease, squareA + QPoint(40, 20), Qt::ShiftModifier);
    check(overlay.m_anns.size() == 1 && overlay.m_anns[0].rect.size() == QSize(41, 41),
          "shift constrains rectangles to squares");
    overlay.setToolNone();
    const QPoint edge(overlay.m_anns[0].rect.center().x(), overlay.m_anns[0].rect.top());
    mouse(QEvent::MouseButtonPress, edge);
    check(overlay.m_selAnn == 0 && overlay.m_itemDrag == H_Move,
          "clicking an annotation selects it");
    mouse(QEvent::MouseMove, edge + QPoint(12, 0));
    mouse(QEvent::MouseButtonRelease, edge + QPoint(12, 0));
    check(overlay.m_anns[0].rect == QRect(squareA + QPoint(12, 0), QSize(41, 41)),
          "dragging moves the selected annotation");
    const QPoint corner = overlay.m_anns[0].rect.bottomRight();
    mouse(QEvent::MouseButtonPress, corner);
    mouse(QEvent::MouseMove, corner + QPoint(25, 5));
    mouse(QEvent::MouseButtonRelease, corner + QPoint(25, 5));
    check(overlay.m_anns[0].rect.size() == QSize(66, 46),
          "corner handles resize the selected annotation");
    const QPoint wheelAt = overlay.m_sel.center();
    QWheelEvent tune(QPointF(wheelAt), QPointF(wheelAt + overlay.geometry().topLeft()),
                     QPoint(0, 8), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                     Qt::ScrollUpdate, false);
    QApplication::sendEvent(&overlay, &tune);
    check(overlay.m_anns[0].pen == 4, "the wheel retunes the selected annotation");
    const QColor originalShapeColor = overlay.m_anns[0].color;
    const int beforeColorUndo = overlay.m_undoStack.size();
    overlay.updateToolOptions();
    check(overlay.m_colorBoard != nullptr, "a selected shape shows the color board");
    auto clickBoard = [&overlay](const QPoint &at) {
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, QPointF(at), QPointF(overlay.m_colorBoard->mapToGlobal(at)), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(overlay.m_colorBoard, &event);
        }
        QApplication::processEvents();
    };
    clickBoard(ColorBoard::swatchRect(5).center());
    check(overlay.m_anns[0].color == QColor("#00a2e8") && overlay.m_undoStack.size() == beforeColorUndo + 1
              && overlay.m_acolor == QColor("#00a2e8") && overlay.m_colorBoard->currentColor() == QColor("#00a2e8"),
          "a preset click recolors the selected annotation as one undo step");
    overlay.undoAnnotation();
    check(overlay.m_anns[0].color == originalShapeColor && overlay.m_acolor == QColor("#00a2e8")
              && overlay.m_colorBoard && overlay.m_colorBoard->currentColor() == QColor("#00a2e8"),
          "undo restores the object color while the current color stays");
    overlay.m_selAnn = 0;
    overlay.updateToolOptions();
    const int beforeDialogUndo = overlay.m_undoStack.size();
    clickBoard(overlay.m_colorBoard->currentRect().center());
    QColorDialog *colorDialog = overlay.m_colorDialog;
    check(colorDialog && colorDialog->isVisible() && colorDialog->isModal()
              && colorDialog->windowTitle() == QStringLiteral("选择颜色")
              && colorDialog->testOption(QColorDialog::ShowAlphaChannel)
              && colorDialog->testOption(QColorDialog::DontUseNativeDialog)
              && colorDialog->currentColor() == QColor("#00a2e8"),
          "the current color square opens the standard color dialog with the current color");
    {
        QScreen *current = QGuiApplication::screenAt(QCursor::pos());
        if (!current) current = QGuiApplication::primaryScreen();
        const QRect available = current->availableGeometry();
        const QPoint expected = available.center() - QRect(QPoint(), colorDialog->size()).center();
        check(colorDialog->pos() == expected && available.contains(colorDialog->frameGeometry()),
              "the color dialog opens centered on the current screen");
    }
    colorDialog->setCurrentColor(QColor("#123abc"));
    check(overlay.m_anns[0].color == QColor("#123abc") && overlay.m_colorBoard->currentColor() == QColor("#123abc")
              && overlay.m_undoStack.size() == beforeDialogUndo,
          "dialog color changes preview live on the selection without undo entries");
    colorDialog->reject();
    QApplication::processEvents();
    check(!overlay.m_colorDialog && overlay.m_anns[0].color == originalShapeColor
              && overlay.m_acolor == QColor("#00a2e8") && overlay.m_colorBoard->currentColor() == QColor("#00a2e8")
              && overlay.m_undoStack.size() == beforeDialogUndo,
          "cancel restores the previous colors with no undo entry");
    clickBoard(overlay.m_colorBoard->currentRect().center());
    colorDialog = overlay.m_colorDialog;
    check(colorDialog != nullptr, "the color dialog reopens after cancel");
    colorDialog->setCurrentColor(QColor("#20406080"));
    colorDialog->setCurrentColor(QColor("#123abc"));
    const QColor customBefore = QColorDialog::customColor(0);
    colorDialog->accept();
    QApplication::processEvents();
    check(!overlay.m_colorDialog && overlay.m_anns[0].color == QColor("#123abc") && overlay.m_acolor == QColor("#123abc")
              && overlay.m_colorBoard->currentColor() == QColor("#123abc")
              && overlay.m_undoStack.size() == beforeDialogUndo + 1
              && QColorDialog::customColor(0) == customBefore,
          "OK commits the dialog color as one undo step without touching the custom colors");
    overlay.undoAnnotation();
    check(overlay.m_anns[0].color == originalShapeColor, "undo restores the color before the dialog");
    overlay.m_selAnn = 0;
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &del);
    check(overlay.m_anns.isEmpty(), "delete removes the selected annotation");
    overlay.undoAnnotation();
    check(overlay.m_anns.size() == 1, "undo restores the deleted annotation");
    overlay.redoAnnotation();
    check(overlay.m_anns.isEmpty(), "redo replays the deletion");
    overlay.selectTool(ATool::Marker);
    const QPoint strokeAt = annOrigin + QPoint(12, 40);
    mouse(QEvent::MouseButtonPress, strokeAt);
    mouse(QEvent::MouseMove, strokeAt + QPoint(15, 5));
    mouse(QEvent::MouseMove, strokeAt + QPoint(18, 18));
    mouse(QEvent::MouseButtonRelease, strokeAt + QPoint(18, 18));
    check(overlay.m_anns.size() == 1 && overlay.m_anns[0].tool == ATool::Marker
              && overlay.m_anns[0].stroke.isEmpty() && overlay.m_anns[0].a == strokeAt
              && overlay.m_anns[0].b == strokeAt + QPoint(18, 18),
          "the marker ignores intermediate samples and commits one straight translucent line");
    const QImage beforeText = overlay.cropped().toImage();
    overlay.selectTool(ATool::Text);
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 20));
    check(overlay.editorActive(), "the text tool opens an inline editor");
    overlay.m_editor->setPlainText(QStringLiteral("标注"));
    overlay.commitTextEdit();
    check(overlay.m_anns.size() == 2 && overlay.m_anns[1].tool == ATool::Text
              && overlay.m_anns[1].text == QStringLiteral("标注")
              && overlay.m_anns[1].font == overlay.m_afont,
          "text commits into an annotation");
    check(overlay.cropped().toImage() != beforeText, "text composes into the capture");
    overlay.setToolNone();                       // Re-editing works in selection mode.
    mouse(QEvent::MouseButtonDblClick, overlay.m_anns[1].rect.center());
    check(overlay.editorActive()
              && overlay.m_editor->toPlainText() == QStringLiteral("标注"),
          "double-click reopens a text annotation for editing");
    overlay.m_editor->setPlainText(QStringLiteral("改"));
    overlay.commitTextEdit();
    check(overlay.m_anns.size() == 2 && overlay.m_anns[1].text == QStringLiteral("改"),
          "re-editing replaces the annotation in place");
    overlay.selectTool(ATool::Number);
    const QPoint n1 = annOrigin + QPoint(15, 15), n2 = annOrigin + QPoint(70, 50);
    mouse(QEvent::MouseButtonPress, n1);
    mouse(QEvent::MouseButtonRelease, n1);
    mouse(QEvent::MouseButtonPress, n2);
    mouse(QEvent::MouseButtonRelease, n2);
    check(overlay.m_anns.size() == 4 && overlay.m_anns[2].text == QStringLiteral("1")
              && overlay.m_anns[3].text == QStringLiteral("2"),
          "number clicks place incrementing sequence digits");
    {
        const QRect start = overlay.m_anns[3].rect;
        const qreal size = overlay.toolSize();
        for (int k = 0; k < 6; ++k) overlay.setToolSize(size + 0.5 * (k + 1));
        for (int k = 5; k >= 0; --k) overlay.setToolSize(size + 0.5 * k);
        check(overlay.m_anns[3].rect == start, "resizing a number keeps it in place");
    }
    overlay.setToolNone();
    mouse(QEvent::MouseButtonPress, n1);
    mouse(QEvent::MouseButtonRelease, n1);
    check(overlay.m_selAnn == -1, "a deselected number is locked and cannot be picked again");
    overlay.m_selAnn = 2;   // Fixture: delete the first number to check renumbering.
    QApplication::sendEvent(&overlay, &del);
    check(overlay.m_anns.size() == 3 && overlay.m_anns[2].text == QStringLiteral("1"),
          "numbers renumber after a deletion");
    const QImage beforeMosaic = overlay.cropped().toImage();
    overlay.selectTool(ATool::Mosaic);
    overlay.selectMosaicMode(false);
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(50, 10));
    mouse(QEvent::MouseMove, annOrigin + QPoint(100, 60));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(100, 60));
    check(overlay.m_anns.size() == 4 && overlay.m_anns[3].tool == ATool::Mosaic
              && overlay.cropped().toImage() != beforeMosaic,
          "mosaic pixelates the frozen frame in its rectangle");
    overlay.selectTool(ATool::Eraser);
    {
        const auto *strip = static_cast<QBoxLayout *>(overlay.m_toolOptions->layout());
        const auto *brushButton = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("eraserBrush"));
        check(!overlay.eraserRect() && strip->count() == 4 && brushButton && brushButton->isChecked()
                  && strip->itemAt(0)->widget() == overlay.m_sizeButton
                  && strip->itemAt(1)->widget() == brushButton
                  && strip->itemAt(2)->widget()->objectName() == QStringLiteral("eraserRectangle")
                  && strip->itemAt(3)->widget()->objectName() == QStringLiteral("eraserSmart")
                  && !overlay.m_colorBoard,
              "the eraser starts in brush mode: width, brush, rectangle, smart, no colors");
    }
    overlay.selectEraserMode(true);
    check(overlay.m_sizeButton != nullptr, "rectangle erasing keeps the width button");
    const QImage beforeErase = overlay.cropped().toImage();
    const QPoint erasePixel = (strokeAt + QPoint(9, 9) - overlay.m_sel.topLeft()) * overlay.m_shot.devicePixelRatio();
    const QImage blank = overlay.m_shot.toImage();
    const QPoint blankPixel = (strokeAt + QPoint(9, 9)) * overlay.m_shot.devicePixelRatio();
    const QRect frameArea(strokeAt + QPoint(4, 4), QSize(11, 11));
    const QImage beforeFrame = overlay.grab(frameArea).toImage();
    mouse(QEvent::MouseButtonPress, strokeAt + QPoint(4, 4));
    mouse(QEvent::MouseMove, strokeAt + QPoint(14, 14));
    check(overlay.grab(frameArea).toImage() != beforeFrame && overlay.m_live && overlay.m_ann.fill,
          "dragging a rectangle erasure draws a frame around the area");
    mouse(QEvent::MouseButtonRelease, strokeAt + QPoint(14, 14));
    const QImage afterRectErase = overlay.cropped().toImage();
    check(overlay.m_anns.size() == 5 && overlay.m_anns[0].tool == ATool::Marker
              && overlay.m_anns[4].tool == ATool::Eraser && overlay.m_anns[4].fill && overlay.m_selAnn == 4,
          "a rectangle erasure keeps the marker, adds an erasure object and selects it");
    check(afterRectErase.pixelColor(erasePixel) == blank.pixelColor(blankPixel)
              && beforeErase.pixelColor(erasePixel) != blank.pixelColor(blankPixel),
          "erased pixels show the capture again");
    const QRect erasedRect = overlay.m_anns[4].rect;
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(100, 15));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(100, 15));
    check(overlay.m_selAnn == -1 && overlay.annotationAt(strokeAt + QPoint(9, 9)) != 4,
          "clicking outside finishes a rectangle erasure");
    {
        QRect keptSel = overlay.m_sel;
        const int kept = overlay.m_anns.size();
        overlay.m_selAnn = 4;
        const QPoint outside = keptSel.bottomRight() + QPoint(60, 60);
        mouse(QEvent::MouseButtonPress, outside);
        mouse(QEvent::MouseButtonRelease, outside);
        const QRect expanded(QPoint(keptSel.left(), keptSel.top()), outside);
        check(overlay.m_sel == expanded && overlay.m_anns.size() == kept && overlay.m_selAnn == -1
                  && overlay.m_atool == ATool::Eraser,
              "an eraser click outside the capture expands the region to the pointer and keeps the erasure");
        keptSel = expanded;
        overlay.selectTool(ATool::Rect);
        mouse(QEvent::MouseButtonPress, keptSel.topLeft() + QPoint(60, 62));
        mouse(QEvent::MouseMove, keptSel.topLeft() + QPoint(100, 76));
        mouse(QEvent::MouseButtonRelease, keptSel.topLeft() + QPoint(100, 76));
        const int shape = overlay.m_anns.size() - 1;
        QToolButton *ellipseButton = nullptr;
        for (QToolButton *b : overlay.m_toolOptions->findChildren<QToolButton *>())
            if (b->accessibleName() == QStringLiteral("椭圆")) ellipseButton = b;
        check(overlay.m_anns[shape].tool == ATool::Rect && overlay.m_selAnn == shape && ellipseButton,
              "a drawn rectangle stays selected with the shape options shown");
        ellipseButton->click();
        check(overlay.m_anns.size() == kept + 1 && overlay.m_anns[shape].tool == ATool::Ellipse
                  && overlay.m_selAnn == shape && overlay.m_atool == ATool::Ellipse,
              "picking the ellipse turns the selected rectangle into an ellipse in place");
        const QPoint outside2 = keptSel.bottomRight() + QPoint(60, 60);
        mouse(QEvent::MouseButtonPress, outside2);
        mouse(QEvent::MouseButtonRelease, outside2);
        check(overlay.m_sel == QRect(keptSel.topLeft(), outside2) && overlay.m_selAnn == -1
                  && overlay.m_atool == ATool::Ellipse
                  && overlay.m_anns[shape].tool == ATool::Ellipse,
              "a shape-tool click outside the capture resizes the region and keeps the shape");
        overlay.undoAnnotation();
        check(overlay.m_anns[shape].tool == ATool::Rect, "undo restores the rectangle");
        overlay.selectTool(ATool::Rect);
        mouse(QEvent::MouseButtonPress, keptSel.topLeft() + QPoint(60, 62));   // Select it again by drawing anew.
        mouse(QEvent::MouseMove, keptSel.topLeft() + QPoint(100, 76));
        mouse(QEvent::MouseButtonRelease, keptSel.topLeft() + QPoint(100, 76));
        const int fresh = overlay.m_anns.size() - 1;
        const bool freshFill = overlay.m_anns[fresh].fill;
        const QPoint blankInside = keptSel.topLeft() + QPoint(10, 10);   // Inside the capture, off the shape.
        mouse(QEvent::MouseButtonPress, blankInside);
        mouse(QEvent::MouseButtonRelease, blankInside);
        check(overlay.m_selAnn == -1 && overlay.m_anns.size() == fresh + 1,
              "a click on blank capture area releases the shape without drawing");
        for (QToolButton *b : overlay.m_toolOptions->findChildren<QToolButton *>())
            if (b->accessibleName() == QStringLiteral("椭圆")) ellipseButton = b;
        ellipseButton->click();
        overlay.toggleFill();
        check(overlay.m_anns[fresh].tool == ATool::Rect && overlay.m_anns[fresh].fill == freshFill
                  && overlay.m_atool == ATool::Ellipse,
              "after releasing, shape and fill picks only set up the next shape");
        overlay.toggleFill();
        overlay.undoAnnotation();
        overlay.undoAnnotation();
        check(overlay.m_anns.size() == kept, "undo removes the shapes");
        overlay.selectEraserMode(true);
    }
    const int finished = overlay.m_anns.size();
    mouse(QEvent::MouseButtonPress, strokeAt + QPoint(9, 9));
    mouse(QEvent::MouseMove, strokeAt + QPoint(20, 20));
    mouse(QEvent::MouseButtonRelease, strokeAt + QPoint(20, 20));
    check(overlay.m_anns[4].rect == erasedRect && overlay.m_anns.size() == finished + 1,
          "dragging inside a finished erasure starts a new one instead of moving it");
    while (overlay.m_anns.size() > 5) overlay.undoAnnotation();
    overlay.undoAnnotation();
    check(overlay.m_anns.size() == 4 && overlay.cropped().toImage() == beforeErase,
          "undo restores the erased pixels");
    overlay.selectEraserMode(false);
    overlay.setToolSize(99);
    check(overlay.toolSize() == 50 && overlay.m_sizeButton && overlay.m_sizeButton->text() == QStringLiteral("50"),
          "the eraser brush width stops at 50 and shows on its button");
    overlay.setToolSize(0);
    check(overlay.toolSize() == 1, "the eraser brush width stops at 1");
    overlay.setToolSize(14);
    mouse(QEvent::MouseButtonPress, strokeAt + QPoint(9, 9));
    mouse(QEvent::MouseButtonRelease, strokeAt + QPoint(9, 9));
    check(overlay.m_anns.size() == 5 && !overlay.m_anns[4].fill && overlay.m_selAnn == -1
              && overlay.cropped().toImage().pixelColor(erasePixel) == blank.pixelColor(blankPixel),
          "a brush click erases a round dot and leaves nothing selected");
    overlay.selectEraserMode(true);
    overlay.undoAnnotation();
    check(overlay.m_anns.size() == 4, "undo removes the brush erasure");
    overlay.m_anns.removeAt(0);                  // Later checks expect the marker gone, as before.
    overlay.m_selAnn = -1;
    overlay.setToolNone();
    overlay.m_selAnn = overlay.m_anns.size() - 1;   // Fixture: a still-selected fresh item.
    QKeyEvent escSel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &escSel);
    check(overlay.m_selAnn == -1 && overlay.isVisible(),
          "escape deselects the annotation before closing");
    // Behavior parity round: right click finishes input, +/- width, WASD nudge,
    // removed rotation controls, and undo/redo button enable state.
    const int annCount = overlay.m_anns.size();
    overlay.selectTool(ATool::Marker);
    const QPoint fin = annOrigin + QPoint(16, 12);
    mouse(QEvent::MouseButtonPress, fin);
    mouse(QEvent::MouseMove, fin + QPoint(14, 6));
    QMouseEvent rightClick(QEvent::MouseButtonPress, fin + QPoint(14, 6),
                           fin + QPoint(14, 6) + overlay.geometry().topLeft(),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &rightClick);
    check(overlay.m_anns.size() == annCount + 1
              && overlay.m_anns.last().tool == ATool::Marker && !overlay.m_live,
          "right click finishes a live stroke instead of discarding it");
    QKeyEvent plus(QEvent::KeyPress, Qt::Key_Plus, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &plus);
    check(overlay.m_anns.last().pen == 4, "the plus key retunes the selected annotation");
    overlay.setToolNone();
    mouse(QEvent::MouseButtonPress, fin + QPoint(7, 3));   // Re-select the stroke.
    mouse(QEvent::MouseButtonRelease, fin + QPoint(7, 3));
    const QPoint beforeNudge = overlay.m_anns.last().a;
    QKeyEvent keyA(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &keyA);
    QKeyEvent keyW(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &keyW);
    check(overlay.m_anns.last().a == beforeNudge + QPoint(-1, -1),
          "WASD keys nudge the selected annotation one pixel");
    const QRect markerBounds = CaptureOverlay::annotationRect(overlay.m_anns.last());
    check(overlay.annHandleAt(overlay.m_anns.last(), QPoint(markerBounds.center().x(), markerBounds.bottom() + 22)) == H_None,
          "the removed rotate handle has no invisible hit target");
    const QPoint inside = CaptureOverlay::annotationRect(overlay.m_anns.last()).center()
                          + QPoint(2, 3);
    mouse(QEvent::MouseButtonPress, inside);
    check(overlay.m_selAnn == overlay.m_anns.size() - 1,
          "annotations remain selectable without a rotation control");
    mouse(QEvent::MouseButtonRelease, inside);
    QKeyEvent keyDel(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &keyDel);
    while (!overlay.m_undoStack.isEmpty()) overlay.undoAnnotation();
    check(!overlay.m_undoButton->isEnabled(), "the undo button disables on an empty stack");
    check(overlay.m_redoButton->isEnabled(), "undone edits light up the redo button");
    overlay.redoAnnotation();
    check(overlay.m_undoButton->isEnabled(), "a redone edit lights up the undo button");
    while (!overlay.m_undoStack.isEmpty()) overlay.undoAnnotation();
    overlay.m_redoStack.clear();
    overlay.syncUndoButtons();
    check(!overlay.m_redoButton->isEnabled(), "the redo button disables on an empty stack");
    overlay.selectTool(ATool::Rect);
    QKeyEvent fillKey(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &fillKey);
    check(overlay.m_afill, "the F key toggles fill mode for the shape tools");
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 20));
    mouse(QEvent::MouseMove, annOrigin + QPoint(95, 55));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(95, 55));
    check(overlay.m_anns.last().fill, "fill mode carries into fresh shapes");
    {
        QImage solid(140, 110, QImage::Format_ARGB32);
        solid.fill(Qt::transparent);
        QPainter painter(&solid);
        overlay.drawAnnotation(painter, overlay.m_anns.last(), QImage());
        painter.end();
        const QPoint inside = overlay.m_anns.last().rect.center();
        check(solid.pixelColor(inside).alpha() == 255, "filled shapes are solid, not a translucent wash");
    }
    overlay.m_afill = false;
    overlay.selectTool(ATool::Marker);
    auto *markerBrush = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("markerBrush"));
    auto *markerRect = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("markerRectangle"));
    auto *markerCircle = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("markerEllipse"));
    check(markerBrush && markerRect && markerCircle && markerBrush->isChecked()
              && !markerRect->isChecked() && !markerCircle->isChecked() && overlay.m_sizeButton,
          "the marker strip starts on its brush with three exclusive modes and a width control");
    markerRect->click();
    markerRect = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("markerRectangle"));
    check(overlay.m_markerFill && !overlay.m_markerEllipse && markerRect && markerRect->isChecked()
              && overlay.m_sizeButton,
          "rectangle highlights select in place and keep the width control visible");
    const int markerWidth = overlay.m_toolPens[int(ATool::Marker)];
    const QPointF markerAt = annOrigin + QPoint(30, 20);
    QWheelEvent areaWheel(markerAt, QPointF(overlay.mapToGlobal(markerAt.toPoint())),
                          QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollUpdate, false);
    QApplication::sendEvent(&overlay, &areaWheel);
    check(overlay.m_toolPens[int(ATool::Marker)] == markerWidth && overlay.m_sizeButton,
          "the wheel does not change thickness in rectangle-highlight mode");
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(10, 10));
    mouse(QEvent::MouseMove, annOrigin + QPoint(70, 30));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(70, 30));
    check(overlay.m_anns.last().tool == ATool::Marker && overlay.m_anns.last().fill
              && !overlay.m_anns.last().markerEllipse
              && overlay.m_anns.last().rect == QRect(annOrigin + QPoint(10, 10), annOrigin + QPoint(70, 30)).normalized()
              && overlay.m_anns.last().stroke.isEmpty(),
          "marker rectangle drags a highlight block instead of a stroke");
    {
        const QRect area = overlay.m_anns.last().rect;
        QImage block(area.width() + 20, area.height() + 20, QImage::Format_ARGB32);
        block.fill(Qt::transparent);
        QPainter painter(&block);
        painter.translate(QPoint(10, 10) - area.topLeft());   // Overlay coords onto the small canvas.
        overlay.drawAnnotation(painter, overlay.m_anns.last(), QImage());
        painter.end();
        const int alpha = block.pixelColor(block.rect().center()).alpha();
        check(alpha > 60 && alpha < 200 && block.pixelColor(11, 11).alpha() > 60,
              "the highlight rectangle is translucent and keeps square corners");
    }
    markerCircle = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("markerEllipse"));
    check(markerCircle != nullptr, "the marker strip exposes a circle button");
    if (markerCircle) markerCircle->click();
    markerCircle = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("markerEllipse"));
    check(overlay.m_markerFill && overlay.m_markerEllipse && markerCircle && markerCircle->isChecked()
              && overlay.m_sizeButton,
          "circle highlights select in place and keep the width control visible");
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(80, 10));
    mouse(QEvent::MouseMove, annOrigin + QPoint(130, 60));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(130, 60));
    check(overlay.m_anns.last().tool == ATool::Marker && overlay.m_anns.last().fill
              && overlay.m_anns.last().markerEllipse && overlay.m_anns.last().stroke.isEmpty(),
          "marker circle drags a circular highlight block");
    {
        const QRect area = overlay.m_anns.last().rect;
        QImage block(area.width() + 20, area.height() + 20, QImage::Format_ARGB32);
        block.fill(Qt::transparent);
        QPainter painter(&block);
        painter.translate(QPoint(10, 10) - area.topLeft());
        overlay.drawAnnotation(painter, overlay.m_anns.last(), QImage());
        painter.end();
        check(block.pixelColor(block.rect().center()).alpha() > 60
                  && block.pixelColor(11, 11).alpha() == 0,
              "the circle highlight is translucent inside and empty at its bounding corners");
    }
    overlay.toggleFill();
    check(!overlay.m_markerFill && !overlay.m_markerEllipse,
          "the marker fill toggle returns to straight brush highlights");
    check(numberLabel(4, 0) == QStringLiteral("4") && numberLabel(9, 1) == QStringLiteral("IX")
              && numberLabel(14, 1) == QStringLiteral("XIV") && numberLabel(27, 2) == QStringLiteral("AA")
              && numberLabel(3, 3) == QStringLiteral("c") && numberLabel(10, 4) == QStringLiteral("十")
              && numberLabel(21, 4) == QStringLiteral("二十一") && numberLabel(105, 4) == QStringLiteral("一百零五"),
          "number formats render arabic, roman, letter and chinese labels");
    {
        QHelpEvent tip(QEvent::ToolTip, QPoint(4, 4), overlay.m_shapeButton->mapToGlobal(QPoint(4, 4)));
        check(overlay.eventFilter(overlay.m_shapeButton, &tip), "hover tooltips are suppressed on capture controls");
    }
    // Per-tool option strip round: merged shape button, arrow styles, per-tool
    // colors, text weight, and eraser reach.
    overlay.setToolNone();
    overlay.selectTool(ATool::Rect);
    check(overlay.m_shapeButton->isChecked() && !overlay.m_shapeEllipse
              && overlay.m_toolOptions->isVisible(),
          "the merged shape button arms the rectangle with its option strip");
    overlay.selectTool(ATool::Ellipse);
    check(overlay.m_shapeEllipse && overlay.m_toolBtn[int(ATool::Ellipse)]->isChecked() && !overlay.m_shapeButton->isChecked(),
          "arming the ellipse flips the shared button's sub-shape");
    overlay.selectTool(ATool::Rect);
    QComboBox *shapeStyle = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("shapeStyleCombo"));
    check(shapeStyle && shapeStyle->count() == 5 && shapeStyle->currentIndex() == 0
              && shapeStyle->iconSize() == QSize(40, 20),
          "the shape strip offers five line styles as 40x20 samples");
    overlay.pickShapeStyle(1);
    const QPoint styleA = annOrigin + QPoint(8, 54), styleB = styleA + QPoint(48, 22);
    mouse(QEvent::MouseButtonPress, styleA);
    mouse(QEvent::MouseMove, styleB);
    mouse(QEvent::MouseButtonRelease, styleB);
    check(overlay.m_anns.last().tool == ATool::Rect && overlay.m_anns.last().strokeStyle == 1,
          "the picked line style carries into a fresh rectangle");
    overlay.updateToolOptions();
    shapeStyle = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("shapeStyleCombo"));
    check(shapeStyle && shapeStyle->currentIndex() == 1,
          "selecting a styled shape shows its line style");
    const int styleUndo = overlay.m_undoStack.size();
    const int styleIndex = overlay.m_selAnn;
    overlay.pickShapeStyle(3);
    check(overlay.m_anns[styleIndex].strokeStyle == 3 && overlay.m_undoStack.size() == styleUndo + 1,
          "changing the selected shape's line style is one undo step");
    overlay.undoAnnotation();
    check(overlay.m_anns[styleIndex].strokeStyle == 1,
          "undo restores the selected shape's line style");
    {
        auto scan = [&overlay](int style, int &inked, int &gaps) {
            QImage image(300, 140, QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            Annotation sample;
            sample.tool = ATool::Rect;
            sample.rect = QRect(20, 20, 260, 100);
            sample.pen = 6;
            sample.color = Qt::black;
            sample.strokeStyle = style;
            QPainter painter(&image);
            overlay.drawAnnotation(painter, sample, QImage());
            painter.end();
            inked = gaps = 0;
            bool wasInk = false;
            for (int x = 40; x < 280; ++x) {
                const bool ink = image.pixelColor(x, 20).alpha() > 128;
                if (ink) ++inked;
                else if (wasInk || x > 40) ++gaps;
                wasInk = ink;
            }
        };
        int solidInk = 0, solidGaps = 0, dashInk = 0, dashGaps = 0, dotInk = 0, dotGaps = 0;
        scan(0, solidInk, solidGaps);
        scan(1, dashInk, dashGaps);
        scan(2, dotInk, dotGaps);
        check(solidInk > 200 && solidGaps == 0 && dashInk > 40 && dashGaps > 40
                  && dotGaps > dashGaps,
              "solid, dashed and dotted outlines alternate ink and gaps at their own densities");
        auto inkCount = [&overlay](ATool tool, int style) {
            QImage image(300, 140, QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            Annotation sample;
            sample.tool = tool;
            sample.rect = QRect(20, 20, 260, 100);
            sample.pen = 6;
            sample.color = Qt::black;
            sample.strokeStyle = style;
            QPainter painter(&image);
            overlay.drawAnnotation(painter, sample, QImage());
            painter.end();
            int ink = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y).alpha() > 128) ++ink;
            return ink;
        };
        check(inkCount(ATool::Ellipse, 1) > 0
                  && inkCount(ATool::Ellipse, 0) > inkCount(ATool::Ellipse, 1),
              "ellipses use the same dashed outline as rectangles");
    }
    overlay.pickToolColor(QColor("#22b14c"));
    for (ATool shared : {ATool::Marker, ATool::Arrow, ATool::Pen, ATool::Text, ATool::Number, ATool::Rect}) {
        overlay.selectTool(shared);
        check(overlay.m_acolor == QColor("#22b14c") && overlay.m_colorBoard
                  && overlay.m_colorBoard->currentColor() == QColor("#22b14c"),
              "the current color is shared by every colored tool");
    }
    overlay.selectTool(ATool::Marker);
    overlay.selectTool(ATool::Arrow);
    auto *arrowShape = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("arrowStyleCombo"));
    auto *arrowLine = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("arrowLineStyleCombo"));
    check(arrowShape && arrowShape->count() == 10 && arrowLine && arrowLine->count() == 5
              && !overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("dashedButton")),
          "the arrow strip offers ten shapes and five line styles without a separate dash toggle");
    overlay.pickArrowStyle(1);
    overlay.pickArrowLineStyle(1);
    const QPoint a1 = annOrigin + QPoint(60, 5), a2 = annOrigin + QPoint(60, 55);
    mouse(QEvent::MouseButtonPress, a1);
    mouse(QEvent::MouseMove, a2);
    mouse(QEvent::MouseButtonRelease, a2);
    check(overlay.m_anns.last().tool == ATool::Arrow
              && overlay.m_anns.last().arrowStyle == 1 && overlay.m_anns.last().strokeStyle == 1
              && overlay.m_anns.last().stroke.isEmpty(),
          "shape and line style carry into a straight drag arrow");
    overlay.setToolNone();
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(60, 30));
    mouse(QEvent::MouseButtonRelease, annOrigin + QPoint(60, 30));
    check(overlay.m_selAnn == overlay.m_anns.size() - 1
              && overlay.m_anns[overlay.m_selAnn].tool == ATool::Arrow,
          "the arrow stays selectable");
    overlay.pickArrowStyle(2);
    overlay.pickArrowLineStyle(3);
    check(overlay.m_anns[overlay.m_selAnn].arrowStyle == 2
              && overlay.m_anns[overlay.m_selAnn].strokeStyle == 3,
          "the options strip restyles the selected arrow");
    overlay.selectTool(ATool::Arrow);
    overlay.pickArrowStyle(0);
    overlay.pickArrowLineStyle(0);
    const QPoint c1 = annOrigin + QPoint(15, 5);
    mouse(QEvent::MouseButtonPress, c1);
    mouse(QEvent::MouseMove, c1 + QPoint(20, 25));
    mouse(QEvent::MouseMove, c1 + QPoint(70, 30));
    mouse(QEvent::MouseButtonRelease, c1 + QPoint(70, 30));
    check(overlay.m_anns.last().a == c1 && overlay.m_anns.last().b == c1 + QPoint(70, 30)
              && overlay.m_anns.last().stroke.isEmpty() && overlay.m_anns.last().arrowStyle == 0,
          "arrow drag keeps only the two endpoints");
    auto rightAt = [&overlay](const QPoint &at) {
        QMouseEvent right(QEvent::MouseButtonPress, at, overlay.mapToGlobal(at),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&overlay, &right);
    };
    const QPoint mid = c1 + QPoint(35, 15);
    rightAt(mid);
    check(overlay.m_selAnn == -1 && overlay.m_atool == ATool::Arrow,
          "the first right click only deselects");
    rightAt(mid);
    check(overlay.m_selAnn == -1 && overlay.m_atool == ATool::Arrow && overlay.isVisible(),
          "further right clicks keep the tool and the capture");
    const QColor endedColor = overlay.m_anns.last().color;
    overlay.pickToolColor(QColor("#a349a4"));
    check(overlay.m_anns.last().color == endedColor && overlay.m_acolor == QColor("#a349a4"),
          "a pick after deselecting leaves drawn arrows alone");
    const QPoint q1 = annOrigin + QPoint(15, 40), q2 = q1 + QPoint(55, 0);
    mouse(QEvent::MouseButtonPress, q1);
    mouse(QEvent::MouseMove, q2);
    mouse(QEvent::MouseButtonRelease, q2);
    check(overlay.m_anns.last().color == QColor("#a349a4")
              && overlay.m_anns.last().a == q1 && overlay.m_anns.last().b == q2,
          "the next arrow starts with the new color");
    // Rendering parity: ten shapes, five line styles, hollow body, bars and antialiased edges.
    {
        const QColor green(0x34, 0xc7, 0x59);
        auto render = [&overlay](Annotation sample) {
            QImage canvas(300, 200, QImage::Format_ARGB32);
            canvas.fill(Qt::transparent);
            QPainter painter(&canvas);
            overlay.drawAnnotation(painter, sample, QImage());
            painter.end();
            return canvas;
        };
        Annotation ann;
        ann.tool = ATool::Arrow;
        ann.color = green;
        ann.pen = 6;
        ann.a = QPoint(10, 100);
        ann.b = QPoint(260, 100);
        for (int style = 0; style < 10; ++style) {
            ann.arrowStyle = style;
            ann.strokeStyle = 0;
            const QImage image = render(ann);
            int ink = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y).alpha() > 0) ++ink;
            check(ink > 100, "every arrow shape draws visible ink");
        }
        auto runLengths = [](const QImage &image) {
            QVector<int> runs;
            int run = 0;
            for (int x = 20; x <= 240; ++x) {
                if (image.pixelColor(x, 100).alpha() > 200) ++run;
                else if (run) { runs << run; run = 0; }
            }
            if (run) runs << run;
            return runs;
        };
        QVector<int> dashes;
        for (int style = 1; style <= 4; ++style) {
            ann.arrowStyle = 0;
            ann.strokeStyle = style;
            const QImage image = render(ann);
            const QVector<int> runs = runLengths(image);
            int gaps = 0;
            bool wasInk = false;
            for (int x = 20; x <= 240; ++x) {
                const bool ink = image.pixelColor(x, 100).alpha() > 200;
                if (wasInk && !ink) ++gaps;
                wasInk = ink;
            }
            int longest = 0;
            for (int run : runs) longest = qMax(longest, run);
            check(!runs.isEmpty() && gaps > 1,
                  "dash, dot, dash-dot and dash-dot-dot shafts alternate ink");
            dashes << longest;
        }
        check(dashes.size() == 4 && dashes[0] > dashes[1] && dashes[2] > dashes[1],
              "the dash style has longer strokes than the dot style");
        ann.arrowStyle = 4;
        ann.strokeStyle = 0;
        {
            const QImage image = render(ann);
            bool inked = false;
            for (int dy = 1; dy <= 6; ++dy)
                inked = inked || image.pixelColor(120, 100 - dy).alpha() > 80
                              || image.pixelColor(120, 100 + dy).alpha() > 80;
            check(image.pixelColor(120, 100).alpha() < 30 && inked,
                  "hollow arrows keep an empty body and inked outline");
        }
        ann.arrowStyle = 5;
        ann.strokeStyle = 1;
        {
            const QImage image = render(ann);
            bool gap = false;
            for (int x = 20; x <= 200; ++x)
                if (image.pixelColor(x, 100).alpha() < 200) gap = true;
            check(!gap, "filled arrows stay solid regardless of the selected line style");
        }
        ann.arrowStyle = 0;
        ann.strokeStyle = 0;
        const QImage single = render(ann);
        ann.arrowStyle = 1;
        const QImage bilateral = render(ann);
        check(single.pixelColor(20, 96).alpha() < 30
                  && bilateral.pixelColor(20, 96).alpha() > 150,
              "double straight arrows carry a head at the start");
        ann.arrowStyle = 6;
        const QImage dimension = render(ann);
        check(dimension.pixelColor(10, 93).alpha() > 150
                  && dimension.pixelColor(260, 93).alpha() > 150
                  && single.pixelColor(10, 93).alpha() < 30,
              "dimension lines add end bars instead of arrowheads");
        ann.arrowStyle = 9;
        const QImage dimensionArrow = render(ann);
        check(dimensionArrow.pixelColor(10, 93).alpha() > 150
                  && dimensionArrow.pixelColor(250, 96).alpha() > 150
                  && dimension.pixelColor(250, 96).alpha() < 30,
              "dimension arrows combine end bars with solid heads");
        ann.arrowStyle = 0;
        ann.a = QPoint(10, 150);
        ann.b = QPoint(200, 80);
        const QImage diagonal = render(ann);
        const QLineF center(ann.a, ann.b);
        const QPointF unit = (center.p2() - center.p1()) / center.length();
        const QPointF edgePoint = center.pointAt(0.5)
            + QPointF(-unit.y(), unit.x()) * (ann.pen / 2.0 + 0.5);
        int blended = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int alpha = diagonal.pixelColor(edgePoint.toPoint() + QPoint(dx, dy)).alpha();
                if (alpha > 0 && alpha < 255) ++blended;
            }
        check(blended > 0, "antialiased arrow edges blend into the background");
        if (!previewPath.isEmpty()) {
            QImage preview(620, 760, QImage::Format_ARGB32);
            preview.fill(QColor(30, 34, 38));
            QPainter previewPainter(&preview);
            Annotation sample;
            sample.tool = ATool::Arrow;
            sample.color = Qt::white;
            sample.pen = 4;
            for (int style = 0; style < 10; ++style) {
                sample.arrowStyle = style;
                sample.a = QPoint(20, 28 + style * 68);
                sample.b = QPoint(280, 28 + style * 68);
                overlay.drawAnnotation(previewPainter, sample, QImage());
            }
            previewPainter.end();
            preview.save(previewPath + QStringLiteral(".arrow-shapes.png"));
        }
    }
    {   // Pen strokes render antialiased diagonals.
        const QColor green(0x34, 0xc7, 0x59);
        Annotation ink;
        ink.tool = ATool::Pen;
        ink.color = green;
        ink.pen = 4;
        ink.stroke << QPointF(10, 10) << QPointF(120, 90);
        QImage canvas(140, 110, QImage::Format_ARGB32);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        overlay.drawAnnotation(painter, ink, QImage());
        painter.end();
        int blended = 0;
        for (int y = 0; y < canvas.height(); ++y)
            for (int x = 0; x < canvas.width(); ++x) {
                const int alpha = canvas.pixelColor(x, y).alpha();
                if (alpha > 0 && alpha < 255) ++blended;
            }
        check(blended > 20, "pen strokes antialias instead of staircasing");
    }
    overlay.setTextBold(true);
    overlay.setTextItalic(true);
    overlay.selectTool(ATool::Text);
    check(overlay.m_abold && overlay.m_aitalic, "text toggles persist per tool");
    mouse(QEvent::MouseButtonPress, annOrigin + QPoint(30, 30));
    overlay.m_editor->setPlainText(QStringLiteral("粗"));
    overlay.commitTextEdit();
    check(overlay.m_anns.last().bold && overlay.m_anns.last().italic,
          "bold and italic commit into text annotations");
    overlay.m_anns.clear();
    overlay.m_selAnn = -1;
    // Every tool exposes an editable size; reclicking the armed tool disarms it, a third click re-arms.
    QVector<QImage> toolbarPreviews;
    for (ATool tool : {ATool::Rect, ATool::Ellipse, ATool::Arrow, ATool::Pen, ATool::Marker,
                       ATool::Text, ATool::Number, ATool::Mosaic, ATool::Eraser}) {
        overlay.selectTool(tool);
        overlay.m_toolBtn[int(tool)]->click();
        check(overlay.m_atool == ATool::None && !overlay.m_toolBtn[int(tool)]->isChecked()
                  && !overlay.m_toolOptions->isVisible(),
              "reclicking the armed tool disarms it and hides its options");
        overlay.m_toolBtn[int(tool)]->click();
        check(overlay.m_atool == tool && overlay.m_toolOptions->isVisible()
                  && (tool == ATool::Mosaic ? overlay.m_mosaicStrengthLabel != nullptr
                      : tool == ATool::Text ? overlay.m_sizeInput != nullptr
                      : overlay.m_sizeButton != nullptr),
              "repeated toolbar clicks keep the matching options and numeric control visible");
        const bool textSize = tool == ATool::Text;
        if (tool == ATool::Eraser) {
            const auto *strip = static_cast<QBoxLayout *>(overlay.m_toolOptions->layout());
            check(strip->itemAt(0)->widget() == overlay.m_sizeButton
                      && overlay.m_sizeButton->text().toInt() == overlay.toolSize()
                      && overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("eraserBrush"))
                      && overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("eraserRectangle"))
                      && overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("eraserSmart")),
                  "the eraser strip starts with the width button, then brush, rectangle, smart");
        } else if (tool != ATool::Mosaic && tool != ATool::Text) {
            const auto *strip = static_cast<QBoxLayout *>(overlay.m_toolOptions->layout());
            check(!overlay.m_sizeInput && strip->itemAt(0)->widget() == overlay.m_sizeButton
                      && overlay.m_sizeButton->text().toDouble() == overlay.toolSize(),
                  "width tools put a width button first instead of a dropdown");
            check(overlay.m_toolOptions->width() == overlay.m_toolOptions->sizeHint().width(),
                  "the option strip is only as wide as its contents");
        } else if (tool == ATool::Mosaic) {
            check(overlay.m_mosaicStrengthLabel->text().toInt() == overlay.mosaicStrength()
                      && !overlay.m_sizeInput && overlay.m_sizeButton
                      && overlay.m_sizeButton->text().toInt() == overlay.widthControlValue(),
                  "mosaic exposes independent mode-specific width and strength controls");
        } else {
        check(overlay.m_sizeInput->width() == (textSize ? 62 : 46)
                  && overlay.m_sizeInput->lineEdit()->alignment() == Qt::AlignCenter,
              "numeric controls use a compact fixed width with centered values");
        check(overlay.m_sizeInput->lineEdit()->contentsRect().width()
                  >= QFontMetrics(overlay.m_sizeInput->lineEdit()->font()).horizontalAdvance(
                      textSize ? QStringLiteral("1637.5") : QStringLiteral("12")),
              "compact numeric controls still fit the complete supported values");
        check(overlay.m_toolOptions->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly).isEmpty(),
              "option strips contain no visible thickness labels or wheel instructions");
        }
        for (QWidget *bar : {overlay.m_tools, overlay.m_toolOptions})
            for (QToolButton *button : bar->findChildren<QToolButton *>())
                check(button == overlay.m_sizeButton || button->text().isEmpty(),
                      "toolbar actions render icons or swatches rather than text");
        if (!previewPath.isEmpty()) {
            QImage bars = overlay.grab(overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry())).toImage();
            bars.setDevicePixelRatio(1);
            toolbarPreviews.append(bars);
        }
        const bool colored = tool != ATool::Mosaic && tool != ATool::Eraser;
        check((overlay.m_colorBoard != nullptr) == colored
                  && overlay.m_toolOptions->findChildren<ColorBoard *>().size() == (colored ? 1 : 0),
              "colored tools end with one color board; mosaic and eraser have none");
        if (colored) {
            const QList<QWidget *> row = overlay.m_toolOptions->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
            const int at = row.indexOf(overlay.m_colorBoard);
            check(at > 0 && row.at(at - 1)->objectName() == QStringLiteral("barSeparator")
                      && overlay.m_colorBoard->focusPolicy() == Qt::StrongFocus,
                  "the color board follows a separator and accepts keyboard focus");
        }
    }
    if (!toolbarPreviews.isEmpty()) {
        int width = 0, height = 0;
        for (const QImage &bars : toolbarPreviews) { width = qMax(width, bars.width()); height += bars.height() + 8; }
        QImage sheet(width, height, QImage::Format_RGB32);
        sheet.fill(QColor(15, 17, 20));
        QPainter painter(&sheet);
        int y = 0;
        for (const QImage &bars : toolbarPreviews) { painter.drawImage(0, y, bars); y += bars.height() + 8; }
        painter.end();
        check(sheet.save(previewPath + QStringLiteral(".toolbars.png")), "save toolbar visual regression sheet");
    }
    overlay.selectTool(ATool::Pen);
    overlay.m_sizeButton->click();
    check(overlay.m_sizePopup && overlay.m_sizePopup->isVisible() && overlay.m_sizeSlider
              && overlay.m_sizeSlider->orientation() == Qt::Horizontal
              && overlay.m_sizeSlider->minimum() == 1 && overlay.m_sizeSlider->maximum() == 50
              && overlay.m_sizePopup->geometry().top() >= overlay.m_sizeButton->mapToGlobal(QPoint(0, overlay.m_sizeButton->height())).y(),
          "clicking the width button opens a horizontal slider below it");
    overlay.m_sizeSlider->setValue(6);
    check(overlay.toolSize() == 6 && overlay.m_sizeButton->text().toDouble() == 6 && overlay.isVisible(),
          "dragging the slider changes the tool width without pinning the capture");
    QWheelEvent sizeWheel(QPointF(10, 10), QPointF(overlay.m_sizeSlider->mapToGlobal(QPoint(10, 10))),
                         QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                         Qt::ScrollUpdate, false);
    QApplication::sendEvent(overlay.m_sizeSlider, &sizeWheel);
    check(overlay.toolSize() == 7 && overlay.m_sizeSlider->value() == 7 && overlay.m_sizeButton->text().toDouble() == 7,
          "wheel input over the slider uses the same width and updates the displayed value");
    overlay.m_sizePopup->close();
    QCoreApplication::processEvents();
    const QPoint sizeStroke = annOrigin + QPoint(15, 15);
    mouse(QEvent::MouseButtonPress, sizeStroke);
    overlay.tuneWidth(1, sizeStroke);
    mouse(QEvent::MouseMove, sizeStroke + QPoint(20, 10));
    overlay.pickToolColor(QColor("#007aff"));
    check(overlay.m_tools->isVisible() && overlay.m_toolOptions->isVisible()
              && overlay.m_sizeButton->text().toDouble() == 8 && overlay.m_ann.pen == 8,
          "drawing keeps both bars visible and wheel changes update the live stroke and number");
    check(overlay.m_ann.color == QColor("#007aff"), "color changes apply to the in-progress stroke");
    mouse(QEvent::MouseButtonRelease, sizeStroke + QPoint(20, 10));
    overlay.m_sizeButton->click();
    overlay.m_sizeSlider->setSliderDown(true);    // One drag across several values is one undo step.
    overlay.m_sizeSlider->setValue(10);
    overlay.m_sizeSlider->setValue(9);
    overlay.m_sizeSlider->setSliderDown(false);
    check(overlay.m_selAnn == -1 && overlay.m_anns.last().pen == 8 && overlay.toolSize() == 9
              && overlay.m_sizeButton->text().toDouble() == 9,
          "the width slider changes future pen strokes without retuning completed ink");
    overlay.m_sizePopup->close();
    QCoreApplication::processEvents();
    overlay.selectTool(ATool::Marker);
    overlay.setToolSize(8);
    mouse(QEvent::MouseButtonPress, sizeStroke + QPoint(30, 10));
    mouse(QEvent::MouseMove, sizeStroke + QPoint(80, 20));
    mouse(QEvent::MouseButtonRelease, sizeStroke + QPoint(80, 20));
    overlay.m_sizeButton->click();
    overlay.m_sizeSlider->setSliderDown(true);
    overlay.m_sizeSlider->setValue(10);
    overlay.m_sizeSlider->setValue(9);
    overlay.m_sizeSlider->setSliderDown(false);
    check(overlay.m_anns.last().pen == 9 && overlay.m_sizeButton->text().toDouble() == 9,
          "the slider retunes a selected geometric highlight");
    overlay.m_sizePopup->close();
    QCoreApplication::processEvents();
    overlay.undoAnnotation();
    check(overlay.m_anns.last().pen == 8, "one undo restores the width before a numeric edit");
    overlay.redoAnnotation();
    check(overlay.m_anns.last().pen == 9, "redo reapplies the numeric width edit");
    overlay.selectTool(ATool::Pen);
    overlay.setToolSize(99);
    check(overlay.toolSize() == 50 && overlay.m_sizeButton->text().toDouble() == 50,
          "width input stays within the supported range");
    overlay.setToolSize(0);
    check(overlay.toolSize() == 1 && overlay.m_sizeButton->text().toDouble() == 1,
          "width input cannot become zero or negative");
    overlay.placeToolBars();
    overlay.showToolBars();
    check(overlay.m_toolOptions->x() == overlay.m_tools->x()
              && overlay.m_toolOptions->y() == overlay.m_tools->geometry().bottom() + 1 + Bar::gap
              && overlay.m_toolOptions->width() == overlay.m_toolOptions->sizeHint().width(),
          "the options strip sits 8px under the toolbar's bottom-left edge at its own width");
    for (QWidget *bar : {overlay.m_tools, overlay.m_toolOptions}) {
        const QImage painted = overlay.grab(bar->geometry()).toImage();
        const qreal dpr = painted.devicePixelRatio();
        const QColor background = painted.pixelColor(qRound((bar->width() - 3) * dpr), qRound((bar->height() / 2) * dpr));
        const QColor edge = painted.pixelColor(qRound(10 * dpr), 0);
        const QColor corner = painted.pixelColor(0, 0);   // Rounded 8px corner leaves the frozen frame visible.
        if (background != Bar::surface || edge != Bar::surface || corner == Bar::surface)
            std::fprintf(stderr, "bar background %d,%d,%d edge %s corner %s size=%dx%d dpr=%.2f main=%d\n",
                         background.red(), background.green(), background.blue(), qPrintable(edge.name()),
                         qPrintable(corner.name()), bar->width(), bar->height(), double(dpr), bar == overlay.m_tools);
        check(background == Bar::surface && edge == Bar::surface && corner != Bar::surface && bar->height() == (bar == overlay.m_tools ? 42 : 44),
              "main and option bars keep their approved compact heights and rounded surfaces");
    }
    for (QWidget *widget : overlay.m_toolButtons) {
        const QIcon icon = static_cast<QToolButton *>(widget)->icon();
        for (const auto &expect : {qMakePair(QIcon::Normal, Bar::ink),
                                   qMakePair(QIcon::Disabled, Bar::dim)}) {
            const QImage pixels = icon.pixmap(QSize(16, 16), expect.first).toImage();
            bool exact = true, visible = false;
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x) {
                    const QColor color = pixels.pixelColor(x, y);
                    if (color.alpha() < 128) continue;    // Thin curves never reach full coverage.
                    visible = true;
                    if (qAbs(color.red() - expect.second.red()) > 3 || qAbs(color.green() - expect.second.green()) > 3
                        || qAbs(color.blue() - expect.second.blue()) > 3) exact = false;
                }
            if (!visible || !exact)
                std::fprintf(stderr, "icon %s mode=%d visible=%d size=%dx%d\n",
                             qPrintable(widget->toolTip()), int(expect.first), int(visible),
                             pixels.width(), pixels.height());
            check(visible && exact, "toolbar icons are light glyphs, dim when disabled");
        }
        check(icon.pixmap(QSize(16, 16), QIcon::Normal, QIcon::On).toImage()
                  != icon.pixmap(QSize(16, 16), QIcon::Normal, QIcon::Off).toImage(),
              "checked tools switch to accent glyphs");
    }
    overlay.m_anns.clear();
    overlay.m_undoStack.clear();
    overlay.m_redoStack.clear();
    overlay.m_atool = ATool::None;
    overlay.m_selAnn = -1;
    overlay.syncUndoButtons();
    overlay.m_windowRects.clear();
    overlay.m_sel = QRect(2, 2, 30, 20);
    overlay.m_mode = Adjusting;
    const QImage cropped = overlay.cropped().toImage();
    check(cropped.size() == QSize(45, 30), "selection crop uses native capture coordinates");
    check(cropped.devicePixelRatio() == 1, "capture output uses raw pixels for pinning and clipboard consumers");
    QKeyEvent copyImage(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(&overlay, &copyImage);
    check(clipboard->image() == cropped && !overlay.isVisible(), "Ctrl+C still copies only the selection and closes capture");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    for (const Mode mode : {Selecting, Adjusting}) {
        overlay.m_mode = mode;
        overlay.show();
        inspector->show();
        overlay.m_tools->setVisible(mode == Adjusting);
        QApplication::sendEvent(&overlay, &escape);
        check(!overlay.isVisible() && !inspector->isVisible() && !overlay.m_tools->isVisible(),
              "Escape exits capture before and after selecting a region");
    }
    clipboard->setMimeData(saved);
    std::puts("PASS: pixel sampling, magnifier, color shortcuts, and capture regression checks");
    return 0;
}


