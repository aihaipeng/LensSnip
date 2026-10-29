#include "shared/dialog_chrome.h"
#include "onnxruntime_c_api.h"
#include <QClipboard>
// lenssnip — screenshot capture, pinned images, scrolling capture, and offline OCR (Qt 6 / MinGW).
// shortcut: Screen captures run serially; use concurrent streamers if synchronized frames are required.

#include "app/controller.h"
#include "shared/i18n.h"
#include "longshot/long_capture.h"
#include "text/engines/ocr_engine.h"
#include "capture/capture_overlay.h"
#include "text/cards/ocr_dialog.h"
#include "pin/pin_window.h"
#include "history/history_store.h"
#include "shared/widgets.h"
#include "tests/test_support.h"
#include <QGuiApplication>
#include <QVector>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QApplication>
#include <QFont>
#include <QtMath>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QColor>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QTranslator>
#include <QSettings>
#include <QDir>
#include <QStyleHints>
#include <qt_windows.h>
#include <cstdio>

int runTranslateGatewayTests();   // tests/translate_gateway_tests.cpp
int runExportTests(const QString &preview);   // tests/export_tests.cpp

int main(int argc, char **argv)
{
    QSettings::setDefaultFormat(QSettings::IniFormat);   // User settings live in %APPDATA%/LensSnip/LensSnip.ini.
    QApplication app(argc, argv);
    // Test modes return without entering the event loop. Drain OCR/erase work
    // here so their workers never outlive the static OCR engine.
    struct PoolDrain {
        ~PoolDrain() { QThreadPool::globalInstance()->waitForDone(); }
    } poolDrain;
    Q_UNUSED(poolDrain);
    app.setApplicationName(QStringLiteral("LensSnip"));
    QFont baseFont = QApplication::font();
    baseFont.setFamilies(QStringList(QStringLiteral("Inter")) + baseFont.families());   // Latin prefers Inter when installed; otherwise falls back to the system stack.
    baseFont.setPointSizeF(10.5);                              // 96 dpi ≈ 14 px standard UI body.
    app.setFont(baseFont);
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);   // Standard dialogs match the dark application chrome.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] { HistoryStore::waitForPending(); });
    QTranslator applicationTranslator, qtTranslator;
    const bool i18nTest = app.arguments().contains(QStringLiteral("--test-i18n"));
    if (I18n::isTestMode(app.arguments())) {
        // Regression runs must never overwrite the user's real preferences.
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QDir::tempPath() + QStringLiteral("/LensSnip-tests-%1")
                               .arg(QCoreApplication::applicationPid()));
    }
    I18n::install(&applicationTranslator, &qtTranslator, I18n::effectiveLocale(app.arguments()),
                  !I18n::isTestMode(app.arguments()) || i18nTest);
    // One palette for every unstyled control: menus, tooltips, message boxes and the color dialog.
    QPalette dark;
    dark.setColor(QPalette::Window, QColor(QStringLiteral("#18191d")));
    dark.setColor(QPalette::WindowText, QColor(QStringLiteral("#f4f5f8")));
    dark.setColor(QPalette::Base, QColor(QStringLiteral("#0c0d10")));
    dark.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#111216")));
    dark.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#18191d")));
    dark.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#f4f5f8")));
    dark.setColor(QPalette::Text, QColor(QStringLiteral("#f4f5f8")));
    dark.setColor(QPalette::Button, QColor(QStringLiteral("#18191d")));
    dark.setColor(QPalette::ButtonText, QColor(QStringLiteral("#f4f5f8")));
    dark.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    dark.setColor(QPalette::Link, QColor(QStringLiteral("#5e6ad2")));
    dark.setColor(QPalette::Highlight, QColor(QStringLiteral("#32345b")));
    dark.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#f4f5f8")));
    dark.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#9c9da1")));
    dark.setColor(QPalette::Mid, QColor(QStringLiteral("#2b2d33")));
    dark.setColor(QPalette::Dark, QColor(QStringLiteral("#0c0d10")));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        dark.setColor(QPalette::Disabled, role, QColor(QStringLiteral("#73757f")));
    app.setPalette(dark);
    installDialogChrome(&app);
    app.setStyleSheet(QStringLiteral(
        "QToolTip{background:#18191d;color:#f4f5f8;border:1px solid #2b2d33;"
            "border-radius:6px;padding:5px 8px;font-size:12px;}"
        "QMessageBox,QInputDialog,QColorDialog{background:#18191d;color:#f4f5f8;}"
        "QPushButton{background:#18191d;color:#f4f5f8;border:1px solid #2b2d33;"
            "border-radius:6px;min-height:28px;padding:0 12px;font-size:14px;text-align:center;}"
        "QPushButton:hover{background:#222326;}"
        "QPushButton:pressed{background:#2b2d33;}"
        "QPushButton:default{background:#5e6ad2;border-color:#5e6ad2;color:#ffffff;}"
        "QPushButton:default:hover{background:#5661c4;border-color:#5661c4;}"
        "QPushButton:default:pressed{background:#4f59b5;border-color:#4f59b5;}"
        "QPushButton:disabled{color:#73757f;background:#18191d;border-color:#2b2d33;}"
        "QPushButton:focus{border:2px solid #828fff;padding:0 11px;}"
        "QPushButton:default:focus{border-color:#f4f5f8;}"
        "QLineEdit,QSpinBox,QDoubleSpinBox,QPlainTextEdit{background:#111216;color:#f4f5f8;"
            "border:1px solid #777980;border-radius:6px;padding:3px 8px;"
            "selection-background-color:#32345b;selection-color:#f4f5f8;}"
        "QLineEdit:focus,QSpinBox:focus,QDoubleSpinBox:focus,QPlainTextEdit:focus{border-color:#5e6ad2;}"));
    if (app.arguments().contains(QStringLiteral("--test-i18n"))) {
        const QString locale = I18n::effectiveLocale(app.arguments());
        const bool english = locale.startsWith(QStringLiteral("en"));
        const QString capture = QCoreApplication::translate("Controller", "截图 (F1)");
        const QString loading = QCoreApplication::translate("OcrDialog", "正在识别…");
        const QString arrow = QCoreApplication::translate("CaptureOverlay", "箭头 (A)");
        const QString pinFail = QCoreApplication::translate("PinWindow", "识别失败，右键重试");
        const QString pinPending = QCoreApplication::translate("PinWindow", "文字识别中…");
        const QString historyEmpty = QCoreApplication::translate(
                "HistoryDialog", "暂无历史截图\n新截图会自动记录在这里。");
        const QString preview = QCoreApplication::translate("ToolLayoutDialog", "截图区域");
        const QString hiddenHint = QCoreApplication::translate(
                "ToolLayoutDialog", "拖到这里收起工具；下拉入口始终保留。");
        const QString save = QCoreApplication::translate("ToolLayoutDialog", "保存");
        const QString dropdown = QCoreApplication::translate("CaptureOverlay", "下拉");
        const QString adjust = QCoreApplication::translate("CaptureOverlay", "调整");
        const QString exportTitle = QCoreApplication::translate("ExportDialog", "保存");
        const QString exportFolder = QCoreApplication::translate("ExportDialog", "选择文件夹");
        const QString insertText = QCoreApplication::translate("CaptureOverlay", "插入文本");
        const QString textAbove = QCoreApplication::translate("CaptureOverlay", "上方");
        const QString textBelow = QCoreApplication::translate("CaptureOverlay", "下方");
        const QString textColor = QCoreApplication::translate("CaptureOverlay", "文字颜色");
        const bool hasEnglish = I18n::availableLocales().contains(QStringLiteral("en_US"));
        const bool ok = hasEnglish
                && (english ? capture == QStringLiteral("Screenshot (F1)")
                                  && loading == QStringLiteral("Recognizing…")
                                  && arrow == QStringLiteral("Arrow (A)")
                                  && pinFail == QStringLiteral("Recognition failed. Right-click to retry.")
                                  && pinPending == QStringLiteral("Recognizing text…")
                                  && historyEmpty == QStringLiteral("No screenshots yet.\nNew screenshots will be recorded here automatically.")
                                   && preview == QStringLiteral("Capture area")
                                   && hiddenHint == QStringLiteral("Drop tools here to hide them. The dropdown entry stays visible.")
                                  && save == QStringLiteral("Save")
                                  && dropdown == QStringLiteral("Dropdown")
                                  && adjust == QStringLiteral("Adjust")
                                  && exportTitle == QStringLiteral("Save")
                                   && exportFolder == QStringLiteral("Choose folder")
                                   && insertText == QStringLiteral("Insert text")
                                    && textAbove == QStringLiteral("Above") && textBelow == QStringLiteral("Below")
                                    && textColor == QStringLiteral("Text color")
                            : capture == QStringLiteral("截图 (F1)")
                                  && loading == QStringLiteral("正在识别…")
                                  && arrow == QStringLiteral("箭头 (A)")
                                  && pinFail == QStringLiteral("识别失败，右键重试")
                                  && pinPending == QStringLiteral("文字识别中…")
                                  && historyEmpty == QStringLiteral("暂无历史截图\n新截图会自动记录在这里。")
                                   && preview == QStringLiteral("截图区域")
                                   && hiddenHint == QStringLiteral("拖到这里收起工具；下拉入口始终保留。")
                                  && save == QStringLiteral("保存")
                                  && dropdown == QStringLiteral("下拉")
                                  && adjust == QStringLiteral("调整")
                                  && exportTitle == QStringLiteral("保存")
                                   && exportFolder == QStringLiteral("选择文件夹")
                                   && insertText == QStringLiteral("插入文本")
                                    && textAbove == QStringLiteral("上方") && textBelow == QStringLiteral("下方")
                                    && textColor == QStringLiteral("文字颜色"));
        std::printf("%s locale=%s locales=%s capture=[%s] loading=[%s] arrow=[%s] pinFail=[%s] pinPending=[%s] historyEmpty=[%s] preview=[%s] hidden=[%s] save=[%s] dropdown=[%s] adjust=[%s]\n",
                    ok ? "PASS" : "FAIL", qPrintable(locale),
                    qPrintable(I18n::availableLocales().join(QLatin1Char(','))),
                    qPrintable(capture), qPrintable(loading), qPrintable(arrow),
                    qPrintable(pinFail), qPrintable(pinPending), qPrintable(historyEmpty),
                    qPrintable(preview), qPrintable(hiddenHint), qPrintable(save),
                    qPrintable(dropdown), qPrintable(adjust));
        return ok ? 0 : 1;
    }
    if (app.arguments().contains(QStringLiteral("--test-ui-theme"))) {
        const QString preview = app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-ui-theme")) + 1);
        const int capture = CaptureOverlay::testThemeUi(preview);
        const int pin = PinWindow::testThemeUi(preview);
        const int scrolling = LongCapture::testThemeUi(preview);
        const int history = HistoryDialog::testUi(preview);
        const int tray = Controller::testThemeUi(preview);
        return capture || pin || scrolling || history || tray ? 1 : 0;
    }
    if (app.arguments().contains(QStringLiteral("--test-history")))
        return HistoryStore::testHistory();
    if (app.arguments().contains(QStringLiteral("--test-history-ui")))
        return HistoryDialog::testUi(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-history-ui")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-ocr-lifetime")))
        return OcrDialog::testLifetime();
    if (app.arguments().contains(QStringLiteral("--test-translate")))
        return OcrDialog::testTranslate();
    if (app.arguments().contains(QStringLiteral("--test-translate-gateway")))
        return runTranslateGatewayTests();
    if (app.arguments().contains(QStringLiteral("--test-round-refresh"))) {
        const QString preview = app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-round-refresh"))+1);
        return CaptureOverlay::testRoundRefresh(preview);
    }
    if (app.arguments().contains(QStringLiteral("--test-barcode"))) {
        const QString preview = app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-barcode"))+1);
        const int card = OcrDialog::testBarcode(preview);
        return card ? card : CaptureOverlay::testBarcode(preview);
    }
    if (app.arguments().contains(QStringLiteral("--test-ocr-languages")))
        return OcrEngine::testLanguages(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-ocr-languages"))+1));
    if (app.arguments().contains(QStringLiteral("--test-ocr")))
        return CaptureOverlay::testOcr(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-ocr"))+1));
    if (app.arguments().contains(QStringLiteral("--test-ocr-loader"))) {
        auto &engine = OcrEngine::instance();
        const QString root = app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-ocr-loader"))+1,
                                                  QCoreApplication::applicationDirPath());
        const QString missing = OcrEngine::assetError(root);
        if (!missing.isEmpty()) { std::printf("UNAVAILABLE: %s\n", qPrintable(missing)); return 2; }
        const QString error = engine.load(root);
        if (!error.isEmpty()) { std::fprintf(stderr,"FAIL: %s\n",qPrintable(error)); return 1; }
        std::printf("PASS: ORT %s / API %d / recognition dictionary %d entries\n",qPrintable(engine.version()),ORT_API_VERSION,engine.dictionarySize());
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--test-mosaic")))
        return CaptureOverlay::testMosaic(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-mosaic")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-smart-erase")))
        return CaptureOverlay::testSmartErase(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-smart-erase")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-auto-mosaic")))
        return CaptureOverlay::testAutoMosaic(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-auto-mosaic")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-output"))) {
        auto *saved = new QMimeData;
        if (const QMimeData *data = QGuiApplication::clipboard()->mimeData())
            for (const QString &format : data->formats()) saved->setData(format, data->data(format));
        const int longResult = LongCapture::testOutput();
        const int captureResult = CaptureOverlay::testOutput();
        QGuiApplication::clipboard()->setMimeData(saved);
        return longResult || captureResult ? 1 : 0;
    }
    if (app.arguments().contains(QStringLiteral("--test-export")))
        return runExportTests(app.arguments().value(
            app.arguments().indexOf(QStringLiteral("--test-export")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-stitching")))
        return LongCapture::testStitching();
    if (app.arguments().contains(QStringLiteral("--test-live-capture")))
        return LongCapture::testLiveCapture(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-live-capture")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-pin-placement")))
        return PinWindow::testPlacement();
    if (app.arguments().contains(QStringLiteral("--test-pin-markup")))
        return PinWindow::testPlacement();           // Isolated process: markup sections crash live OCR otherwise.
    if (app.arguments().contains(QStringLiteral("--test-color-picker")))
        return CaptureOverlay::testColorPicker(app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-color-picker")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-selection")))
        return CaptureOverlay::testSelection();
    if (app.arguments().contains(QStringLiteral("--test-annotation-controls")))
        return CaptureOverlay::testSelectionBorders() ? 1 : 0;
    if (app.arguments().contains(QStringLiteral("--test-toolbar-placement")))
        return CaptureOverlay::testToolbarPlacement(app.arguments().value(
            app.arguments().indexOf(QStringLiteral("--test-toolbar-placement")) + 1));
    if (app.arguments().contains(QStringLiteral("--test-window-detection")))
        return CaptureOverlay::testWindowDetection();
    if (app.arguments().contains(QStringLiteral("--test-text-annotation")))
        return CaptureOverlay::testTextAnnotation();
    if (app.arguments().contains(QStringLiteral("--test-arrow-comment"))) {
        const QString preview = app.arguments().value(
            app.arguments().indexOf(QStringLiteral("--test-arrow-comment")) + 1);
        return CaptureOverlay::testArrowComment(preview);
    }
    if (app.arguments().contains(QStringLiteral("--test-watermark"))) {
        const QString preview = app.arguments().value(app.arguments().indexOf(QStringLiteral("--test-watermark")) + 1);
        return CaptureOverlay::testWatermark(preview);
    }
    if (app.arguments().contains(QStringLiteral("--test-color-wheel")))
        return ColorBoard::testRendering();
    const int probe = app.arguments().indexOf(QStringLiteral("--probe-uia"));
    if (probe > 0 && probe + 2 < app.arguments().size()) {
        // Diagnostic: run the production element engines at one native-pixel screen point.
        const QPoint point(app.arguments().at(probe + 1).toInt(), app.arguments().at(probe + 2).toInt());
        const HRESULT com = CoInitialize(nullptr);
        UiaAutomation *automation = nullptr;
        if (SUCCEEDED(com))
            CoCreateInstance(kUiaClassId, nullptr, CLSCTX_INPROC_SERVER, kUiaIid,
                             reinterpret_cast<void **>(&automation));
        QElapsedTimer t;
        t.start();
        const WindowTargets targets = detectWindowRects();
        const qint64 enumMs = t.elapsed();
        std::printf("detectWindowRects %lldms (%lld windows)\n", enumMs, (long long)targets.size());
        for (const DetectedWindow &win : targets) {
            if (!win.native.contains(point)) continue;
            QStringList slow;
            UiaCache uiaCache;
            QElapsedTimer total, t;
            total.start();
            t.start();
            const QRect uia = uiaRectAt(automation, win.hwnd, point, win.native, QRect(), &slow, win.program,
                                        &uiaCache);
            const qint64 uiaMs = t.restart();
            const QRect warm = uiaRectAt(automation, win.hwnd, point, win.native, QRect(), &slow, win.program,
                                         &uiaCache);
            const qint64 warmMs = t.restart();
            const QRect msaa = SUCCEEDED(com) ? accessibleAt(win.hwnd, point, win.native) : QRect();
            const qint64 msaaMs = t.restart();
            const QRect child = childAt(win, point);
            const qint64 childMs = t.elapsed();
            std::printf("window (%d,%d %dx%d) program=%s\nuia    (%d,%d %dx%d) %lldms\nwarm   (%d,%d %dx%d) %lldms\nmsaa   (%d,%d %dx%d) %lldms\nchild  (%d,%d %dx%d) %lldms\ntotal query %lldms\n",
                        win.native.x(), win.native.y(), win.native.width(), win.native.height(),
                        qPrintable(win.program),
                        uia.x(), uia.y(), uia.width(), uia.height(), uiaMs,
                        warm.x(), warm.y(), warm.width(), warm.height(), warmMs,
                        msaa.x(), msaa.y(), msaa.width(), msaa.height(), msaaMs,
                        child.x(), child.y(), child.width(), child.height(), childMs,
                        total.elapsed());
            break;
        }
        if (automation) automation->Release();
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--dump-windows"))) {
        // Diagnostic: print detected targets in global logical coordinates.
        const QVector<ScreenFrame> frames = freezeScreens();
        if (frames.isEmpty()) { std::puts("no frames"); return 1; }
        for (const DetectedWindow &w : detectWindowRects()) {
            const QRect top = logicalFromNative(frames, w.native);
            std::printf("top  (%d,%d %dx%d)", top.x(), top.y(), top.width(), top.height());
            for (const QRect &c : w.nativeChildren) {
                const QRect lc = logicalFromNative(frames, c);
                std::printf("  child (%d,%d %dx%d)", lc.x(), lc.y(), lc.width(), lc.height());
            }
            std::printf("\n");
        }
        return 0;
    }
    // A typo'd test or probe flag must fail loudly instead of falling through
    // to a resident tray process that exits 0.
    if (I18n::isTestMode(app.arguments())) {
        std::fprintf(stderr, "FAIL: unmatched test or probe argument\n");
        return 2;
    }
    app.setQuitOnLastWindowClosed(false);            // Keep the tray active after all pins are closed.

    // One resident instance owns the F1 hotkey and the history store; a second
    // launch would report its own registration as a conflict and race the DB.
    // CreateMutexW does not reset last error on a fresh create, so clear it first:
    // a stale ERROR_ALREADY_EXISTS would make the FIRST instance exit silently.
    ::SetLastError(0);
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"LensSnip.SingleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        return 0;
    }
    if (!instanceMutex)
        qWarning("single-instance mutex unavailable; running without the guard");

    Controller ctrl;
    HotkeyFilter hf;
    hf.onHotkey = [&ctrl] { ctrl.startCapture(true); };
    app.installNativeEventFilter(&hf);
    if (!RegisterHotKey(nullptr, 1, MOD_NOREPEAT, VK_F1))
        showAlert(nullptr, QCoreApplication::translate("Main", "无法注册截图热键"),
                  QCoreApplication::translate("Main",
                      "F1 已被其他程序占用。\n请释放该快捷键后重新启动 LensSnip。"));

    const int exitCode = app.exec();
    QThreadPool::globalInstance()->waitForDone();
    return exitCode;
}
