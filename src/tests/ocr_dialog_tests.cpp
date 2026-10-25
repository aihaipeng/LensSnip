#include "text/cards/ocr_dialog.h"
#include "text/engines/translate_gateway.h"
#include <QApplication>
#include <QEventLoop>
#include <QMessageBox>
#include <QPainter>
#include <QElapsedTimer>
#include <QComboBox>
#include <QLabel>
#include "shared/settings.h"
#include <QSettings>
#include <QPlainTextEdit>
#include <QWheelEvent>
#include <QScrollBar>
#include <QPushButton>
#include <QTextDocument>
#include <QThreadPool>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QLineF>
#include <QNetworkProxy>
#include <cstdio>
#include <algorithm>

int OcrDialog::testLifetime()
{
    auto &engine = OcrEngine::instance();
    engine.busy.store(true);
    QImage image(320, 120, QImage::Format_RGB32);
    image.fill(Qt::white);
    OcrDialog dialog(image);
    dialog.show();
    QApplication::processEvents();
    engine.busy.store(false);
    auto *text = dialog.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *retry = dialog.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    const QString corrected = QStringLiteral("Retain these corrections");
    text->setReadOnly(false);
    text->setPlainText(corrected);
    text->document()->setModified(true);
    bool confirmed = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!question || question->parentWidget() != &dialog) return;
        confirmed = true;
        question->button(QMessageBox::Yes)->click();
        dialog.reject();
    });
    dialog.start();
    const bool passed = confirmed && !dialog.isVisible() && !engine.busy.load()
        && retry->isEnabled() && !text->isReadOnly() && text->toPlainText() == corrected;
    QThreadPool::globalInstance()->waitForDone();
    if (!passed) {
        std::fprintf(stderr, "FAIL: closing OCR during retry confirmation must not start work or alter corrections\n");
        return 1;
    }
    std::puts("PASS: closed OCR cards cannot restart recognition after retry confirmation");
    const bool formats =
        formatText(QStringLiteral("截图\n文字\nHello\nWorld"), TextFormat::JoinLines) == QStringLiteral("截图文字Hello World")
        && formatText(QStringLiteral("a b\tc\nd e"), TextFormat::RemoveSpaces) == QStringLiteral("abc\nde")
        && formatText(QStringLiteral("你好,世界!3.14"), TextFormat::FullWidth) == QStringLiteral("你好，世界！3.14")
        && formatText(QStringLiteral("你好，世界！。"), TextFormat::HalfWidth) == QStringLiteral("你好,世界!.");
    const bool layouts = OcrDialog::preferredSize(QRect(0,0,1920,1040),false)==QSize(1100,650)
        && OcrDialog::preferredSize(QRect(0,0,2560,1400),true)==QSize(1700,760)
        && OcrDialog::preferredSize(QRect(0,0,1280,720),true)==QSize(1100,650)
        && OcrDialog::preferredSize(QRect(0,0,1366,728),true)==QSize(1334,650)
        && OcrDialog::preferredSize(QRect(0,0,932,720),true)==QSize(900,650)
        && OcrDialog::preferredSize(QRect(0,0,931,720),true)==QSize(680,650)
        && OcrDialog::preferredSize(QRect(0,0,800,600),true)==QSize(680,568)
        && OcrDialog::preferredSize(QRect(0,0,1082,720),false)==QSize(1050,650)
        && OcrDialog::preferredSize(QRect(0,0,1081,720),false)==QSize(660,650)
        && OcrDialog::preferredSize(QRect(0,0,800,600),false)==QSize(660,568);
    const QImage portrait(240,480,QImage::Format_RGB32);
    OcrEngine::instance().busy.store(true);
    OcrDialog portraitOcr(portrait,{},false);
    portraitOcr.show();
    QApplication::processEvents();
    portraitOcr.showLoading(false);
    QApplication::processEvents();
    const QRect portraitArea = portraitOcr.screen()->availableGeometry();
    const QSize portraitPreferred = OcrDialog::preferredSize(portraitArea,false);
    const bool wideOcr = portraitPreferred.width() >= 1050;
    const QList<int> portraitSizes = portraitOcr.m_split->sizes();
    const bool paneWidths = portraitOcr.width()==portraitPreferred.width()
        && portraitOcr.m_split->orientation()==(wideOcr?Qt::Horizontal:Qt::Vertical)
        && portraitSizes.size()==2 && portraitSizes.value(1)>portraitSizes.value(0);
    const int portraitHeight = portraitOcr.height();
    portraitOcr.resize(800,portraitHeight);
    QApplication::processEvents();
    const QList<int> narrowSizes = portraitOcr.m_split->sizes();
    const bool narrowLayout = portraitOcr.m_split->orientation()==Qt::Vertical
        && narrowSizes.size()==2 && narrowSizes.value(1)>narrowSizes.value(0)
        && portraitOcr.height()==portraitHeight;
    portraitOcr.resize(portraitPreferred.width(),portraitHeight);
    QApplication::processEvents();
    const bool ocrResponsive = narrowLayout && portraitOcr.m_split->orientation()==(wideOcr?Qt::Horizontal:Qt::Vertical)
        && portraitOcr.height()==portraitHeight;
    portraitOcr.close();
    OcrEngine::instance().busy.store(false);
    QThreadPool::globalInstance()->waitForDone();
    OcrDialog finder(image);
    finder.show();
    QApplication::processEvents();
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();
    auto *edit = finder.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    edit->setReadOnly(false);
    edit->setPlainText(QStringLiteral("ab ab ab"));
    finder.showFind(true);
    finder.m_findEdit->setText(QStringLiteral("ab"));
    finder.m_replaceEdit->setText(QStringLiteral("x"));
    bool readable = edit->extraSelections().size() == 3;
    for (const QTextEdit::ExtraSelection &mark : edit->extraSelections())
        readable = readable && mark.format.foreground().color() == QColor(0x18, 0x18, 0x1b);
    const bool counted = finder.matches().size() == 3 && edit->extraSelections().size() == 3 && readable;
    finder.replaceOne();
    const bool one = edit->toPlainText() == QStringLiteral("x ab ab");
    finder.replaceAll();
    const bool all = edit->toPlainText() == QStringLiteral("x x x") && finder.matches().isEmpty();
    edit->undo();
    const bool undo = edit->toPlainText() == QStringLiteral("x ab ab");
    finder.hideFind();
    const bool hidden = !finder.m_find->isVisible() && edit->extraSelections().isEmpty();
    // Format menu: Auto first and checked by default, a chosen mode becomes the only check, Auto restores.
    finder.m_raw = QStringLiteral("a b\nc");
    edit->setPlainText(finder.m_raw);
    const QList<QAction *> modes = finder.m_format->menu()->actions();
    modes.at(2)->trigger();
    const bool chosen = edit->toPlainText() == QStringLiteral("ab\nc") && modes.at(2)->isChecked()
        && !modes.at(0)->isChecked();
    modes.at(0)->trigger();
    const bool restored = modes.at(0)->text() == QStringLiteral("自动") && modes.at(0)->isChecked()
        && edit->toPlainText() == finder.m_raw;
    // Wheel zoom without any prior mouse move keeps the point under the cursor in place.
    auto *view = finder.findChild<QGraphicsView *>();
    const QPoint at(view->viewport()->width() / 3, view->viewport()->height() / 3);
    const QPointF before = view->mapToScene(at);
    QWheelEvent wheel(at, view->viewport()->mapToGlobal(at), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(view->viewport(), &wheel);
    const bool anchored = QLineF(view->mapToScene(at), before).length() < 2 / view->transform().m11();
    // Mouse clicks must not focus (and outline) any card button.
    bool noClickFocus = true;
    for (auto *button : finder.findChildren<QToolButton *>())
        noClickFocus = noClickFocus && !(button->focusPolicy() & Qt::ClickFocus);
    const bool noLayout = !finder.findChild<QToolButton *>(QStringLiteral("ocrLayout"));
    auto *ocrImage = finder.findChild<QWidget *>(QStringLiteral("ocrImagePanel"));
    auto *ocrText = finder.findChild<QWidget *>(QStringLiteral("ocrTextPanel"));
    const bool horizontalOcr = finder.width() >= 1050;
    const bool tallText = ocrImage->height() > 0
        && (horizontalOcr ? ocrText->width() > ocrImage->width()
                          : ocrText->height() > ocrImage->height());
    finder.close();
    QThreadPool::globalInstance()->waitForDone();
    // Text quantity no longer changes the card size. Long output stays inside the same
    // panes and scrolls; translation keeps both reading panes equal.
    OcrEngine::instance().busy.store(true);
    const QImage wide(640,140,QImage::Format_RGB32);
    OcrDialog loadingCard(wide,{},false);
    loadingCard.show();
    const QScreen *loadingScreen = loadingCard.screen() ? loadingCard.screen() : QGuiApplication::primaryScreen();
    const bool loadingState = !loadingCard.m_loadingPage->isHidden() && loadingCard.m_content->isHidden()
        && loadingCard.size()==OcrDialog::loadingSize()
        && loadingCard.geometry()==OcrDialog::screenCentered(loadingScreen->geometry(),loadingCard.size());
    QApplication::processEvents();
    const QSize loadingFinal = OcrDialog::preferredSize(loadingScreen->availableGeometry(),false);
    const bool loadingEnds = loadingCard.m_loadingPage->isHidden() && loadingCard.m_content->isVisible()
        && loadingCard.size()==loadingFinal
        && loadingCard.geometry()==OcrDialog::screenCentered(loadingScreen->geometry(),loadingCard.size());
    loadingCard.close();
    OcrDialog longCard(wide,{},false), shortCard(wide,{},false);
    OcrDialog translatedA(wide,{},true), translatedB(wide,{},true);
    for (OcrDialog *card : {&longCard,&shortCard,&translatedA,&translatedB}) card->show();
    QApplication::processEvents();
    QString longText;
    for (int i = 0; i < 80; ++i) longText += QStringLiteral("第 %1 行：这是一段用于测量结果框高度的长文本。\n").arg(i);
    auto *longEdit = longCard.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *shortEdit = shortCard.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    longEdit->setPlainText(longText);
    shortEdit->setPlainText(QStringLiteral("短"));
    longCard.applyLayout(true); shortCard.applyLayout(true);
    QApplication::processEvents();
    const QSize ocrExpected = OcrDialog::preferredSize(longCard.screen()->availableGeometry(),false);
    const bool fixedSizes = longCard.size()==shortCard.size()
        && longCard.size()==ocrExpected
        && longEdit->height()==shortEdit->height()
        && longEdit->verticalScrollBar()->isVisible();
    auto *sourceA = translatedA.findChild<QPlainTextEdit *>(QStringLiteral("translateSourceText"));
    auto *resultA = translatedA.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *sourceB = translatedB.findChild<QPlainTextEdit *>(QStringLiteral("translateSourceText"));
    auto *resultB = translatedB.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    sourceA->setPlainText(QStringLiteral("Hello"));
    resultA->setPlainText(longText);
    sourceB->setPlainText(longText);
    resultB->setPlainText(QStringLiteral("Hello"));
    translatedA.applyLayout(true); translatedB.applyLayout(true);
    QApplication::processEvents();
    const bool translatedMax = translatedA.size()==translatedB.size()
        && translatedA.height()==OcrDialog::preferredSize(translatedA.screen()->availableGeometry(),true).height()
        && qAbs(translatedA.m_textSplit->sizes().value(0)-translatedA.m_textSplit->sizes().value(1))<=1
        && qAbs(translatedB.m_textSplit->sizes().value(0)-translatedB.m_textSplit->sizes().value(1))<=1;
    for (OcrDialog *card : {&longCard,&shortCard,&translatedA,&translatedB}) card->close();
    OcrEngine::instance().busy.store(false);
    QThreadPool::globalInstance()->waitForDone();
    if (!(formats && layouts && paneWidths && counted && one && all && undo && hidden && chosen && restored && anchored
          && noClickFocus && noLayout && tallText && fixedSizes && translatedMax && loadingState && loadingEnds
          && ocrResponsive)) {

        std::fprintf(stderr, "FAIL: widths portraitOcr=%d split=%d sizes=%d,%d finder=%d split=%d sizes=%d,%d; fixed long=%d short=%d max=%d responsive=%d loading=%d/%d; text formats %d, sizes %d, pane widths %d, find/replace %d%d%d%d%d, format menu %d%d, wheel anchor %d, click focus %d, no layout button %d, wide text %d\n",
                     portraitOcr.width(),portraitOcr.m_split->width(),portraitOcr.m_split->sizes().value(0),portraitOcr.m_split->sizes().value(1),
                     finder.width(),finder.m_split->width(),finder.m_split->sizes().value(0),finder.m_split->sizes().value(1),
                     longCard.height(),shortCard.height(),translatedA.height(),ocrResponsive,loadingState,loadingEnds,
                     formats, layouts, paneWidths, counted, one, all, undo, hidden, chosen, restored, anchored, noClickFocus, noLayout, tallText);
        return 1;
    }
    std::puts("PASS: text formats, format menu checks, cursor-anchored wheel zoom, width-driven split and find/replace with undo");
    return 0;
}

int OcrDialog::testTranslate()
{
    // Part 1: live smoke of the aggregating gateway as wired into the card -
    // two lines in, two aligned non-empty translations out (auto, target zh).
    TranslateGateway gateway;
    QEventLoop loop;
    TranslateResult out;
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    gateway.translate({QStringLiteral("The quick brown fox"), QStringLiteral("jumps over the lazy dog")},
                      QString(), QStringLiteral("zh"), TranslateGateway::Provider::Auto,
                      [&](const TranslateResult &result) { out = result; loop.quit(); });
    loop.exec();
    const bool aligned = out.code == TranslateResult::Error::None && out.lines.size() == 2
        && !out.lines.constFirst().isEmpty() && !out.lines.constLast().isEmpty();
    if (!aligned) {
        std::fprintf(stderr, "FAIL: gateway smoke: %s\n",
            qPrintable(out.code == TranslateResult::Error::None ? QStringLiteral("unaligned or empty output") : out.error));
        return 1;
    }
    std::printf("PASS: %s translated 2/2 lines aligned (%lldms)\n", qPrintable(out.provider), out.elapsed);

    // Invalid input must be reported as a failure, not delivered as an empty success.
    TranslateGateway validation;
    bool validationCalled = false;
    TranslateResult invalid;
    validation.translate({QString(5001, QLatin1Char('a'))}, QString(), QStringLiteral("zh"),
                         TranslateGateway::Provider::Edge,
                         [&](const TranslateResult &result) { invalid = result; validationCalled = true; });
    if (!validationCalled || invalid.code != TranslateResult::Error::Invalid || invalid.error.isEmpty()) {
        std::fprintf(stderr, "FAIL: oversized input was not rejected\n");
        return 1;
    }
    std::puts("PASS: oversized translation input is rejected");
    const QSize compact = OcrDialog::preferredSize(QRect(0, 0, 960, 520), true);
    if (compact.width() > 960 - 32 || compact.height() > 520 - 32) {
        std::fprintf(stderr, "FAIL: translate card prefers %dx%d on a 960x520 workspace\n",
                     compact.width(), compact.height());
        return 1;
    }
    std::puts("PASS: translate card fits a 200% 1080p workspace");

    // Part 2: the whole card chain - rendered text through recognition and the
    // gateway ends up as translated text in the pane.
    const QString loadError = OcrEngine::instance().load(QCoreApplication::applicationDirPath());
    if (!loadError.isEmpty()) {
        std::printf("SKIP: card chain: %s\n", qPrintable(loadError));
        return 2;   // Do not let a gate treat a missing model as a pass.
    }
    QImage image(640, 140, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        QFont font = painter.font();
        font.setPointSize(26);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("The quick brown fox jumps over the lazy dog"));
    }
    // The card reads the last choice from settings: start from defaults, restore after.
    LensSnipSettings settings;
    const QVariant savedProvider = settings.value(QStringLiteral("Translate/provider")),
                   savedSource = settings.value(QStringLiteral("Translate/source")),
                   savedTarget = settings.value(QStringLiteral("Translate/target"));
    settings.remove(QStringLiteral("Translate"));
    settings.setValue(QStringLiteral("Translate/provider"), int(TranslateGateway::Provider::Youdao));   // Old builds stored it.
    settings.sync();
    const auto restore = [&] {
        for (const auto &[key, value] : {std::pair(QStringLiteral("Translate/provider"), savedProvider),
                                         std::pair(QStringLiteral("Translate/source"), savedSource),
                                         std::pair(QStringLiteral("Translate/target"), savedTarget)}) {
            if (value.isValid()) settings.setValue(key, value);
            else settings.remove(key);
        }
    };
    OcrDialog dialog(image, {}, true);
    dialog.show();
    QElapsedTimer deadline;
    deadline.start();
    while (dialog.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"))->toPlainText().isEmpty()
           && deadline.elapsed() < 90000)
        QApplication::processEvents(QEventLoop::AllEvents, 200);
    auto *pane = dialog.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    const QString translated = pane->toPlainText();
    if (translated.isEmpty()) {
        restore();
        std::fprintf(stderr, "FAIL: card chain produced no translated text within 90s\n");
        return 1;
    }
    std::printf("PASS: card chain rendered %d chars of translated text\n", int(translated.size()));

    // Part 3: pickers. Engine list with Auto first, auto-detect source, zh target;
    // switching the engine re-translates with it, and the retry button reruns.
    auto *provider = dialog.findChild<QComboBox *>(QStringLiteral("translateProvider"));
    auto *source = dialog.findChild<QComboBox *>(QStringLiteral("translateSource"));
    auto *target = dialog.findChild<QComboBox *>(QStringLiteral("translateTarget"));
    auto *again = dialog.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    auto *translateAgain = dialog.findChild<QToolButton *>(QStringLiteral("translateAgain"));
    auto *viewButton = dialog.findChild<QToolButton *>(QStringLiteral("translateView"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("ocrStatus"));
    auto *resultBar = dialog.findChild<QWidget *>(QStringLiteral("translateResultBar"));
    auto *translateToolbar = dialog.findChild<QWidget *>(QStringLiteral("translateToolbar"));
    const auto settle = [&] {
        QElapsedTimer wait;
        wait.start();
        while ((!again->isEnabled() || pane->toPlainText().isEmpty()) && wait.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 200);
    };
    const bool lists = provider && source && target && again && translateAgain && viewButton && resultBar && translateToolbar
        && provider->count() == 8
        && provider->itemData(0).toInt() == int(TranslateGateway::Provider::Auto) && provider->currentIndex() == 0
        && source->itemData(0).toString().isEmpty() && target->currentData().toString() == QStringLiteral("zh")
        && again->isEnabled() && status->text().isEmpty()
        && again->parentWidget() == translateToolbar && translateAgain->parentWidget() == translateToolbar
        && viewButton->parentWidget() == translateToolbar
        && source->mapTo(translateToolbar,QPoint()).x() < target->mapTo(translateToolbar,QPoint()).x()
        && target->mapTo(translateToolbar,QPoint()).x() < provider->mapTo(translateToolbar,QPoint()).x()
        && provider->mapTo(translateToolbar,QPoint()).x() < viewButton->mapTo(translateToolbar,QPoint()).x()
        && viewButton->mapTo(translateToolbar,QPoint()).x() < translateAgain->mapTo(translateToolbar,QPoint()).x()
        && translateAgain->mapTo(translateToolbar,QPoint()).x() < again->mapTo(translateToolbar,QPoint()).x()
        && again->mapTo(translateToolbar,QPoint()).x()+again->width() == translateToolbar->width();
    bool switched = false, reran = false, retargeted = false;
    if (lists) {
        provider->setCurrentIndex(provider->findData(int(TranslateGateway::Provider::Transmart)));
        settle();
        switched = !pane->toPlainText().isEmpty()
            && provider->currentData().toInt() == int(TranslateGateway::Provider::Transmart);
        // Transmart lacks Dutch: the target list follows the engine matrix.
        retargeted = target->findData(QStringLiteral("nl")) < 0;
        // Refresh recognizes again and translates with the engine still chosen.
        again->click();
        settle();
        const QString enginePrefix = dialog.m_rows ? QStringLiteral("引擎 (") : QStringLiteral("翻译引擎 (");
        reran = !pane->toPlainText().isEmpty()
            && !LensSnipSettings().contains(QStringLiteral("Translate/provider"))
            && dialog.m_provider->displayText() == enginePrefix + QStringLiteral("腾讯交互翻译)");
        provider->setCurrentIndex(0);   // Back to Auto: the picker names the engine that answered.
        settle();
        const QString shown = dialog.m_provider->displayText();
        reran = reran && shown.startsWith(enginePrefix) && shown != enginePrefix + QStringLiteral("自动选择)");
    }
    if (!(lists && switched && retargeted && reran)) {
        restore();
        std::fprintf(stderr, "FAIL: translate pickers lists %d engine switch %d matrix %d retry %d (%s)\n",
                     lists, switched, retargeted, reran, status ? qPrintable(status->text()) : "");
        return 1;
    }
    std::puts("PASS: engine/source/target pickers, engine switch re-translates, retry reruns");

    // Part 4: the recognized text sits in an editable source pane; auto-detect names the
    // language it found; typing a correction re-translates after the pause.
    const QString preview = qEnvironmentVariable("LENSSNIP_TRANSLATE_PREVIEW");
    if (!preview.isEmpty()) dialog.grab().save(preview + QStringLiteral("-result.png"));
    auto *sourceText = dialog.findChild<QPlainTextEdit *>(QStringLiteral("translateSourceText"));
    const bool recognized = sourceText && sourceText->toPlainText().contains(QStringLiteral("fox"), Qt::CaseInsensitive)
        && !sourceText->isReadOnly() && !sourceText->document()->isModified();
    auto *sourceFooter=dialog.findChild<QWidget *>(QStringLiteral("translateSourceBar"));
    auto *resultFooter=dialog.findChild<QWidget *>(QStringLiteral("translateResultBar"));
    auto *sourceCopy=dialog.findChild<QToolButton *>(QStringLiteral("translateSourceCopy"));
    auto *resultCopy=dialog.findChild<QToolButton *>(QStringLiteral("ocrTextCopy"));
    auto *sourceCount=dialog.findChild<QLabel *>(QStringLiteral("translateSourceCount"));
    auto *resultCount=dialog.findChild<QLabel *>(QStringLiteral("ocrTextCount"));
    auto *imageCount=dialog.findChild<QLabel *>(QStringLiteral("ocrImageCount"));
    const bool blocks = sourceFooter && resultFooter && sourceText && sourceCopy && resultCopy
        && sourceCount && resultCount && imageCount
        && dialog.findChildren<QLabel *>(QStringLiteral("cardSectionTitle")).isEmpty()
        && dialog.findChild<QWidget *>(QStringLiteral("translateToolbar"))->isAncestorOf(source)
        && sourceFooter->mapTo(&dialog,QPoint()).y()>=sourceText->mapTo(&dialog,QPoint()).y()+sourceText->height()
        && resultFooter->mapTo(&dialog,QPoint()).y()>=pane->mapTo(&dialog,QPoint()).y()+pane->height()
        && sourceText->lineWrapMode()==QPlainTextEdit::WidgetWidth
        && pane->lineWrapMode()==QPlainTextEdit::WidgetWidth
        && sourceCount->text()==QString::number(sourceText->toPlainText().toUcs4().size())
        && resultCount->text()==QString::number(pane->toPlainText().toUcs4().size())
        && imageCount->text()==sourceCount->text()
        && !sourceCount->text().contains(QStringLiteral("字符"));
    if(!blocks){restore();std::fprintf(stderr,"FAIL: translation must use per-module footers and preserve both reading panes\n");return 1;}
    auto *sourceZoomLevel=dialog.findChild<QLabel *>(QStringLiteral("translateSourceZoomLevel"));
    auto *resultZoomLevel=dialog.findChild<QLabel *>(QStringLiteral("ocrTextZoomLevel"));
    const qreal sourceFontBefore=sourceText->font().pointSizeF();
    const qreal resultFontBefore=pane->font().pointSizeF();
    const QPointF sourceWheelPos(sourceText->viewport()->rect().center());
    QWheelEvent sourceWheelUp(sourceWheelPos,sourceText->viewport()->mapToGlobal(sourceWheelPos.toPoint()),
                              QPoint(),QPoint(0,120),Qt::NoButton,Qt::ControlModifier,
                              Qt::NoScrollPhase,false);
    QApplication::sendEvent(sourceText->viewport(),&sourceWheelUp);
    const bool sourceZoomed=sourceText->font().pointSizeF()>sourceFontBefore
        && sourceZoomLevel->text()==QStringLiteral("110%")
        && qAbs(pane->font().pointSizeF()-resultFontBefore)<0.01
        && resultZoomLevel->text()==QStringLiteral("100%");
    QWheelEvent sourceWheelDown(sourceWheelPos,sourceText->viewport()->mapToGlobal(sourceWheelPos.toPoint()),
                                QPoint(),QPoint(0,-120),Qt::NoButton,Qt::ControlModifier,
                                Qt::NoScrollPhase,false);
    QApplication::sendEvent(sourceText->viewport(),&sourceWheelDown);
    const bool sourceBack=qAbs(sourceText->font().pointSizeF()-sourceFontBefore)<0.01
        && sourceZoomLevel->text()==QStringLiteral("100%");
    const bool normalFooters=sourceFooter->height()==28 && resultFooter->height()==28
        && sourceCopy->width()==26 && sourceCopy->height()==24
        && resultCopy->iconSize()==QSize(20,20);
    if(!sourceZoomed || !sourceBack || !normalFooters){
        restore();
        std::fprintf(stderr,"FAIL: Ctrl+wheel or compact per-module footers are not correct\n");
        return 1;
    }
    // The capture in the translate card carries the recognized characters as selectable text.
    if (!dialog.m_imageView || dialog.m_imageView->glyphCount() == 0) {
        restore();
        std::fprintf(stderr, "FAIL: translate card capture has no selectable text\n");
        return 1;
    }
    const QString recognizedAll = dialog.m_ocrLineTexts.join(QLatin1Char('\n'));
    dialog.m_imageView->selectRange(0, dialog.m_imageView->glyphCount() - 1);
    QString mirroredSource = sourceText->textCursor().selectedText();
    mirroredSource.replace(QChar(0x2029), QLatin1Char('\n'));
    if (!sourceText->toPlainText().contains(dialog.m_imageView->selectedText().section(QLatin1Char('\n'), 0, 0))
        || mirroredSource != dialog.m_imageView->selectedText()
        || mirroredSource != recognizedAll) {
        restore();
        std::fprintf(stderr, "FAIL: capture selection does not mirror the exact recognized text (%s)\n",
                     qPrintable(dialog.m_imageView->selectedText()));
        return 1;
    }
    dialog.m_imageView->selectRange(0, 2);
    QString mirroredPart = sourceText->textCursor().selectedText();
    mirroredPart.replace(QChar(0x2029), QLatin1Char('\n'));
    const QString imagePart = dialog.m_imageView->selectedText();
    if (imagePart.size() != 3 || mirroredPart != imagePart || imagePart == recognizedAll) {
        restore();
        std::fprintf(stderr, "FAIL: capture selection does not map character by character (%s)\n",
                     qPrintable(imagePart));
        return 1;
    }
    dialog.m_imageView->selectRange(-1, -1);
    if (sourceText->textCursor().hasSelection()) {
        restore();
        std::fprintf(stderr, "FAIL: clearing the capture selection keeps the source selection\n");
        return 1;
    }
    // Selecting recognized text in the source pane marks the matching glyphs on the image.
    const int foxAt = sourceText->toPlainText().indexOf(QStringLiteral("fox"), 0, Qt::CaseInsensitive);
    QTextCursor sourceCursor = sourceText->textCursor();
    sourceCursor.setPosition(qMax(0,foxAt));
    sourceCursor.setPosition(qMax(0,foxAt) + 3, QTextCursor::KeepAnchor);
    sourceText->setTextCursor(sourceCursor);
    const bool sourceHighlights = foxAt >= 0 && dialog.m_imageView->highlighted().size() == 3;
    auto *title = dialog.findChild<QLabel *>(QStringLiteral("ocrTitle"));
    const qreal titlePt = title ? title->font().pointSizeF() : 0;
    if (titlePt < 12.0 || sourceText->font().pointSizeF() > pane->font().pointSizeF() + 0.01
        || pane->font().weight() < QFont::Medium) {
        restore();
        std::fprintf(stderr, "FAIL: translate type hierarchy title=%.2f source=%.2f result=%.2f weight=%d\n",
                     titlePt, sourceText->font().pointSizeF(), pane->font().pointSizeF(), pane->font().weight());
        return 1;
    }
    const bool sourceSelected = sourceText->textCursor().hasSelection();
    if (title) {
        const QPoint at = title->rect().center();
        QMouseEvent outside(QEvent::MouseButtonPress,QPointF(at),QPointF(title->mapToGlobal(at)),
                            Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(title,&outside);
    }
    const bool sourceCleared = !sourceText->textCursor().hasSelection()
        && dialog.m_imageView->highlighted().isEmpty();
    if (!sourceHighlights || !title || !sourceSelected || !sourceCleared) {
        restore();
        std::fprintf(stderr, "FAIL: translate source selection did not mirror onto the capture or clear on click\n");
        return 1;
    }
    // The translation pane also drops its native selection when the click lands elsewhere.
    QTextCursor resultCursor = pane->textCursor();
    resultCursor.movePosition(QTextCursor::Start);
    resultCursor.movePosition(QTextCursor::EndOfBlock,QTextCursor::KeepAnchor);
    pane->setTextCursor(resultCursor);
    const bool resultSelected = pane->textCursor().hasSelection();
    if (title) {
        const QPoint at = title->rect().center();
        QMouseEvent outside(QEvent::MouseButtonPress,QPointF(at),QPointF(title->mapToGlobal(at)),
                            Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(title,&outside);
    }
    if (!resultSelected || pane->textCursor().hasSelection()) {
        restore();
        std::fprintf(stderr, "FAIL: translate result selection did not clear on click elsewhere\n");
        return 1;
    }
    const QString languagePrefix = dialog.m_rows ? QStringLiteral("语言 (") : QStringLiteral("语言检测 (");
    const bool named = dialog.m_source->displayText() == languagePrefix + QStringLiteral("英语)");
    bool edited = false, hazed = false, quiet = false;
    auto *mask = dialog.findChild<QWidget *>(QStringLiteral("retranslateMask"));
    auto *maskButton = dialog.findChild<QPushButton *>(QStringLiteral("retranslateButton"));
    if (recognized && mask && maskButton) {
        const QString before = pane->toPlainText();
        const bool clearAtFirst = !mask->isVisible();
        sourceText->setFocus();
        sourceText->selectAll();
        // Same path as typing; switching to Japanese must re-detect the language on the next run.
        sourceText->insertPlainText(QStringLiteral("おはようございます。また明日会いましょう。"));
        // Typing alone never calls the engine: the old translation stays under the haze.
        QElapsedTimer pause;
        pause.start();
        while (pause.elapsed() < 1500) QApplication::processEvents(QEventLoop::AllEvents, 100);
        quiet = pane->toPlainText() == before && again->isEnabled();
        hazed = clearAtFirst && mask->isVisible() && mask->geometry() == pane->rect();
        if (!preview.isEmpty()) dialog.grab().save(preview + QStringLiteral("-hazed.png"));
        maskButton->click();
        QElapsedTimer wait;
        wait.start();
        while ((pane->toPlainText() == before || pane->toPlainText().isEmpty() || !again->isEnabled())
               && wait.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 100);
        edited = !pane->toPlainText().isEmpty() && pane->toPlainText() != before && sourceText->hasFocus()
            && !mask->isVisible() && dialog.m_source->displayText() == languagePrefix + QStringLiteral("日语)");
    }
    if (!preview.isEmpty()) dialog.grab().save(preview + QStringLiteral("-edited.png"));
    restore();
    if (!(recognized && named && quiet && hazed && edited)) {
        std::fprintf(stderr, "FAIL: source pane recognized %d detected-name %d typing quiet %d haze %d button re-translates %d (%s | %s)\n",
                     recognized, named, quiet, hazed, edited, qPrintable(dialog.m_source->displayText()), qPrintable(status->text()));
        return 1;
    }
    std::puts("PASS: editable source pane, detected language named, edits haze the translation until 重新翻译");

    // Part 5: back stays in the header, no layout/search/format buttons; width picks three
    // columns / stacked reading panes / texts only; display modes; find scoped to the pane
    // it runs in; failures read as plain advice, never the raw engine error.
    auto *view = dialog.findChild<QToolButton *>(QStringLiteral("translateView"));
    auto *translateBack = dialog.findChild<QToolButton *>(QStringLiteral("ocrBack"));
    auto *translatePin = dialog.findChild<QToolButton *>(QStringLiteral("ocrPin"));
    auto *imagePanel = dialog.findChild<QWidget *>(QStringLiteral("ocrImagePanel"));
    auto *sourceBlock = dialog.findChild<QWidget *>(QStringLiteral("translateSourceBlock"));
    auto *resultBlock = dialog.findChild<QWidget *>(QStringLiteral("translateResultBlock"));
    const bool trimmed = translateBack && translatePin
        && translateBack->parentWidget()==translatePin->parentWidget()
        && translateBack->mapTo(&dialog,QPoint()).x()<translatePin->mapTo(&dialog,QPoint()).x()
        && !dialog.findChild<QToolButton *>(QStringLiteral("ocrLayout"))
        && !dialog.findChild<QSplitter *>(QStringLiteral("translateSplit"))
        && !dialog.findChild<QToolButton *>(QStringLiteral("ocrSearch"))
        && !dialog.findChild<QToolButton *>(QStringLiteral("ocrFormat"))->isVisible();
    QApplication::processEvents();
    const QSize dialogExpected = OcrDialog::preferredSize(dialog.screen()->availableGeometry(),true);
    const bool wide = dialogExpected.width() >= 1250;
    const bool imageAllowed = dialogExpected.width() >= 900;
    const bool tallReadingPanes = dialog.size() == dialogExpected
        && dialog.geometry()==OcrDialog::screenCentered(dialog.screen()->geometry(),dialog.size())
        && dialog.m_split->count() == 2 && dialog.m_split->orientation() == Qt::Horizontal
        && dialog.m_split->widget(0) == imagePanel
        && dialog.m_split->widget(1) == dialog.m_textSplit
        && dialog.m_textSplit->count() == 2
        && dialog.m_textSplit->widget(0) == sourceBlock
        && dialog.m_textSplit->widget(1) == resultBlock
        && dialog.m_textSplit->orientation() == (wide ? Qt::Horizontal : Qt::Vertical)
        && dialog.m_textSplit->sizes().value(0) == dialog.m_textSplit->sizes().value(1)
        && dialog.m_split->sizes().value(1) > dialog.m_split->sizes().value(0);
    bool modes = view && view->menu() && view->menu()->actions().size() == 3
        && dialog.m_split->count() == 2 && dialog.m_split->orientation() == Qt::Horizontal
        && dialog.m_textSplit->orientation() == (wide ? Qt::Horizontal : Qt::Vertical)
        && imagePanel->isVisible() == imageAllowed
        && sourceText->isVisible() && pane->isVisible();
    if (modes) {
        const QList<QAction *> picks = view->menu()->actions();
        picks[1]->trigger();
        modes = !imagePanel->isVisible() && sourceText->isVisible() && pane->isVisible()
            && (wide ? sourceText->mapTo(&dialog,QPoint()).x() < pane->mapTo(&dialog,QPoint()).x()
                     : sourceText->mapTo(&dialog,QPoint()).y() < pane->mapTo(&dialog,QPoint()).y());
        picks[2]->trigger();
        modes = modes && !imagePanel->isVisible() && !sourceText->isVisible() && pane->isVisible()
            && LensSnipSettings().value(QStringLiteral("Translate/view")).toInt() == 2;
        picks[0]->trigger();
        modes = modes && imagePanel->isVisible() == imageAllowed && sourceText->isVisible()
            && dialog.m_textSplit->orientation() == (wide ? Qt::Horizontal : Qt::Vertical);
    }
    // Capture shape no longer picks the layout: every wide card opens the same three columns.
    QImage tall(120,360,QImage::Format_RGB32);
    QImage square(240,240,QImage::Format_RGB32);
    tall.fill(Qt::white);
    square.fill(Qt::white);
    OcrDialog tallCard(tall,{},true);
    OcrDialog squareCard(square,{},true);
    tallCard.show(); squareCard.show();
    QApplication::processEvents();
    tallCard.showLoading(false); squareCard.showLoading(false);
    QApplication::processEvents();
    tallCard.applyLayout(true); squareCard.applyLayout(true);
    QApplication::processEvents();
    const QSize tallExpected = OcrDialog::preferredSize(tallCard.screen()->availableGeometry(),true);
    const bool tallWide = tallExpected.width() >= 1250;
    auto *tallResult = tallCard.findChild<QWidget *>(QStringLiteral("translateResultBlock"));
    const bool columns = tallCard.size() == tallExpected && squareCard.size() == tallExpected
        && tallCard.m_split->count() == 2 && squareCard.m_split->count() == 2
        && tallCard.m_split->orientation() == Qt::Horizontal
        && squareCard.m_split->orientation() == Qt::Horizontal
        && tallCard.m_split->widget(0) == tallCard.m_imagePanel
        && tallCard.m_split->widget(1) == tallCard.m_textSplit
        && tallCard.m_textSplit->widget(0) == tallCard.m_sourceBox
        && tallCard.m_textSplit->widget(1) == tallResult
        && tallCard.m_textSplit->orientation() == (tallWide ? Qt::Horizontal : Qt::Vertical)
        && tallCard.m_textSplit->sizes().value(0) == tallCard.m_textSplit->sizes().value(1)
        && tallCard.m_split->sizes().value(1) > tallCard.m_split->sizes().value(0);
    if (!preview.isEmpty())
        tallCard.grab().save(preview + QStringLiteral("-three-columns.png"));
    const int columnHeight = tallCard.height(), columnWidth = tallCard.width();
    const QList<int> outerBefore = tallCard.m_split->sizes();
    const QList<int> innerBefore = tallCard.m_textSplit->sizes();
    QString columnText;
    for (int i=0;i<80;++i) columnText += QStringLiteral("第 %1 行：这是一段用于测量原文和译文显示区宽度的长文本。\n").arg(i);
    tallCard.m_sourcePane->setPlainText(columnText);
    tallCard.m_text->setPlainText(columnText);
    tallCard.m_translatedSource = columnText;
    tallCard.updateMask();
    tallCard.applyLayout(true);
    QApplication::processEvents();
    tallCard.m_status->clear();
    const bool fixedText = tallCard.size()==QSize(columnWidth,columnHeight)
        && tallCard.m_split->sizes()==outerBefore && tallCard.m_textSplit->sizes()==innerBefore
        && tallCard.m_text->verticalScrollBar()->isVisible();
    if (!preview.isEmpty())
        tallCard.grab().save(preview + QStringLiteral("-three-columns-long.png"));
    tallCard.resize(1100,columnHeight);
    QApplication::processEvents();
    const bool medium = tallCard.width()==1100 && tallCard.m_imagePanel->isVisible()
        && tallCard.m_textSplit->orientation()==Qt::Vertical
        && tallCard.m_sourceBox->mapTo(&tallCard,QPoint()).y() < tallCard.m_text->mapTo(&tallCard,QPoint()).y()
        && tallCard.m_sourceBox->height() > 200;
    if (!preview.isEmpty())
        tallCard.grab().save(preview + QStringLiteral("-medium.png"));
    tallCard.resize(680,columnHeight);
    QApplication::processEvents();
    const bool narrow = tallCard.width()==680 && !tallCard.m_imagePanel->isVisible()
        && tallCard.m_textSplit->orientation()==Qt::Vertical
        && tallCard.m_sourceBox->isVisible() && tallCard.m_text->isVisible()
        && tallCard.m_sourceBox->width() > 600;
    const bool responsiveTranslation = medium && narrow;
    if (!preview.isEmpty())
        tallCard.grab().save(preview + QStringLiteral("-narrow.png"));
    tallCard.close(); squareCard.close();
    dialog.showFind(false, sourceText);
    dialog.m_findEdit->setText(QStringLiteral("明日"));
    const bool scoped = dialog.m_find->parentWidget()->isAncestorOf(sourceText)
        && !dialog.m_find->parentWidget()->isAncestorOf(pane)
        && sourceText->extraSelections().size() == 1 && pane->extraSelections().isEmpty();
    // Opening replace from the strip (its toggle, or Ctrl+H typed inside it) keeps it in the original pane.
    dialog.m_replaceToggle->click();
    const bool stays = dialog.m_find->parentWidget()->isAncestorOf(sourceText) && dialog.m_replaceRow->isVisible()
        && dialog.m_findTarget == sourceText;
    dialog.showFind(false, pane);
    const bool moved = dialog.m_find->parentWidget()->isAncestorOf(pane) && sourceText->extraSelections().isEmpty();
    dialog.hideFind();
    auto *retranslate = dialog.findChild<QToolButton *>(QStringLiteral("translateAgain"));
    // A dead proxy fails every engine at once: a chosen engine and Auto each get their own advice.
    const QNetworkProxy savedProxy = QNetworkProxy::applicationProxy();
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 1));
    const auto failWith = [&](TranslateGateway::Provider engine) {
        provider->setCurrentIndex(provider->findData(int(engine)));
        if (provider->currentData().toInt() == int(engine)) retranslate->click();   // Already selected: re-translate.
        QElapsedTimer wait;
        wait.start();
        while (!again->isEnabled() && wait.elapsed() < 120000) QApplication::processEvents(QEventLoop::AllEvents, 100);
        return status->text();
    };
    const QString chosen = failWith(TranslateGateway::Provider::Edge);
    const QString automatic = failWith(TranslateGateway::Provider::Auto);
    QNetworkProxy::setApplicationProxy(savedProxy);
    const bool friendly = chosen == QStringLiteral("Edge暂时无法翻译，请稍后重试或切换为其他翻译引擎")
        && automatic == QStringLiteral("翻译服务暂时不可用，请检查网络后重试") && !pane->toPlainText().isEmpty();
    if (!preview.isEmpty()) {
        OcrEngine::instance().busy.store(true);
        OcrDialog longCard(image,{},true);
        longCard.show();
        QApplication::processEvents();
        QString longText;
        for (int i=0;i<80;++i) longText += QStringLiteral("第 %1 行：这是一段用于测量原文和译文结果框高度的长文本。\n").arg(i);
        longCard.m_sourcePane->setPlainText(longText);
        longCard.m_text->setPlainText(longText);
        longCard.m_translatedSource = longText;
        longCard.updateMask();
        longCard.m_status->clear();
        longCard.applyLayout(true);
        QApplication::processEvents();
        const QSize longExpected = OcrDialog::preferredSize(longCard.screen()->availableGeometry(),true);
        if (longCard.size()!=longExpected || longCard.m_sourcePane->height()<=200) {
            OcrEngine::instance().busy.store(false);
            restore();
            std::fprintf(stderr, "FAIL: long translation changed the fixed card (%dx%d, expected %dx%d, source=%d)\n",
                         longCard.width(),longCard.height(),longExpected.width(),longExpected.height(),
                         longCard.m_sourcePane->height());
            return 1;
        }
        longCard.grab().save(preview + QStringLiteral("-long.png"));
        longCard.reject();
        OcrEngine::instance().busy.store(false);
        QThreadPool::globalInstance()->waitForDone();
        OcrDialog loadingCard(image,{},true);
        loadingCard.show();
        loadingCard.grab().save(preview + QStringLiteral("-loading.png"));
        loadingCard.reject();
    }
    restore();
    LensSnipSettings().remove(QStringLiteral("Translate/view"));
    if (!(trimmed && retranslate && modes && tallReadingPanes && columns && scoped && stays && moved && friendly
          && fixedText && responsiveTranslation)) {
        std::fprintf(stderr, "FAIL: trimmed %d retranslate %d modes %d tall panes %d (card=%dx%d split=%dx%d sizes=%d,%d) columns %d fixed %d responsive %d (tall=%dx%d outer=%d,%d inner=%d,%d) find scoped %d replace stays %d moved %d friendly %d (%s | %s)\n",
                     trimmed, retranslate != nullptr, modes, tallReadingPanes,
                     dialog.width(),dialog.height(),dialog.m_split->width(),dialog.m_split->height(),
                     dialog.m_split->sizes().value(0),dialog.m_split->sizes().value(1),
                     columns,fixedText,responsiveTranslation,tallCard.width(),tallCard.height(),
                     tallCard.m_split->sizes().value(0),tallCard.m_split->sizes().value(1),
                     tallCard.m_textSplit->sizes().value(0),tallCard.m_textSplit->sizes().value(1),
                     scoped, stays, moved, friendly, qPrintable(chosen), qPrintable(automatic));
        return 1;
    }
    std::puts("PASS: width-driven columns, fixed height with scrolling, display modes, per-pane find/replace, friendly failure advice");
    return 0;
}
