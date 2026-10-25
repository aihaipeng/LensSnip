#include "capture/capture_overlay.h"
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
#include <QSize>
#include <QString>
#include <QStringList>
#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QPainter>
#include <QKeyEvent>
#include <QLabel>
#include <QGraphicsView>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QClipboard>
#include <QMessageBox>
#include <QTimer>
#include <QMimeData>
#include <QEventLoop>
#include <QThread>
#include <QElapsedTimer>
#include <QPointer>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QWheelEvent>
#include <QThreadPool>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <cmath>

int CaptureOverlay::testOcr(const QString &previewPath)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("OCR phase: start");
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
    };
    OcrEngine::testAlgorithms();
    const QRect primary(0,0,1920,1040);
    const QSize sampleCard(1100,650);
    const QRect primaryCard = OcrDialog::screenCentered(primary,sampleCard);
    check(primaryCard.center()==primary.center() && primary.contains(primaryCard),
          "OCR/translation cards centre on the screen diagonal intersection");
    for (const QPoint offset : {QPoint(-1920,0),QPoint(1920,-1080)}) {
        const QRect screen = primary.translated(offset);
        const QRect card = OcrDialog::screenCentered(screen,sampleCard);
        check(card.center()==screen.center() && screen.contains(card),
              "screen centring follows the selected secondary monitor");
    }
    const QString missing = OcrEngine::assetError(QCoreApplication::applicationDirPath());
    if (!missing.isEmpty()) {
        QImage image(640,180,QImage::Format_RGB32); image.fill(Qt::white);
        const QRect screen=QGuiApplication::primaryScreen()->geometry();
        CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(),image.size()),QRect(QPoint(),image.size()),image}});
        overlay.setAttribute(Qt::WA_DeleteOnClose,false);
        QToolButton *button = nullptr;
        for (QWidget *bar : {overlay.m_tools, overlay.m_toolsTop,
                             overlay.m_toolsLeft, overlay.m_toolsRight})
            if (bar && !button)
                button = bar->findChild<QToolButton *>(QStringLiteral("ocrButton"));
        check(button && !button->isEnabled() && button->toolTip()==missing,
              "missing OCR assets disable the toolbar button with an actionable explanation");
        overlay.close();
        std::printf("UNAVAILABLE: %s\n",qPrintable(missing));
        return 0;
    }
    auto &engine = OcrEngine::instance();
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    auto fixture = [](qreal dpr, const QStringList &rows, const QVector<int> &sizes, int angle = 0) {
        QImage image(qRound(800*dpr),qRound(280*dpr),QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.scale(dpr,dpr);
        if (angle) { painter.translate(400,140); painter.rotate(angle); painter.translate(-400,-140); }
        painter.setPen(Qt::black);
        for (int i=0; i<rows.size(); ++i) {
            QFont font(QStringLiteral("Microsoft YaHei")); font.setPixelSize(sizes[i]); painter.setFont(font);
            painter.drawText(QPointF(36,rows.size()==1 ? 140 : 58+i*85),rows[i]);
        }
        painter.end();
        image.setDevicePixelRatio(dpr);
        return image;
    };
    const QStringList chinese{QStringLiteral("截图文字识别"),QStringLiteral("中文测试"),QStringLiteral("对照结果")};
    for (qreal dpr : {1.0,1.5,2.0}) {
        for (int scenario=0; scenario<4; ++scenario) {
            const QStringList rows = scenario==0 ? chinese : QStringList{QStringLiteral("Hello世界123")};
            const QImage image = fixture(dpr,rows,scenario==0 ? QVector<int>{16,24,40} : QVector<int>{36},
                                         scenario==2 ? 10 : scenario==3 ? -10 : 0);
            const OcrResult result = engine.recognize(image,cancel);
            std::printf("OCR dpr=%.1f case=%d rows=%lld elapsed=%lldms text=%s error=%s\n",dpr,scenario,
                        (long long)result.lines.size(),result.elapsed,qPrintable(result.text()),qPrintable(result.error));
            if (!previewPath.isEmpty()) image.save(previewPath+QStringLiteral("-%1-%2.png").arg(dpr).arg(scenario));
            check(result.error.isEmpty() && !result.cancelled,"real OCR pipeline finishes without an inference error");
            check(result.text() == rows.join(QLatin1Char('\n')),"real OCR output matches Chinese, mixed and rotated fixtures exactly");
        }
        QImage blank(qRound(800*dpr),qRound(280*dpr),QImage::Format_RGB32); blank.fill(Qt::white);
        const OcrResult empty = engine.recognize(blank,cancel);
        check(empty.error.isEmpty() && empty.lines.isEmpty(),"blank screenshots produce no recognized rows");
    }
    QImage dense = fixture(1,{QStringLiteral("中文测试中文测试中文测试")},{10});
    const OcrResult small = engine.recognize(dense,cancel);
    check(small.error.isEmpty(),"dense small text completes safely without assuming all characters are readable");
    QImage full(1920,1080,QImage::Format_RGB32); full.fill(Qt::white);
    {
        QPainter painter(&full); painter.setPen(Qt::black);
        QFont font(QStringLiteral("Microsoft YaHei")); font.setPixelSize(32); painter.setFont(font);
        for (int row=0; row<10; ++row) painter.drawText(QPoint(40,80+row*90),QStringLiteral("截图文字识别 HelloWorld123"));
    }
    const OcrResult performance=engine.recognize(full,cancel);
    check(performance.error.isEmpty() && performance.lines.size()==10,"1080p screenshot retains all ten text rows");
    std::printf("PERFORMANCE: 1920x1080 / ten rows / %lldms%s\n",performance.elapsed,
                performance.elapsed>3000 ? " / WARNING: exceeds the reference budget" : "");
    class Calibration : public QWidget {
    public:
        Calibration() : QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint) {}
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this); painter.fillRect(rect(),Qt::white); painter.setPen(Qt::black);
            QFont font(QStringLiteral("Microsoft YaHei")); font.setPixelSize(32); painter.setFont(font);
            painter.drawText(QPoint(24,62),QStringLiteral("截图文字识别"));
            painter.drawText(QPoint(24,132),QStringLiteral("HelloWorld123"));
        }
    } calibration;
    const QRect available=QGuiApplication::primaryScreen()->availableGeometry();
    calibration.setGeometry(QRect(available.topLeft()+QPoint(80,80),QSize(640,190)));
    calibration.show(); calibration.repaint();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QThread::msleep(100);
    RECT client{}; POINT origin{};
    const HWND hwnd=reinterpret_cast<HWND>(calibration.winId());
    check(GetClientRect(hwnd,&client) && ClientToScreen(hwnd,&origin),"live OCR fixture has valid native screen coordinates");
    const QRect liveNative(origin.x,origin.y,client.right,client.bottom);
    const QImage live=grabNative(liveNative);
    const OcrResult liveResult=engine.recognize(live,cancel);
    std::printf("LIVE OCR: %dx%d / %lldms / text=[%s] / error=[%s]\n",live.width(),live.height(),liveResult.elapsed,
                qPrintable(liveResult.text()),qPrintable(liveResult.error));
    for (const OcrLine &line : liveResult.lines) {
        std::printf("LIVE BOX: text=[%s]",qPrintable(line.text));
        for (const QPointF &point : line.box) std::printf(" (%.3f,%.3f)",point.x(),point.y());
        std::putchar('\n');
    }
    if (!previewPath.isEmpty()) check(live.save(previewPath+QStringLiteral("-live.png")),"save actual desktop OCR fixture");
    check(liveResult.error.isEmpty() && liveResult.text()==QStringLiteral("截图文字识别\nHelloWorld123"),
          "actual desktop pixels pass through detection and recognition with exact Chinese and English output");
    const QVector<ScreenFrame> liveFrames=freezeScreens();
    check(!liveFrames.isEmpty(),"freeze actual desktop frames for the OCR toolbar workflow");
    calibration.close();
    QImage invalid;
    check(!engine.recognize(invalid,cancel).error.isEmpty(),"empty images return an explicit OCR error");
    const QImage image = fixture(1,{QStringLiteral("截图文字识别")},{32});
    cancel->store(true);
    check(engine.recognize(image,cancel).cancelled,"pre-cancelled work does not load or infer");
    cancel->store(false);
    CaptureOverlay overlay(liveFrames);
    std::puts("OCR phase: overlay created");
    overlay.setAttribute(Qt::WA_DeleteOnClose,false);
    overlay.m_mode = Adjusting;
    overlay.m_sel = overlay.localScreenRect(logicalFromNative(liveFrames,liveNative));
    Annotation annotation;
    annotation.tool=ATool::Text; annotation.color=Qt::black; annotation.font=16;
    annotation.text=QStringLiteral("OCR2026");
    annotation.rect=QRect(overlay.m_sel.topLeft()+QPoint(24,153),QSize(300,35));
    overlay.m_anns.append(annotation);
    overlay.placeToolBars(); overlay.showToolBars();
    const QRect selection = overlay.m_sel;
    const QImage before = overlay.cropped().toImage();
    QToolButton *button = nullptr;
    for (QWidget *bar : {overlay.m_tools, overlay.m_toolsTop,
                         overlay.m_toolsLeft, overlay.m_toolsRight})
        if (bar && !button)
            button = bar->findChild<QToolButton *>(QStringLiteral("ocrButton"));
    check(button && button->isEnabled() && !button->toolTip().isEmpty(),"capture toolbar exposes a named usable OCR button");
    button->click();
    std::puts("OCR phase: card opened");
    QPointer<OcrDialog> dialog = overlay.m_ocrDialog;
    check(dialog && dialog->findChild<QSplitter *>(QStringLiteral("ocrSplit"))->count()==2,
          "OCR opens a two-pane screenshot and text comparison");
    check(!dialog->findChild<QToolButton *>(QStringLiteral("ocrLayout")),
          "the comparison has no manual layout switch");
    QApplication::processEvents();
    check(!overlay.isVisible() && !overlay.m_tools->isVisible() && !overlay.m_toolOptions->isVisible()
              && !overlay.m_inspector->isVisible() && dialog->windowModality()==Qt::NonModal && !QApplication::activeModalWidget(),
          "OCR hides every capture overlay and does not block windows outside the card");
    const QRect captureRect=overlay.globalScreenRect(selection);
    const QScreen *captureScreen=QGuiApplication::screenAt(captureRect.center());
    check(captureScreen && dialog->screen()==captureScreen
              && dialog->size()==OcrDialog::loadingSize()
              && dialog->geometry()==OcrDialog::screenCentered(captureScreen->geometry(),dialog->size())
              && dialog->windowTitle()==QStringLiteral("LensSnip OCR"),
          "LensSnip OCR opens its compact loading card at the selected screen centre");
    const HWND cardHwnd=reinterpret_cast<HWND>(dialog->winId());
    check(GetWindow(cardHwnd,GW_OWNER)==nullptr && !(GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW),
          "OCR is an independent taskbar window rather than a tool owned by a hidden capture overlay");
    QWidget desktopProbe(nullptr,Qt::Tool|Qt::FramelessWindowHint);
    desktopProbe.setGeometry(QRect(captureScreen->availableGeometry().bottomRight()-QPoint(90,90),QSize(80,80)));
    desktopProbe.show(); desktopProbe.raise(); desktopProbe.activateWindow();
    QApplication::processEvents();
    const HWND probeHwnd=reinterpret_cast<HWND>(desktopProbe.winId());
    POINT probePoint{20,20};
    check(ClientToScreen(probeHwnd,&probePoint) && WindowFromPoint(probePoint)==probeHwnd && IsWindowEnabled(probeHwnd),
          "an independent desktop window stays enabled and receives native hit testing while OCR is open");
    desktopProbe.close(); dialog->raise(); dialog->activateWindow();
    auto *text = dialog->findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *retry = dialog->findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    check(text->isReadOnly() && !retry->isEnabled() && dialog->findChildren<QComboBox *>().isEmpty(),
          "unified recognition has no language selector and disables only busy controls without blocking the UI");
    auto wait = [&] {
        QElapsedTimer timer; timer.start();
        while (dialog && !retry->isEnabled() && timer.elapsed()<30000) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QThread::msleep(10);
        }
        check(dialog && retry->isEnabled(),"asynchronous OCR finishes within the test timeout");
    };
    wait();
    const QString expectedText=QStringLiteral("截图文字识别\nHelloWorld123\nOCR2026");
    const QSize finalExpected=OcrDialog::preferredSize(captureScreen->availableGeometry(),false);
    check(dialog->size()==finalExpected
              && dialog->geometry()==OcrDialog::screenCentered(captureScreen->geometry(),dialog->size()),
          "finished OCR expands at the same screen centre");
    auto *ocrImage=dialog->findChild<QWidget *>(QStringLiteral("ocrImagePanel"));
    auto *ocrText=dialog->findChild<QWidget *>(QStringLiteral("ocrTextPanel"));
    check(dialog->findChild<QSplitter *>(QStringLiteral("ocrSplit"))->orientation()
              == (finalExpected.width()<1050 ? Qt::Vertical : Qt::Horizontal)
              && (dialog->findChild<QSplitter *>(QStringLiteral("ocrSplit"))->orientation()==Qt::Horizontal
                  ? ocrText->width()>ocrImage->width()
                  : ocrText->height()>ocrImage->height()),
          "card width, not capture shape, picks the OCR split and gives reading more room");
    check(!text->isReadOnly() && text->toPlainText()==expectedText,"OCR recognizes the composed screenshot including annotations and allows editing");
    auto *copy=dialog->findChild<QToolButton *>(QStringLiteral("ocrTextCopy"));
    auto *back=dialog->findChild<QToolButton *>(QStringLiteral("ocrBack"));
    auto *headerPin=dialog->findChild<QToolButton *>(QStringLiteral("ocrPin"));
    auto *format=dialog->findChild<QToolButton *>(QStringLiteral("ocrFormat"));
    auto *search=dialog->findChild<QToolButton *>(QStringLiteral("ocrSearch"));
    auto *status=dialog->findChild<QLabel *>(QStringLiteral("ocrStatus"));
    auto *topBar=dialog->findChild<QWidget *>(QStringLiteral("ocrToolbar"));
    auto *textBar=dialog->findChild<QWidget *>(QStringLiteral("ocrTextBar"));
    auto *imageBar=dialog->findChild<QWidget *>(QStringLiteral("ocrImageBar"));
    auto *imageCount=dialog->findChild<QLabel *>(QStringLiteral("ocrImageCount"));
    auto *textCount=dialog->findChild<QLabel *>(QStringLiteral("ocrTextCount"));
    auto *zoomView=static_cast<OcrImageView *>(dialog->findChild<QGraphicsView *>());
    auto *zoomLevel=dialog->findChild<QLabel *>(QStringLiteral("ocrZoomLevel"));
    auto *zoomIn=dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomIn"));
    auto *zoomOut=dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomOut"));
    const int beforeZoom=zoomView->percent();
    zoomIn->click();
    const bool wrappedText=text->lineWrapMode()==QPlainTextEdit::WidgetWidth;
    const bool larger=zoomView->percent()==beforeZoom+10
        && zoomLevel->text()==QStringLiteral("%1%").arg(beforeZoom+10);
    zoomOut->click();
    zoomOut->click();
    const bool smaller=zoomView->percent()==beforeZoom-10;
    auto *fitToggle=dialog->findChild<QToolButton *>(QStringLiteral("ocrFitToggle"));
    const bool fitInFooter=fitToggle && fitToggle->parentWidget()==imageBar
        && fitToggle->mapTo(imageBar,QPoint()).x()>zoomLevel->mapTo(imageBar,QPoint()).x()+zoomLevel->width()
        && imageCount->mapTo(imageBar,QPoint()).x()>fitToggle->mapTo(imageBar,QPoint()).x()+fitToggle->width()
        && fitToggle->size()==QSize(26,24);
    fitToggle->click();
    const bool fittedNow=zoomView->fitted()
        && fitToggle->toolTip()==OcrDialog::tr("原始大小 (1:1)");
    fitToggle->click();
    const bool actualNow=!zoomView->fitted() && zoomView->percent()==100
        && fitToggle->toolTip()==OcrDialog::tr("适应屏幕");
    check(wrappedText && larger && smaller && fitInFooter && fittedNow && actualNow,
          "text wraps to the fixed pane width; image tools toggle fit and one-to-one");
    const QString chars=QString::number(text->toPlainText().toUcs4().size());
    check(imageCount->text()==chars && textCount->text()==chars
              && !textCount->text().contains(QStringLiteral("字符")),
          "each module footer shows only its numeric character count");
    check(dialog->findChildren<QLabel *>(QStringLiteral("cardSectionTitle")).isEmpty(),
          "the screenshot/original/result section captions are gone");
    auto *textZoomIn=dialog->findChild<QToolButton *>(QStringLiteral("ocrTextZoomIn"));
    auto *textZoomOut=dialog->findChild<QToolButton *>(QStringLiteral("ocrTextZoomOut"));
    auto *textZoomLevel=dialog->findChild<QLabel *>(QStringLiteral("ocrTextZoomLevel"));
    const qreal beforeTextFont=text->font().pointSizeF();
    textZoomIn->click();
    const bool textLarger=text->font().pointSizeF()>beforeTextFont
        && textZoomLevel->text()==QStringLiteral("110%");
    textZoomOut->click();
    const bool textBack=qAbs(text->font().pointSizeF()-beforeTextFont)<0.01
        && textZoomLevel->text()==QStringLiteral("100%");
    const QPointF wheelPos(text->viewport()->rect().center());
    QWheelEvent wheelUp(wheelPos,text->viewport()->mapToGlobal(wheelPos.toPoint()),
                        QPoint(),QPoint(0,120),Qt::NoButton,Qt::ControlModifier,
                        Qt::NoScrollPhase,false);
    QApplication::sendEvent(text->viewport(),&wheelUp);
    const bool wheelLarger=text->font().pointSizeF()>beforeTextFont
        && textZoomLevel->text()==QStringLiteral("110%");
    QWheelEvent wheelDown(wheelPos,text->viewport()->mapToGlobal(wheelPos.toPoint()),
                          QPoint(),QPoint(0,-120),Qt::NoButton,Qt::ControlModifier,
                          Qt::NoScrollPhase,false);
    QApplication::sendEvent(text->viewport(),&wheelDown);
    const bool wheelBack=qAbs(text->font().pointSizeF()-beforeTextFont)<0.01
        && textZoomLevel->text()==QStringLiteral("100%");
    check(textLarger && textBack && wheelLarger && wheelBack
              && !dialog->findChild<QToolButton *>(QStringLiteral("ocrFit")),
          "text footer +/- and Ctrl+wheel change only that module font; the old fit control is gone");
    const int textBottom=text->mapTo(dialog,QPoint()).y()+text->height();
    const bool actionsAtRight = topBar && format && search
        && format->parentWidget()==topBar && search->parentWidget()==topBar
        && retry->parentWidget()==topBar && status->parentWidget()==topBar
        && format->mapTo(topBar,QPoint()).x()<search->mapTo(topBar,QPoint()).x()
        && search->mapTo(topBar,QPoint()).x()<retry->mapTo(topBar,QPoint()).x()
        && retry->mapTo(topBar,QPoint()).x()+retry->width()==topBar->width();
    check(retry->text()==OcrDialog::tr("重新识别") && !retry->icon().isNull() && !copy->icon().isNull()
              && textBar && imageBar && imageBar->isVisible() && textBar->isVisible()
              && textBar->mapTo(dialog,QPoint()).y()>=textBottom
              && copy->toolTip()==OcrDialog::tr("复制识别文本")
              && back && headerPin && back->parentWidget()==headerPin->parentWidget()
              && back->mapTo(dialog,QPoint()).x()<headerPin->mapTo(dialog,QPoint()).x()
              && actionsAtRight && status->text().isEmpty()
              && text->toolTip().isEmpty(),
          "top actions sit right; each module has its own footer copy/zoom/count and no section captions");
    if (!previewPath.isEmpty()) check(dialog->grab().save(previewPath+QStringLiteral("-comparison.png")),"save comparison window preview");
    check(qAbs(text->font().pointSizeF()-qMax(11.25,QApplication::font().pointSizeF()))<0.01
              && qAbs(dialog->font().pointSizeF()-QApplication::font().pointSizeF())<0.01
              && copy->width()==26 && copy->height()==24 && retry->height()==32
              && copy->iconSize()==QSize(20,20) && textBar->height()==28
              && fitToggle->size()==QSize(26,24),
          "OCR keeps the approved type scale and compact footer targets");
    // Recognized characters are selectable on the capture, and text-pane selections light them up.
    const QString wantedLine=QStringLiteral("HelloWorld123");
    const int glyphTotal=QString(expectedText).remove(QLatin1Char('\n')).size();
    bool ordered=zoomView->glyphCount()==glyphTotal;
    for (int i=1;ordered && i<zoomView->glyphCount();++i) {
        const QRectF a=zoomView->glyphPolygon(i-1).boundingRect(), b=zoomView->glyphPolygon(i).boundingRect();
        ordered = b.top()>a.center().y() || b.left()>=a.left();   // Left to right inside a row, rows top to bottom.
    }
    check(ordered,"every recognized character gets its own box on the capture, in reading order");
    {
        QTextCursor pick(text->document());
        pick.setPosition(text->toPlainText().indexOf(wantedLine));
        pick.setPosition(pick.position()+5,QTextCursor::KeepAnchor);   // "Hello"
        text->setTextCursor(pick);
    }
    const QVector<int> lit=zoomView->highlighted();
    const QList<QTextEdit::ExtraSelection> textMarks=text->extraSelections();
    const int helloFirst=QStringLiteral("截图文字识别").size();
    check(lit==QVector<int>({helloFirst,helloFirst+1,helloFirst+2,helloFirst+3,helloFirst+4})
              && !textMarks.isEmpty()
              && textMarks.first().format.background().color()==QColor(0x32,0x34,0x5b),
          "selecting text in the OCR pane highlights exactly those characters with a text selection");
    auto *title = dialog->findChild<QLabel *>(QStringLiteral("ocrTitle"));
    const bool selectedBefore = text->textCursor().hasSelection();
    if (title) {
        const QPoint at = title->rect().center();
        QMouseEvent outside(QEvent::MouseButtonPress,QPointF(at),QPointF(title->mapToGlobal(at)),
                            Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(title,&outside);
    }
    check(selectedBefore && title && !text->textCursor().hasSelection()
              && text->extraSelections().isEmpty() && zoomView->highlighted().isEmpty(),
          "clicking outside the OCR text clears the selection and capture highlight");
    const auto viewMouse=[&](QEvent::Type type,int glyph,Qt::MouseButtons buttons) {
        const QPoint at=zoomView->mapFromScene(zoomView->glyphPolygon(glyph).boundingRect().center());
        QMouseEvent event(type,QPointF(at),QPointF(zoomView->viewport()->mapToGlobal(at)),
                          type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,buttons,Qt::NoModifier);
        QApplication::sendEvent(zoomView->viewport(),&event);
    };
    viewMouse(QEvent::MouseMove,helloFirst+1,Qt::NoButton);
    const bool hoverBeam=zoomView->viewport()->hasMouseTracking() && zoomView->viewport()->cursor().shape()==Qt::IBeamCursor;
    const QPointF centreBefore=zoomView->mapToScene(zoomView->viewport()->rect().center());
    viewMouse(QEvent::MouseButtonPress,helloFirst+5,Qt::LeftButton);        // "W"
    viewMouse(QEvent::MouseMove,helloFirst+9,Qt::LeftButton);
    const bool dragBeam=zoomView->viewport()->cursor().shape()==Qt::IBeamCursor;
    viewMouse(QEvent::MouseMove,helloFirst+12,Qt::LeftButton);              // "3"
    viewMouse(QEvent::MouseButtonRelease,helloFirst+12,Qt::NoButton);
    check(hoverBeam && dragBeam,"hovering and dragging over capture text show the text cursor, not the hand");
    const bool dragged=zoomView->selectedText()==QStringLiteral("World123")
        && zoomView->mapToScene(zoomView->viewport()->rect().center())==centreBefore;
    QApplication::clipboard()->clear();
    zoomView->setFocus();
    QKeyEvent copyKey(QEvent::KeyPress,Qt::Key_C,Qt::ControlModifier);
    QApplication::sendEvent(zoomView,&copyKey);
    check(dragged && QApplication::clipboard()->text()==QStringLiteral("World123") && status->text().isEmpty(),
          "dragging across capture text selects it without panning; Ctrl+C copies it without a copy toast");
    const bool exactMirror=text->textCursor().hasSelection()
        && text->textCursor().selectedText()==QStringLiteral("World123")
        && text->extraSelections().isEmpty() && zoomView->highlighted().isEmpty();
    check(exactMirror,"selecting capture text highlights exactly those characters in the OCR pane");
    const QPoint textAt=text->viewport()->rect().center();
    QMouseEvent textPress(QEvent::MouseButtonPress,QPointF(textAt),QPointF(text->viewport()->mapToGlobal(textAt)),
                          Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(text->viewport(),&textPress);
    QMouseEvent textRelease(QEvent::MouseButtonRelease,QPointF(textAt),QPointF(text->viewport()->mapToGlobal(textAt)),
                            Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(text->viewport(),&textRelease);
    check(exactMirror && !text->textCursor().hasSelection() && text->extraSelections().isEmpty()
              && zoomView->selectedText().isEmpty(),
          "clicking the OCR text drops the capture selection and its character highlight");
    viewMouse(QEvent::MouseButtonPress,0,Qt::LeftButton);
    viewMouse(QEvent::MouseMove,glyphTotal-1,Qt::LeftButton);
    viewMouse(QEvent::MouseButtonRelease,glyphTotal-1,Qt::NoButton);
    QString mirroredText=text->textCursor().selectedText();
    mirroredText.replace(QChar(0x2029),QLatin1Char('\n'));
    check(zoomView->selectedText()==expectedText && mirroredText==expectedText,
          "a selection across rows keeps the recognized line breaks in both panes");
    viewMouse(QEvent::MouseButtonPress,3,Qt::LeftButton);
    viewMouse(QEvent::MouseButtonRelease,3,Qt::NoButton);
    check(zoomView->selectedText().isEmpty() && !text->textCursor().hasSelection()
              && text->extraSelections().isEmpty(),
          "a click on text without dragging clears both panes");
    const int zoomBeforeDouble=zoomView->percent();
    {
        const QPoint at=zoomView->mapFromScene(zoomView->glyphPolygon(helloFirst+2).boundingRect().center());
        QMouseEvent twice(QEvent::MouseButtonDblClick,QPointF(at),QPointF(zoomView->viewport()->mapToGlobal(at)),
                          Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(zoomView->viewport(),&twice);
    }
    check(zoomView->selectedText()==wantedLine && text->textCursor().selectedText()==wantedLine
              && zoomView->percent()==zoomBeforeDouble,
          "double-clicking capture text selects its whole line without refitting");
    zoomView->selectRange(-1,-1);
    check(!text->textCursor().hasSelection() && text->extraSelections().isEmpty(),
          "clearing the capture selection clears the linked OCR characters");
    if (!previewPath.isEmpty()) {
        QTextCursor pick(text->document());
        pick.setPosition(text->toPlainText().indexOf(wantedLine));
        pick.setPosition(pick.position()+wantedLine.size(),QTextCursor::KeepAnchor);
        text->setTextCursor(pick);
        QApplication::processEvents();
        check(dialog->grab().save(previewPath+QStringLiteral("-highlight.png")),"save text highlight preview");
        text->moveCursor(QTextCursor::End);
    }
    const auto contrast = [](const QColor &a,const QColor &b) {
        const auto luminance = [](const QColor &color) {
            const auto linear=[](double value) { return value<=0.04045 ? value/12.92 : std::pow((value+0.055)/1.055,2.4); };
            return 0.2126*linear(color.redF())+0.7152*linear(color.greenF())+0.0722*linear(color.blueF());
        };
        const double first=luminance(a),second=luminance(b);
        return (qMax(first,second)+0.05)/(qMin(first,second)+0.05);
    };
    check(contrast(QColor("#f4f5f8"),QColor("#0c0d10"))>=7
              && contrast(QColor("#9c9da1"),QColor("#0c0d10"))>=4.5
              && contrast(QColor("#5e6ad2"),QColor("#18191d"))>=3
              && contrast(QColor("#f4f5f8"),QColor("#32345b"))>=4.5,
          "card text, muted metadata, accent controls and text selection meet their contrast budgets");
    const QList<QWidget *> tabOrder{back,
        dialog->findChild<QToolButton *>(QStringLiteral("ocrPin")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrMinimize")),dialog->findChild<QToolButton *>(QStringLiteral("ocrMaximize")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrClose")),dialog->findChild<QGraphicsView *>(),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrCopyImage")),dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomIn")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomOut")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrFitToggle")),
        text,copy,dialog->findChild<QToolButton *>(QStringLiteral("ocrTextZoomIn")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrTextZoomOut")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrFormat")),dialog->findChild<QToolButton *>(QStringLiteral("ocrSearch")),retry};
    tabOrder.first()->setFocus();
    for (int i=1;i<tabOrder.size();++i) {
        QKeyEvent tab(QEvent::KeyPress,Qt::Key_Tab,Qt::NoModifier);
        QApplication::sendEvent(QApplication::focusWidget(),&tab);
        check(QApplication::focusWidget()==tabOrder[i],"Tab follows the visual window-actions, image and text-actions order without a keyboard trap");
    }
    const QSize normalSize=dialog->size();
    dialog->resize(dialog->minimumSize()); QApplication::processEvents();
    const auto controlsFit = [&] {
        const auto inBar = [](QWidget *bar, QWidget *widget) {
            return bar && widget && bar->rect().contains(QRect(widget->mapTo(bar,QPoint()),widget->size()));
        };
        return dialog->rect().contains(QRect(topBar->mapTo(dialog,QPoint()),topBar->size()))
            && inBar(topBar,format) && inBar(topBar,search) && inBar(topBar,retry)
            && inBar(textBar,copy) && inBar(textBar,textZoomIn) && inBar(textBar,textZoomOut)
            && inBar(textBar,textZoomLevel) && inBar(textBar,textCount)
            && copy->mapTo(textBar,QPoint()).x()<textZoomIn->mapTo(textBar,QPoint()).x()
            && textZoomIn->mapTo(textBar,QPoint()).x()<textZoomOut->mapTo(textBar,QPoint()).x()
            && textCount->mapTo(textBar,QPoint()).x()+textCount->width()==textBar->width()
            && text->height()>100;
    };
    if (dialog->size()!=dialog->minimumSize() || !controlsFit())
        std::fprintf(stderr,"MIN: dialog=%dx%d min=%dx%d hint=%dx%d text-height=%d copy=%dx%d\n",
            dialog->width(),dialog->height(),dialog->minimumWidth(),dialog->minimumHeight(),
            dialog->minimumSizeHint().width(),dialog->minimumSizeHint().height(),text->height(),
            copy->width(),copy->height());
    check(dialog->size()==dialog->minimumSize() && controlsFit(),"minimum OCR window retains both panes and every footer control without overlap");
    if (!previewPath.isEmpty()) check(dialog->grab().save(previewPath+QStringLiteral("-compact.png")),"save minimum-size OCR preview");
    QVector<QPair<QWidget *,QFont>> savedFonts;
    for (QWidget *widget:dialog->findChildren<QWidget *>())
        if (qobject_cast<QLabel *>(widget) || qobject_cast<QToolButton *>(widget)
            || qobject_cast<QPlainTextEdit *>(widget))
            savedFonts.append({widget,widget->font()});
    for (const auto &entry:savedFonts) {
        QFont largeFont=entry.second; largeFont.setPointSizeF(largeFont.pointSizeF()*1.25);
        entry.first->setFont(largeFont);
    }
    for (int pass=0;pass<3;++pass) QApplication::processEvents();
    const auto originalPointSize = [&savedFonts](QWidget *widget) {
        for (const auto &entry : savedFonts) if (entry.first == widget) return entry.second.pointSizeF();
        return 0.0;
    };
    check(qAbs(retry->font().pointSizeF() - originalPointSize(retry) * 1.25) < 0.01
              && qAbs(text->font().pointSizeF() - originalPointSize(text) * 1.25) < 0.01,
          "the enlarged-font preview increases both interface and reading fonts by exactly 25 percent");
    if (!controlsFit()) std::fprintf(stderr,"LAYOUT: dialog=%dx%d panel=%dx%d retry=(%d,%d %dx%d) copy=(%d,%d %dx%d) text-height=%d\n",
        dialog->width(),dialog->height(),retry->parentWidget()->width(),retry->parentWidget()->height(),
        retry->x(),retry->y(),retry->width(),retry->height(),
        copy->x(),copy->y(),copy->width(),copy->height(),text->height());
    check(controlsFit(),"OCR controls tolerate an application-level twenty-five percent font enlargement at minimum size");
    if (!previewPath.isEmpty()) check(dialog->grab().save(previewPath+QStringLiteral("-large-font.png")),"save enlarged-interface-font OCR preview");
    for (const auto &entry:savedFonts) entry.first->setFont(entry.second);
    dialog->resize(normalSize); QApplication::processEvents();
    text->selectAll(); text->insertPlainText(QStringLiteral("校对后的文本"));
    auto *saved = new QMimeData;
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (const QMimeData *data=clipboard->mimeData()) for (const QString &format:data->formats()) saved->setData(format,data->data(format));
    copy->click();
    const bool copied = clipboard->text()==QStringLiteral("校对后的文本");
    clipboard->setMimeData(saved);
    check(copied,"copy-all outputs the edited text and leaves the comparison open");
    QTimer::singleShot(0,dialog,[dialog] {
        for (QWidget *widget:QApplication::topLevelWidgets())
            if (auto *question=qobject_cast<QMessageBox *>(widget))
                if (question->parentWidget()==dialog) question->button(QMessageBox::No)->click();
    });
    retry->click();
    check(text->toPlainText()==QStringLiteral("校对后的文本") && !text->isReadOnly() && retry->isEnabled(),
          "declining retry preserves manual corrections and does not start inference");
    QTimer::singleShot(0,dialog,[dialog] {
        for (QWidget *widget:QApplication::topLevelWidgets())
            if (auto *question=qobject_cast<QMessageBox *>(widget))
                if (question->parentWidget()==dialog) question->button(QMessageBox::Yes)->click();
    });
    retry->click(); wait();
    check(text->toPlainText()==expectedText,"retry recognizes the same retained screenshot");
    auto *maximize=dialog->findChild<QToolButton *>(QStringLiteral("ocrMaximize"));
    maximize->click(); QApplication::processEvents();
    QElapsedTimer maximizeTimer; maximizeTimer.start();
    while (dialog->geometry()!=dialog->screen()->availableGeometry() && maximizeTimer.elapsed()<2000) {
        QApplication::processEvents(); QThread::msleep(10);
    }
    check(dialog->m_maximized && maximize->toolTip()==QStringLiteral("还原")
              && dialog->geometry()==dialog->screen()->availableGeometry(),"maximize fills the work area and changes to a restore control");
    // Maximized by geometry: the edges still resize and empty chrome still drags.
    const auto mouse = [&](QEvent::Type type,const QPoint &local,Qt::MouseButtons buttons) {
        QMouseEvent event(type,QPointF(local),QPointF(dialog->mapToGlobal(local)),
                          type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,buttons,Qt::NoModifier);
        QApplication::sendEvent(dialog,&event);
    };
    const QRect maxed=dialog->geometry();
    const QPoint rightEdge(dialog->width()-3,dialog->height()/2);
    mouse(QEvent::MouseMove,rightEdge,Qt::NoButton);
    const bool edgeCursor=dialog->cursor().shape()==Qt::SizeHorCursor;
    mouse(QEvent::MouseButtonPress,rightEdge,Qt::LeftButton);
    mouse(QEvent::MouseMove,rightEdge-QPoint(120,0),Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease,rightEdge-QPoint(120,0),Qt::NoButton);
    QApplication::processEvents();
    check(edgeCursor && !dialog->m_maximized && dialog->width()==maxed.width()-120 && dialog->x()==maxed.x()
              && maximize->toolTip()==QStringLiteral("最大化"),
          "a maximized card still resizes from its edge and drops the maximized state");
    const QPoint titleSpot=dialog->m_header->geometry().center();
    const QPoint startPos=dialog->pos();
    mouse(QEvent::MouseButtonPress,titleSpot,Qt::LeftButton);
    mouse(QEvent::MouseMove,titleSpot+QPoint(40,30),Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease,titleSpot+QPoint(40,30),Qt::NoButton);
    check(dialog->pos()==startPos+QPoint(40,30),"dragging the empty title bar moves the card");
    QMouseEvent twice(QEvent::MouseButtonDblClick,QPointF(titleSpot),QPointF(dialog->mapToGlobal(titleSpot)),
                      Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(dialog,&twice);
    check(dialog->m_maximized && dialog->geometry()==maxed,"double-clicking the title bar maximizes again");
    const QPoint maxSpot=dialog->m_header->geometry().center();
    mouse(QEvent::MouseButtonPress,maxSpot,Qt::LeftButton);
    mouse(QEvent::MouseMove,maxSpot+QPoint(25,0),Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease,maxSpot+QPoint(25,0),Qt::NoButton);
    check(dialog->pos()==maxed.topLeft()+QPoint(25,0),"a maximized card can still be dragged");
    const QPoint inner(dialog->width()/2,dialog->height()-3), deep(dialog->width()/2,dialog->height()-12);
    check(dialog->resizeEdges(inner)==Qt::BottomEdge && !dialog->resizeEdges(deep)
              && dialog->resizeEdges(QPoint(2,2))==(Qt::LeftEdge|Qt::TopEdge),
          "the resize band covers the outer half of the edge padding and combines corners");
    maximize->click(); QApplication::processEvents();
    check(!dialog->m_maximized && text->toPlainText()==expectedText,"restoring the card keeps the recognized text");
    dialog->findChild<QToolButton *>(QStringLiteral("ocrMinimize"))->click(); QApplication::processEvents();
    check(dialog->isMinimized() && !overlay.isVisible(),"minimizing OCR does not restore the global frozen overlay");
    dialog->showNormal(); QApplication::processEvents();
    const auto existingWindows=QApplication::topLevelWidgets();
    auto *pin=dialog->findChild<QToolButton *>(QStringLiteral("ocrPin"));
    const QRect cardGeometry=dialog->geometry();
    check(pin->isCheckable() && !pin->isChecked() && !(GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOPMOST),
          "OCR starts unpinned with an explicit topmost toggle");
    pin->click(); QApplication::processEvents();
    check(pin->isChecked() && (GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOPMOST)
              && pin->toolTip()==QStringLiteral("取消置顶 OCR 卡片") && pin->accessibleName()==pin->toolTip()
              && dialog->geometry()==cardGeometry && reinterpret_cast<HWND>(dialog->winId())==cardHwnd
              && text->toPlainText()==expectedText && dialog->isVisible() && !overlay.isVisible(),
          "pin makes the entire OCR card topmost without changing its handle, geometry, text or capture state");
    check(QApplication::topLevelWidgets().size()==existingWindows.size(),"OCR pin does not create an image window");
    dialog->findChild<QToolButton *>(QStringLiteral("ocrMinimize"))->click(); QApplication::processEvents();
    dialog->showNormal(); QApplication::processEvents();
    maximize->click(); QApplication::processEvents();
    maximize->click(); QApplication::processEvents();
    check(pin->isChecked() && (GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOPMOST) && text->toPlainText()==expectedText,
          "topmost survives minimize restore and maximize restore without discarding the OCR result");
    pin->click(); QApplication::processEvents();
    check(!pin->isChecked() && !(GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOPMOST)
              && pin->toolTip()==QStringLiteral("置顶 OCR 卡片") && dialog->geometry()==cardGeometry,
          "clicking pin again returns the OCR card to normal window stacking");
    dialog->findChild<QToolButton *>(QStringLiteral("ocrClose"))->click();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    check(!overlay.isVisible() && overlay.m_sel==selection && overlay.cropped().toImage()==before,
          "closing OCR ends the capture and leaves the selection and output untouched");
    overlay.show(); overlay.placeToolBars(); overlay.showToolBars();
    button->click();
    dialog = overlay.m_ocrDialog;
    QApplication::processEvents();
    dialog->findChild<QToolButton *>(QStringLiteral("ocrClose"))->click();
    QApplication::processEvents();                       // Runs the queued capture close before showing it again.
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    QThreadPool::globalInstance()->waitForDone();
    check(!engine.busy.load() && !overlay.isVisible(),"closing during recognition cancels cooperatively and releases the single-job slot");
    overlay.show(); overlay.placeToolBars(); overlay.showToolBars();
    for (QScreen *screen:QGuiApplication::screens()) {
        const QRect area=screen->availableGeometry();
        const QRect anchor(area.topLeft()+QPoint(30,30),QSize(100,80));
        OcrDialog positioned(image,anchor);
        positioned.show();
        check(positioned.screen()==screen
                  && positioned.geometry()==OcrDialog::screenCentered(screen->geometry(),positioned.size()),
              "actual OCR windows select each monitor from the capture anchor and centre there");
        positioned.reject();
        QApplication::processEvents();
    }
    const QString mixedText=QStringLiteral("截图 繁體 日本語 Hello été");
    OcrDialog foreign(fixture(1,{mixedText},{36}));
    foreign.show();
    QApplication::processEvents();
    QElapsedTimer foreignTimer; foreignTimer.start();
    auto *foreignRetry=foreign.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    while (!foreignRetry->isEnabled() && foreignTimer.elapsed()<30000) {
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); QThread::msleep(10);
    }
    foreignRetry->click(); foreignTimer.restart();
    while (!foreignRetry->isEnabled() && foreignTimer.elapsed()<30000) {
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); QThread::msleep(10);
    }
    check(foreignRetry->isEnabled() && foreign.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"))->toPlainText()==mixedText
              && foreign.findChildren<QComboBox *>().isEmpty(),
          "one model recognizes and retries mixed scripts without a language selector");
    foreign.reject();
    engine.busy.store(true);
    OcrDialog blocked(image); blocked.show();
    QApplication::processEvents();
    check(blocked.findChild<QLabel *>(QStringLiteral("ocrStatus"))->text().contains(QStringLiteral("上一项"))
              && blocked.findChild<QToolButton *>(QStringLiteral("ocrRetry"))->isEnabled(),
          "a second request does not queue another job while the process-wide slot is occupied");
    blocked.reject(); engine.busy.store(false);
    if (!previewPath.isEmpty()) {
        engine.busy.store(true);
        OcrDialog longCard(image);
        longCard.show();
        QApplication::processEvents();
        QString longText;
        for (int i=0;i<80;++i) longText += QStringLiteral("第 %1 行：这是一段用于测量结果框高度的长文本。\n").arg(i);
        auto *longEdit=longCard.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
        longEdit->setPlainText(longText);
        longCard.applyLayout(true);
        QApplication::processEvents();
        const QSize longExpected=OcrDialog::preferredSize(longCard.screen()->availableGeometry(),false);
        check(longCard.size()==longExpected && longEdit->height()>300,
              "long OCR output stays in the fixed card and scrolls");
        check(longCard.grab().save(previewPath+QStringLiteral("-long.png")),"save long OCR preview");
        longCard.reject();
        engine.busy.store(false);
        QApplication::processEvents();
    }
    button->click();
    dialog=overlay.m_ocrDialog;
    back=dialog->findChild<QToolButton *>(QStringLiteral("ocrBack"));
    back->click();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    check(overlay.isVisible() && overlay.m_sel==selection && !overlay.m_ocrDialog,
          "back from OCR restores the live capture selection and toolbars");
    button->click();
    dialog=overlay.m_ocrDialog;
    dialog->findChild<QToolButton *>(QStringLiteral("ocrClose"))->click();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    check(!overlay.isVisible(),"closing OCR ends the capture rather than locking the desktop again");
    QThreadPool::globalInstance()->waitForDone();
    overlay.close();
    std::puts("PASS: offline OCR, exact native-pixel fixtures and comparison/edit/copy/retry/cancel workflow");
    return 0;
}
