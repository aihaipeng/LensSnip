#include "platform/window_detection.h"
#include "platform/output.h"
#include "tests/test_support.h"
#include "shared/widgets.h"
#include "pin/pin_window.h"
#include "longshot/long_capture.h"
#include "text/cards/ocr_dialog.h"
#include <QtMath>
#include <QSemaphore>
#include <qt_windows.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <cstdio>
#include <cstdlib>

static LRESULT CALLBACK layeredFixtureProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WindowDetector::testCapacity()
{
    class Waiting : public WindowDetector {
    public:
        Waiting() : WindowDetector(nullptr, DetectionOptions()) {}
        QSemaphore entered, release;
        void run() override { entered.release(); release.acquire(); }
    } first, second;
    first.start(); second.start();
    first.entered.acquire(); second.entered.acquire();
    s_workers.append(&first); s_workers.append(&second);
    WindowDetector *rejected = create(nullptr, DetectionOptions());
    const bool capped = rejected == nullptr;
    delete rejected;
    first.release.release(); second.release.release();
    first.wait(); second.wait();
    WindowDetector *resumed = create(nullptr, DetectionOptions());
    const bool recovered = resumed != nullptr;
    delete resumed;
    if (!capped || !recovered) std::fprintf(stderr, "FAIL: blocked-worker capacity and release recovery\n");
    return capped && recovered ? 0 : 1;
}
#include "capture/capture_overlay.h"
#include <QGuiApplication>
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QRect>
#include <QStringList>
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QCursor>
#include <QTimer>
#include <QEventLoop>
#include <QThread>
#include <QElapsedTimer>
#include <QPointer>
#include <QVariantAnimation>
#include <QFileInfo>

int CaptureOverlay::testWindowDetection()
{
    const auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    static_assert(DWMWA_CLOAKED == 14, "Use the cloaking attribute, not a rendering policy");
    check(WindowDetector::testCapacity() == 0, "stalled detection workers are bounded across capture sessions");
    DetectedWindow synthetic(QRect(0, 0, 100, 100), {QRect(0, 20, 100, 80), QRect(0, 20, 100, 80), QRect(0, 0, 100, 100)});
    prepareChildren(synthetic);
    check(synthetic.nativeChildren.size() == 2, "children are clipped, deduplicated, and split into uncovered strips");
    check(::childAt(synthetic, QPoint(30, 10)) == QRect(0, 0, 100, 20), "uncovered title strip is a detection target");
    check(::childAt(synthetic, QPoint(30, 40)) == QRect(0, 20, 100, 80), "content excludes the title strip exactly");
    check(::childAt(synthetic, QPoint(30, 40), QRect(0, 20, 100, 80)) == synthetic.native,
          "expanding skips the current child and returns its containing window");
    check(rectArea(QRect(0, 0, 80000, 80000)) == Q_INT64_C(6400000000), "rectangle ordering cannot overflow 32 bits");
    const QVector<ScreenFrame> scaled{{QRect(-200, 0, 200, 100), QRect(-300, 0, 300, 150), QImage()}};
    check(logicalFromNative(scaled, QRect(-300, 0, 300, 150)) == QRect(-200, 0, 200, 100),
          "native rectangles map exclusive edges without a one-pixel expansion");
    QImage shadowImage(160, 120, QImage::Format_RGB32);
    shadowImage.fill(QColor(45, 45, 45));
    for (int y = 8; y < 112; ++y)
        for (int x = 10; x < 150; ++x)
            shadowImage.setPixelColor(x, y, QColor(220, 30, 40));
    const QRect shadowNative(1000, 1000, 160, 120);
    const QRect shadowHit(1004, 1004, 152, 112);
    check(pixelContentRect(shadowImage, shadowNative, shadowHit) == QRect(1010, 1008, 140, 104),
          "pixel refinement stops at the first strong content edge inside the hit-test rim");
    const QRect available = nativeFromLogical(QGuiApplication::primaryScreen()->availableGeometry());
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"LensSnipDetectionFixture";
    check(RegisterClassW(&windowClass) != 0, "register the native fixture class");
    struct Fixture {
        HWND window = nullptr;
        QPoint cursor = QCursor::pos();
        ~Fixture() {
            if (window) DestroyWindow(window);
            UnregisterClassW(L"LensSnipDetectionFixture", GetModuleHandleW(nullptr));
            QCursor::setPos(cursor);
        }
    } fixture;
    fixture.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, windowClass.lpszClassName, L"Detection test",
        WS_POPUP | WS_VISIBLE, available.center().x() - 160, available.center().y() - 100,
        320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(fixture.window != nullptr, "create a native detection fixture");
    HWND button = CreateWindowExW(0, L"BUTTON", L"Button", WS_CHILD | WS_VISIBLE,
        20, 30, 100, 32, fixture.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND edit = CreateWindowExW(0, L"EDIT", L"Input", WS_CHILD | WS_VISIBLE | WS_BORDER,
        20, 90, 180, 30, fixture.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(button && edit, "create button and input controls");
    const auto contains = [&fixture](const WindowTargets &targets) {
        for (const DetectedWindow &win : targets) if (win.hwnd == fixture.window) return true;
        return false;
    };
    check(contains(detectWindowRects()), "visible fixture is detected");
    check(!contains(detectWindowRects(fixture.window)), "the capture overlay handle is excluded");
    check(!contains(detectWindowRects(nullptr, {QFileInfo(QCoreApplication::applicationFilePath()).fileName().toUpper()})),
          "program exclusions use executable names and ignore case");
    SetWindowLongPtrW(fixture.window, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT);
    check(!contains(detectWindowRects()), "click-through windows are not capture targets");
    SetWindowLongPtrW(fixture.window, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    EnableWindow(fixture.window, FALSE);
    check(!contains(detectWindowRects()), "disabled windows are not capture targets");
    EnableWindow(fixture.window, TRUE);
    const auto rawRect = [](HWND hwnd) {
        RECT r{};
        GetWindowRect(hwnd, &r);
        return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    };
    const QRect buttonRect = rawRect(button), editRect = rawRect(edit);
    bool exact = false;
    for (const DetectedWindow &win : detectWindowRects())
        if (win.hwnd == fixture.window) exact = win.nativeChildren.contains(buttonRect) && win.nativeChildren.contains(editRect);
    check(exact, "native child rectangles do not grow by one pixel");
    DetectionOptions windowsOnly;
    WindowTargets snapshot;
    for (const DetectedWindow &win : detectWindowRects())
        if (win.hwnd == fixture.window) snapshot.append(win);
    ShowWindow(fixture.window, SW_HIDE);
    WindowDetector seeded(nullptr, windowsOnly, snapshot);
    QRect seededResult;
    QEventLoop seededLoop;
    QObject::connect(&seeded, &WindowDetector::gotRect, &seededLoop, [&](const QRect &r, quint64) {
        seededResult = r; seededLoop.quit();
    });
    seeded.query(buttonRect.center());
    seeded.start();
    QTimer::singleShot(5000, &seededLoop, &QEventLoop::quit);
    seededLoop.exec();
    seeded.stop();
    seeded.wait();
    ShowWindow(fixture.window, SW_SHOW);
    check(seededResult == snapshot.first().native,
          "a pre-activation target snapshot survives focus-loss-only popups");
    WindowDetector coarse(nullptr, windowsOnly);
    QRect coarseResult;
    quint64 lastId = 0, returnedId = 0;
    QEventLoop coarseLoop;
    QObject::connect(&coarse, &WindowDetector::gotRect, &coarseLoop, [&](const QRect &r, quint64 id) {
        coarseResult = r; returnedId = id; coarseLoop.quit();
    });
    for (int i = 0; i < 20; ++i) lastId = coarse.query(buttonRect.center());
    coarse.start();
    QTimer::singleShot(5000, &coarseLoop, &QEventLoop::quit);
    coarseLoop.exec();
    coarse.stop();
    coarse.wait();
    check(returnedId == lastId && coarseResult == windowFrame(fixture.window),
          "UI detection defaults off and rapid requests return only the newest whole-window result");
    const QVector<ScreenFrame> frames = freezeScreens();
    check(!frames.isEmpty(), "capture fixture pixels");
    QCursor::setPos(logicalFromNative(frames, buttonRect).center());
    DetectionOptions options;
    options.elements = true;
    CaptureOverlay overlay(frames, {}, options, true);
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    const auto waitResult = [&overlay, &check]() {
        QElapsedTimer budget;
        budget.start();
        while (overlay.m_queryPending && budget.elapsed() < 5000) {
            QEventLoop loop;
            QTimer::singleShot(10, &loop, &QEventLoop::quit);
            loop.exec();
        }
        check(!overlay.m_queryPending, "background detector replies without blocking the GUI");
    };
    waitResult();
    const QRect expectedButton = overlay.localScreenRect(logicalFromNative(frames, buttonRect));
    const QRect expectedEdit = overlay.localScreenRect(logicalFromNative(frames, editRect));
    overlay.requestDetection(expectedButton.center());
    waitResult();
    if (overlay.m_hover != expectedButton)
        std::fprintf(stderr, "hover=(%d,%d %dx%d) expected=(%d,%d %dx%d) windows=%lld children=%lld pending=%d\n",
            overlay.m_hover.x(), overlay.m_hover.y(), overlay.m_hover.width(), overlay.m_hover.height(),
            expectedButton.x(), expectedButton.y(), expectedButton.width(), expectedButton.height(),
            (long long)overlay.m_windowRects.size(),
            overlay.m_windowRects.isEmpty() ? 0LL : (long long)overlay.m_windowRects.first().nativeChildren.size(),
            int(overlay.m_queryPending));
    check(overlay.m_hover == expectedButton, "live MSAA/native detection highlights a button under the overlay");
    QCursor::setPos(expectedEdit.center() + overlay.geometry().topLeft());
    overlay.requestDetection(expectedEdit.center());
    waitResult();
    check(overlay.m_hover == expectedEdit, "moving queries the input control instead of retaining the button");
    const quint64 stale = overlay.m_queryId;
    overlay.cancelDetection();
    overlay.handleDetectedRect(buttonRect, stale);
    check(overlay.m_hover == expectedEdit, "cancelled or stale replies cannot replace the current target");
    overlay.handleDetectedRect(buttonRect, stale + 1);
    check(overlay.m_hover == expectedEdit,
          "cancelling an unapplied query rejects its late reply too");
    overlay.m_mode = Adjusting;
    overlay.m_sel = expectedEdit;
    overlay.m_hover = {};
    overlay.m_queryId = stale + 2;
    overlay.handleDetectedRect(buttonRect, stale + 1);
    check(overlay.m_hover.isEmpty() && overlay.m_sel == expectedEdit,
          "superseded replies cannot restore hover highlights after selection lock");
    overlay.m_mode = Selecting;
    overlay.m_sel = {};
    overlay.cancelDetection();
    // A result superseded by a newer query must not paint an older cursor position:
    // doing so makes fast slides flicker back and forth.
    overlay.m_hover = expectedEdit;
    overlay.requestDetection(expectedButton.center());
    const quint64 superseded = overlay.m_queryId;      // First of two back-to-back queries.
    overlay.requestDetection(expectedEdit.center());
    overlay.handleDetectedRect(buttonRect, superseded);
    check(overlay.m_hover == expectedEdit, "superseded hover results cannot replace the newest target");
    waitResult();
    check(overlay.m_hover == expectedEdit, "the newest query result wins once it arrives");
    overlay.m_detectionPoint = expectedEdit.center();
    overlay.m_queryId = 9200;
    overlay.m_hover = expectedButton;
    overlay.m_hoverPaint = expectedButton;
    if (overlay.m_hoverAnimation) overlay.m_hoverAnimation->stop();
    overlay.m_hoverPaintTimer.stop();
    overlay.handleDetectedRect(editRect, 9200);
    check(overlay.m_hover == expectedEdit, "a new hover target commits immediately");
    check(overlay.m_hoverPaint == expectedButton, "the new target glides from the visible rect");
    QElapsedTimer startMotion;
    startMotion.start();
    while (overlay.m_hoverPaint == expectedButton && startMotion.elapsed() < 80) {
        QEventLoop loop;
        QTimer::singleShot(5, &loop, &QEventLoop::quit);
        loop.exec();
    }
    check(overlay.m_hoverPaint != expectedButton, "the first transition frame starts without a cadence wait");
    QElapsedTimer transition;
    transition.start();
    while (overlay.m_hoverPaint != expectedEdit && transition.elapsed() < 300) {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    check(overlay.m_hoverPaint == expectedEdit, "the mask hole settles on the new target");
    // A late whole-window result must not replace the element the cursor is
    // still inside; only leaving it (or wheel expansion) may widen the target.
    const QRect expectedWindow = overlay.localScreenRect(logicalFromNative(frames, windowFrame(fixture.window)));
    overlay.m_mode = Selecting;
    overlay.m_sel = {};
    overlay.m_drag = H_None;
    overlay.m_hover = expectedButton;
    overlay.m_detectionPoint = expectedButton.center();
    overlay.m_expanding = false;
    overlay.m_queryId = 9001;
    overlay.handleDetectedRect(windowFrame(fixture.window), 9001);
    check(overlay.m_hover == expectedButton,
          "a slow provider cannot widen the hovered element to its window");
    overlay.m_detectionPoint = expectedEdit.center();
    overlay.handleDetectedRect(windowFrame(fixture.window), 9001);
    check(overlay.m_hover == expectedWindow,
          "leaving the element lets the containing window take over");
    overlay.m_hover = expectedWindow;
    overlay.m_detectionPoint = expectedWindow.center();
    overlay.m_refineTried = false;
    overlay.m_queryId = 9100;
    overlay.handleDetectedRect(windowFrame(fixture.window), 9100);
    check(overlay.m_refineScheduled, "container-sized hover schedules one refinement");
    overlay.m_refineTimer.stop();
    overlay.m_refineScheduled = false;
    overlay.m_hover = expectedEdit;
    overlay.m_detectionPoint = expectedEdit.center();
    // Probe COM directly without native child hints while the UI pumps provider messages.
    QRect accessible;
    QEventLoop comLoop;
    QThread *probe = QThread::create([&] {
        const HRESULT hr = CoInitialize(nullptr);
        if (SUCCEEDED(hr)) {
            accessible = accessibleAt(fixture.window, buttonRect.center(), rawRect(fixture.window));
            CoUninitialize();
        }
    });
    QObject::connect(probe, &QThread::finished, &comLoop, &QEventLoop::quit);
    probe->start();
    QTimer::singleShot(5000, &comLoop, &QEventLoop::quit);
    comLoop.exec();
    check(probe->isFinished(), "MSAA-only probe finishes");
    probe->wait();
    delete probe;
    if (accessible != buttonRect)
        std::fprintf(stderr, "MSAA returned (%d,%d %dx%d); button is (%d,%d %dx%d)\n",
            accessible.x(), accessible.y(), accessible.width(), accessible.height(),
            buttonRect.x(), buttonRect.y(), buttonRect.width(), buttonRect.height());
    check(accessible == buttonRect, "IAccessible accHitTest returns the button without native child hints");
    // Scoped UIA probe: walks from the window's own element, so the fullscreen capture
    // overlay above the fixture cannot hijack the result the way a global query would.
    QRect uiaHit, uiaGrow, uiaMarked;
    QThread *uiaProbe = QThread::create([&] {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            UiaAutomation *automation = nullptr;
            if (SUCCEEDED(CoCreateInstance(kUiaClassId, nullptr, CLSCTX_INPROC_SERVER, kUiaIid,
                                           reinterpret_cast<void **>(&automation))) && automation) {
                QStringList slow;
                const QRect window = rawRect(fixture.window);
                uiaHit = uiaRectAt(automation, fixture.window, buttonRect.center(), window,
                                   QRect(), &slow, QStringLiteral("uia-probe"));
                uiaGrow = uiaRectAt(automation, fixture.window, buttonRect.center(), window,
                                    buttonRect, &slow, QStringLiteral("uia-probe"));
                QStringList marked{QStringLiteral("uia-probe")};
                uiaMarked = uiaRectAt(automation, fixture.window, buttonRect.center(), window,
                                      QRect(), &marked, QStringLiteral("uia-probe"));
                automation->Release();
            }
            CoUninitialize();
        }
    });
    QEventLoop uiaLoop;
    QObject::connect(uiaProbe, &QThread::finished, &uiaLoop, &QEventLoop::quit);
    uiaProbe->start();
    QTimer::singleShot(5000, &uiaLoop, &QEventLoop::quit);
    uiaLoop.exec();
    check(uiaProbe->isFinished(), "UIA-only probe finishes");
    uiaProbe->wait();
    delete uiaProbe;
    if (uiaHit != buttonRect)
        std::fprintf(stderr, "UIA returned (%d,%d %dx%d); button is (%d,%d %dx%d)\n",
            uiaHit.x(), uiaHit.y(), uiaHit.width(), uiaHit.height(),
            buttonRect.x(), buttonRect.y(), buttonRect.width(), buttonRect.height());
    check(uiaHit == buttonRect, "scoped UIA walk returns the button without native hints");
    check(uiaGrow.contains(buttonRect) && uiaGrow != buttonRect,
          "UIA wheel expansion walks to a strictly containing ancestor");
    check(uiaMarked == buttonRect,
          "a slow-provider mark is diagnostic and cannot disable later UIA queries");
    // A warm walk answers sibling and ancestor queries without COM, so a fast
    // sweep does not repeat the 240 ms tree walk for every control.
    UiaCache cache;
    QRect cachedHit, cachedGrow;
    QThread *cacheProbe = QThread::create([&] {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            UiaAutomation *automation = nullptr;
            if (SUCCEEDED(CoCreateInstance(kUiaClassId, nullptr, CLSCTX_INPROC_SERVER, kUiaIid,
                                           reinterpret_cast<void **>(&automation))) && automation) {
                QStringList slow;
                const QRect window = rawRect(fixture.window);
                uiaRectAt(automation, fixture.window, buttonRect.center(), window,
                          QRect(), &slow, QStringLiteral("uia-cache"), &cache);
                cachedHit = uiaRectAt(nullptr, fixture.window, editRect.center(), window,
                                      QRect(), nullptr, QString(), &cache);
                cachedGrow = uiaRectAt(nullptr, fixture.window, buttonRect.center(), window,
                                       buttonRect, nullptr, QString(), &cache);
                automation->Release();
            }
            CoUninitialize();
        }
    });
    QEventLoop cacheLoop;
    QObject::connect(cacheProbe, &QThread::finished, &cacheLoop, &QEventLoop::quit);
    cacheProbe->start();
    QTimer::singleShot(5000, &cacheLoop, &QEventLoop::quit);
    cacheLoop.exec();
    check(cacheProbe->isFinished(), "warm-cache probe finishes");
    cacheProbe->wait();
    delete cacheProbe;
    check(cachedHit == editRect, "a warm cache resolves a sibling control without COM");
    check(cachedGrow.contains(buttonRect) && cachedGrow != buttonRect,
          "a warm cache walks to the parent without COM");
    overlay.m_hoverHistory.clear();
    QWheelEvent expand(expectedEdit.center(), expectedEdit.center() + overlay.geometry().topLeft(),
        QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&overlay, &expand);
    waitResult();
    check(overlay.m_hover.contains(expectedEdit) && overlay.m_hover != expectedEdit,
          "element expansion returns a strictly containing region");
    QWheelEvent shrink(expectedEdit.center(), expectedEdit.center() + overlay.geometry().topLeft(),
        QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&overlay, &shrink);
    check(overlay.m_hover == expectedEdit, "wheel contraction restores the previous finer element");
    // The current frame is the click target. A fresh query here would undo a
    // wheel-expanded parent and lock whatever happens to be under the cursor.
    QMouseEvent lockPress(QEvent::MouseButtonPress, expectedEdit.center(),
                          expectedEdit.center() + overlay.geometry().topLeft(),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &lockPress);
    QMouseEvent lockRelease(QEvent::MouseButtonRelease, expectedEdit.center(),
                            expectedEdit.center() + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &lockRelease);
    check(overlay.m_sel == expectedEdit && overlay.m_mode == Adjusting,
          "a click locks the currently shown frame without waiting for a new query");
    overlay.resetSelection();
    overlay.m_hover = expectedEdit;
    overlay.m_hoverPaint = expectedEdit;
    const QPointer<WindowDetector> silent = overlay.m_detector;
    silent->blockSignals(true);                  // Simulate a provider that never supplies its result.
    overlay.cancelDetection();
    overlay.requestDetection(expectedEdit.center());
    QTimer rapidHover;
    rapidHover.setInterval(25);
    QObject::connect(&rapidHover, &QTimer::timeout, &overlay, [&] {
        if (overlay.m_detector == silent)
            overlay.requestDetection(overlay.m_detectionPoint == expectedEdit.center()
                ? expectedButton.center() : expectedEdit.center());
    });
    rapidHover.start();
    waitResult();
    rapidHover.stop();
    check(overlay.m_detector != silent && !overlay.m_hover.isEmpty(),
          "the watchdog replaces a silent worker and uses native geometry without blocking capture");
    overlay.m_hover = expectedEdit;
    overlay.requestDetection(expectedButton.center());
    const quint64 freshSequence = overlay.m_queryId;
    overlay.requestDetection(expectedEdit.center());
    overlay.handleDetectedRect(buttonRect, freshSequence);
    check(overlay.m_hover == expectedEdit, "superseded results stay rejected after replacing a worker");
    waitResult();
    if (silent) {
        silent->blockSignals(false);
        silent->stop();
        silent->wait();
        delete silent.data();
    }
    overlay.m_drag = H_New;
    overlay.m_pressPos = expectedButton.center();
    QCursor::setPos(expectedButton.center() + overlay.geometry().topLeft());
    overlay.requestDetection(expectedButton.center());
    overlay.m_waitingClick = true;
    overlay.m_drag = H_None;
    waitResult();
    check(overlay.m_sel == expectedButton && overlay.m_mode == Adjusting,
          "a click locks the fresh asynchronous element result");
    // Once locked, moving the mouse must not re-arm hover detection: the highlight would
    // otherwise chase the cursor and visually unlock the selection.
    QMouseEvent slideAfterLock(QEvent::MouseMove, expectedEdit.center(),
                               expectedEdit.center() + overlay.geometry().topLeft(),
                               Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &slideAfterLock);
    // Moving outside a locked selection intentionally cancels hover detection,
    // so the query id may reset; the selection and hover state must not move.
    check(!overlay.m_queryPending && overlay.m_hover.isNull() && overlay.m_sel == expectedButton,
          "the locked selection stays put while the mouse keeps moving");
    // The status bar steps aside while the selection is dragged and returns on release.
    overlay.m_drag = H_BR;
    overlay.m_pressSel = overlay.m_sel;
    overlay.m_pressPos = expectedButton.bottomRight();
    const QPoint grown = expectedButton.bottomRight() + QPoint(40, 30);
    QMouseEvent dragMove(QEvent::MouseMove, grown, grown + overlay.geometry().topLeft(),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &dragMove);
    check(!overlay.m_tools->isVisible() && overlay.m_inspector->isVisible()
              && overlay.m_sel != expectedButton,
           "region resizing hides the status bar but retains the precision magnifier");
    QMouseEvent dragRelease(QEvent::MouseButtonRelease, grown, grown + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &dragRelease);
    check(overlay.m_tools->isVisible() && overlay.m_tools->y() > overlay.m_sel.bottom(),
          "the status bar returns below the resized selection once the drag ends");
    QMouseEvent hoverAfter(QEvent::MouseMove, grown + QPoint(6, 0), grown + QPoint(6, 0) + overlay.geometry().topLeft(),
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &hoverAfter);
    check(overlay.m_inspector->isVisible(),
           "the color value card remains available inside a locked selection");
    // Instant annotation: a rectangle stroke commits, composes, and undoes.
    overlay.selectTool(ATool::Rect);
    const QRect annRect = overlay.m_sel.adjusted(12, 12, -12, -12);
    const QPoint annA = annRect.topLeft(), annB = annRect.bottomRight();
    QMouseEvent annPress(QEvent::MouseButtonPress, annA, annA + overlay.geometry().topLeft(),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &annPress);
    check(overlay.m_live && overlay.m_tools->isVisible() && overlay.m_toolOptions->isVisible(),
          "an annotation drag keeps the active toolbar and options visible");
    QMouseEvent annMove(QEvent::MouseMove, annB, annB + overlay.geometry().topLeft(),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &annMove);
    QMouseEvent annRelease(QEvent::MouseButtonRelease, annB, annB + overlay.geometry().topLeft(),
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &annRelease);
    check(!overlay.m_live && overlay.m_anns.size() == 1 && overlay.m_anns.first().rect == annRect
              && overlay.m_tools->isVisible(),
          "the annotation commits with the dragged geometry and the tool stays ready");
    const QImage composed = overlay.cropped().toImage();
    overlay.undoAnnotation();
    check(overlay.m_anns.isEmpty() && overlay.cropped().toImage() != composed,
          "undo removes the annotation from the composed capture");
    QKeyEvent escTool(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &escTool);
    check(overlay.m_atool == ATool::None && overlay.isVisible(),
          "escape drops the annotation tool before closing the overlay");
    // Layered windows (pinned screenshots) keep a self-drawn shadow inside
    // their window rect. The visible content must win over that transparent rim.
    WNDCLASSW layeredClass = windowClass;
    layeredClass.lpfnWndProc = layeredFixtureProc;
    layeredClass.lpszClassName = L"LensSnipLayeredFixture";
    layeredClass.hbrBackground = nullptr;
    check(RegisterClassW(&layeredClass) != 0, "register the layered detection fixture class");
    HWND layered = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        layeredClass.lpszClassName, L"Layered pin", WS_POPUP | WS_VISIBLE,
        available.center().x() - 172, available.center().y() - 112, 344, 224,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(layered != nullptr, "create a layered detection fixture");
    HRGN region = CreateRectRgn(12, 12, 332, 212);
    check(region && SetWindowRgn(layered, region, TRUE), "inset the layered hit-test region");
    SetLayeredWindowAttributes(layered, 0, 255, LWA_ALPHA);
    ShowWindow(layered, SW_SHOWNA);
    UpdateWindow(layered);
    const QRect layeredRaw = windowFrame(layered);
    // Capture fresh pixels with the fixture on screen; the main overlay's frozen
    // frame predates this window and would let desktop edges drive the refinement.
    overlay.hide();
    QVector<ScreenFrame> layeredFrames = freezeScreens();
    // Pixel refinement is covered by the synthetic-image assertion above; keep
    // these frames uniform and check the native OS hit-test trim here.
    for (ScreenFrame &frame : layeredFrames)
        if (!frame.image.isNull()) frame.image.fill(Qt::black);
    CaptureOverlay layeredOverlay(layeredFrames, {}, DetectionOptions(), false);
    layeredOverlay.setAttribute(Qt::WA_DeleteOnClose, false);
    const QRect trimmedNative = layeredOverlay.trimLayeredContent(layered, layeredRaw);
    if (trimmedNative.size() != layeredRaw.size() - QSize(24, 24))
        std::fprintf(stderr, "layered native trimmed=%dx%d full=%dx%d\n",
            trimmedNative.width(), trimmedNative.height(), layeredRaw.width(), layeredRaw.height());
    check(trimmedNative.size() == layeredRaw.size() - QSize(24, 24),
          "layered windows trim their transparent shadow margin");
    layeredOverlay.close();
    DestroyWindow(layered);
    UnregisterClassW(L"LensSnipLayeredFixture", GetModuleHandleW(nullptr));
    overlay.show();
    overlay.setWindowTargets({});
    overlay.m_hover = {};
    overlay.m_sel = {};
    bool pinned = false;
    QRect pinnedRegion;
    const QRect expectedPinnedRegion = overlay.globalScreenRect(expectedButton);
    QObject::connect(&overlay, &CaptureOverlay::grabbed, &overlay,
                     [&pinned, &pinnedRegion](const QPixmap &pix, const QRect &region) {
        pinned = !pix.isNull();
        pinnedRegion = region;
    });
    overlay.resetSelection();
    QMouseEvent press(QEvent::MouseButtonPress, expectedButton.center(), expectedButton.center() + overlay.geometry().topLeft(),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, expectedButton.center(), expectedButton.center() + overlay.geometry().topLeft(),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, expectedButton.center(), expectedButton.center() + overlay.geometry().topLeft(),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &press);
    QApplication::sendEvent(&overlay, &doubleClick);
    QApplication::sendEvent(&overlay, &release);
    waitResult();
    check(pinned && !overlay.isVisible(), "double-click pinning waits for the fresh element result");
    check(pinnedRegion == expectedPinnedRegion,
          "double-click pinning reports the locked selection's original global geometry");
    overlay.close();
    check(!overlay.isVisible() && !overlay.m_queryPending, "closing capture cancels pending detection");
    if (frames.size() > 1) {
        DetectedWindow cross(frames[0].pixelRect.united(frames[1].pixelRect));
        const QRect logical = logicalFromNative(frames, cross.native);
        if (QGuiApplication::screenAt(logical.center())) {
            overlay.setWindowTargets({cross});
            if (overlay.m_windowRects.first().native.size() != overlay.localScreenRect(logical).size())
                std::fprintf(stderr, "cross-screen local actual=%dx%d expected=%dx%d logical=(%d,%d %dx%d)\n",
                    overlay.m_windowRects.first().native.width(), overlay.m_windowRects.first().native.height(),
                    overlay.localScreenRect(logical).width(), overlay.localScreenRect(logical).height(),
                    logical.x(), logical.y(), logical.width(), logical.height());
            check(overlay.m_windowRects.first().native.size() == overlay.localScreenRect(logical).size(),
                  "non-maximized windows crossing screens retain their full rectangle");
            cross.maximized = true;
            overlay.setWindowTargets({cross});
            check(rectArea(overlay.m_windowRects.first().native) < rectArea(overlay.localScreenRect(logical)),
                  "maximized detection rectangles are clipped to the center screen");
        }
    }
    std::puts("PASS: window filtering, child geometry, MSAA controls, asynchronous detection, and cancellation");
    return 0;
}
