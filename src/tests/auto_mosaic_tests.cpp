#include "capture/capture_overlay.h"
#include "capture/auto_mosaic.h"
#include "tests/test_support.h"
#include "text/engines/ocr_engine.h"
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

// Covers the auto mosaic tool: identifier classification, QR detection, the
// standalone toolbar entry, the auto-only options strip, hover/click toggling,
// linked blocks and the manual drag fallback.
namespace {
void check(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

// A blank placeholder frame plus a neutered frame source: the injected canvas
// is deterministic and no live re-freeze can replace it.
std::unique_ptr<CaptureOverlay> testOverlay()
{
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    QImage placeholder(4, 4, QImage::Format_RGB32);
    placeholder.fill(QColor(20, 20, 20));
    return std::make_unique<CaptureOverlay>(
        QVector<ScreenFrame>{ScreenFrame{screen, QRect(screen.topLeft(), QSize(4, 4)), placeholder}});
}

QMouseEvent mouse(QEvent::Type type, const QPoint &pos, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    return QMouseEvent(type, QPointF(pos), button, buttons, Qt::NoModifier);
}

bool waitUntil(const std::function<bool()> &done, int timeout = 20000)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeout);
    while (!done() && timer.isActive()) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}
}

int CaptureOverlay::testAutoMosaic(const QString &previewPath)
{
    using AutoMosaic::Kind;
    check(AutoMosaic::testMerging(), "tiled candidates merge into whole lines");
    {   // Identifier classification: only actual privacy text is covered.
        check(AutoMosaic::classify(QStringLiteral("+86-13800138000")) == Kind::Phone,
              "CN mobile numbers classify as phone");
        check(AutoMosaic::classify(QStringLiteral("alice.brown927@qq.com")) == Kind::Email,
              "e-mail addresses classify as email");
        check(AutoMosaic::classify(QStringLiteral("https://pixpin.cn/docs/")) == Kind::Url,
              "links classify as URL");
        check(AutoMosaic::classify(QStringLiteral("ABC123")) == Kind::None,
              "ordinary repeated text is not a privacy identifier");
        check(AutoMosaic::classify(QString()) == Kind::None, "empty text classifies as none");
    }

    {   // QR fixture from the barcode regression: the symbol becomes a block.
        const QByteArray encoded =
            "iVBORw0KGgoAAAANSUhEUgAAAJQAAACUAQAAAABdRz15AAABO0lEQVR4nO1WQWrEQAyTh73LP9j/Pys/kF+g4sleuqVQ6jmuIcQEIiayJCeM96r14xHweYbfOIiIBJAVr3bN+KONyBKR3c65j4SvZ4G7PTLL4sWE/vXut3rct2QZF87gGSkweLczvDACzdyuAqiRPx59pk1gIV/eWwM82CRp07BN2RrigRYE0aTMOV7DNSwNYo4nwhQI48j3mt7HlEBpeL4FREEtO1K3ctbsfLoptKw9FQ31jNY0cWEbmOO8L0fwAlN4Yuq31WEAFa9qtH2toT/Epq2VR075g3sKHcu3Cg/oj0TLeOcCfUDP3jkAt4Onflv3fhPD1TuuI3/N8GijgkWiXXxkvz0FZqO2pNcQD+hVVPVaHziAl73iklUa+gMdV/d4O2E6HDT1bwQy3clw5YF9+V71+XfEn/n7Alf1yGlCqVzEAAAAAElFTkSuQmCC";
        const QImage qr = QImage::fromData(QByteArray::fromBase64(encoded), "PNG");
        check(!qr.isNull(), "embedded QR fixture decodes");
        AutoMosaic::Result result = AutoMosaic::detect(qr);
        bool found = false;
        for (const AutoMosaic::Detected &block : result.blocks)
            if (block.kind == Kind::Qr && block.key.contains(QStringLiteral("example.com"))) found = true;
        check(found, "QR payload becomes a syncable block");
    }

    {   // Non-text elements: colored avatar-like discs are candidates too.
        QImage canvas(400, 400, QImage::Format_RGB32);
        canvas.fill(Qt::white);
        {
            QPainter p(&canvas);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(120, 120, 120), 2));
            const QColor left(200, 90, 70);
            const QList<QColor> right = {QColor(60, 140, 200), QColor(80, 180, 100),
                                         QColor(180, 120, 60)};
            for (int i = 0; i < 3; ++i) {
                p.setBrush(left);
                p.drawEllipse(QRect(70, 60 + i * 100, 56, 56));
                p.setBrush(right[i]);
                p.drawEllipse(QRect(240, 60 + i * 100, 56, 56));
            }
        }
        const AutoMosaic::Result result = AutoMosaic::detect(canvas);
        QVector<AutoMosaic::Detected> images;
        for (const AutoMosaic::Detected &block : result.blocks)
            if (block.key.startsWith(QStringLiteral("image:"))) images.append(block);
        check(images.size() >= 6, "colored image elements become candidates");
        QStringList leftKeys;
        for (const AutoMosaic::Detected &block : images)
            if (block.rect.center().x() < 200) leftKeys << block.key;
        check(leftKeys.size() >= 3 && leftKeys.first() == leftKeys[1]
                  && leftKeys.first() == leftKeys[2],
              "identical image elements share one sync key");
    }

    {   // Real OCR path; the engine is optional in stripped test environments.
        const QString assets = QCoreApplication::applicationDirPath();
        const QString assetError = OcrEngine::assetError(assets);
        QImage image(1100, 260, QImage::Format_RGB32);
        image.fill(Qt::white);
        {
            QPainter p(&image);
            QFont font(QStringLiteral("Arial"));
            font.setPixelSize(32);
            p.setFont(font);
            p.setPen(Qt::black);
            p.drawText(60, 80, QStringLiteral("alice.brown927@qq.com"));
            p.drawText(60, 160, QStringLiteral("https://pixpin.cn/docs/"));
            p.drawText(60, 240, QStringLiteral("+86-13800138000"));
        }
        AutoMosaic::Result result = AutoMosaic::detect(image);
        if (!assetError.isEmpty()) {
            std::printf("SKIP: OCR assets unavailable (%s)\n", qPrintable(assetError));
        } else {
            int email = 0, url = 0, phone = 0;
            for (const AutoMosaic::Detected &block : result.blocks) {
                if (block.kind == Kind::Email) ++email;
                if (block.kind == Kind::Url) ++url;
                if (block.kind == Kind::Phone) ++phone;
            }
            if (!result.error.isEmpty()) std::printf("OCR note: %s\n", qPrintable(result.error));
            check(email > 0, "OCR finds the e-mail identifier");
            check(url > 0, "OCR finds the link");
            check(phone > 0, "OCR finds the phone identifier");

            // Wide captures are tiled; identifiers near both edges must survive.
            QImage wide(2600, 480, QImage::Format_RGB32);
            wide.fill(Qt::white);
            {
                QPainter p(&wide);
                QFont font(QStringLiteral("Arial"));
                font.setPixelSize(20);
                p.setFont(font);
                p.setPen(Qt::black);
                p.drawText(100, 120, QStringLiteral("alice.brown927@qq.com"));
                p.drawText(2330, 120, QStringLiteral("https://pixpin.cn/docs/"));
            }
            const AutoMosaic::Result tiled = AutoMosaic::detect(wide);
            int tiledEmail = 0, tiledUrl = 0;
            for (const AutoMosaic::Detected &block : tiled.blocks) {
                if (block.kind == Kind::Email) ++tiledEmail;
                if (block.kind == Kind::Url) ++tiledUrl;
            }
            check(tiledEmail > 0 && tiledUrl > 0,
                  "wide captures keep identifiers in both tiles");

            // A line straddling a horizontal tile seam must survive as one complete
            // block, not as the half detected in the tile above the seam.
            QImage seam(1600, 2200, QImage::Format_RGB32);
            seam.fill(Qt::white);
            {
                QPainter p(&seam);
                QFont font(QStringLiteral("Arial"));
                font.setPixelSize(32);
                p.setFont(font);
                p.setPen(Qt::black);
                p.drawText(100, 1415, QStringLiteral("alice.brown927@qq.com"));
            }
            const AutoMosaic::Result seamResult = AutoMosaic::detect(seam);
            int seamEmail = 0;
            QRect seamBlock;
            for (const AutoMosaic::Detected &block : seamResult.blocks)
                if (block.kind == Kind::Email) { ++seamEmail; seamBlock = block.rect; }
            check(seamEmail == 1, "a line across a horizontal tile seam stays one block");
            check(seamBlock.height() >= 30, "the seam block covers the whole line");

            // A link longer than one tile is split by two seams; merging must
            // reunite it before recognition so it still classifies as a URL.
            QImage longUrl(3200, 500, QImage::Format_RGB32);
            longUrl.fill(Qt::white);
            {
                QPainter p(&longUrl);
                QFont font(QStringLiteral("Arial"));
                font.setPixelSize(28);
                p.setFont(font);
                p.setPen(Qt::black);
                p.drawText(1300, 180, QStringLiteral(
                    "https://example.com/reports/2026/october/quarterly-summary/department/"
                    "finance/archive/download?id=1234567890"));
            }
            const AutoMosaic::Result longResult = AutoMosaic::detect(longUrl);
            int longUrlCount = 0;
            QRect longUrlBlock;
            for (const AutoMosaic::Detected &block : longResult.blocks)
                if (block.kind == Kind::Url) { ++longUrlCount; longUrlBlock = block.rect; }
            check(longUrlCount == 1, "a link across two tile seams stays one block");
            check(longUrlBlock.width() >= 1350, "the merged link covers the whole line");

            // The same link starting at the left edge crosses all three tiles.
            // Merging must follow the widening tile span, not stop after the
            // second tile and leave the tail as its own block.
            QImage leftUrl(3200, 500, QImage::Format_RGB32);
            leftUrl.fill(Qt::white);
            {
                QPainter p(&leftUrl);
                QFont font(QStringLiteral("Arial"));
                font.setPixelSize(56);
                p.setFont(font);
                p.setPen(Qt::black);
                p.drawText(0, 200, QStringLiteral(
                    "https://example.com/reports/2026/october/quarterly-summary/department/"
                    "finance/archive/download?id=1234567890"));
            }
            const AutoMosaic::Result leftResult = AutoMosaic::detect(leftUrl);
            int leftUrlCount = 0;
            QRect leftUrlBlock;
            for (const AutoMosaic::Detected &block : leftResult.blocks)
                if (block.kind == Kind::Url) { ++leftUrlCount; leftUrlBlock = block.rect; }
            check(leftUrlCount == 1, "a left-aligned link across three tiles stays one block");
            check(leftUrlBlock.width() >= 2500, "the left-aligned link covers the whole line");
            int leftLineBlocks = 0;
            for (const AutoMosaic::Detected &block : leftResult.blocks)
                if (block.rect.center().y() > 100 && block.rect.center().y() < 260) ++leftLineBlocks;
            check(leftLineBlocks == 1, "the left-aligned link leaves no tail block");
        }
    }

    {   // End-to-end overlay scan: activation turns recognized identifiers into blocks.
        auto overlay = testOverlay();
        overlay->m_frameSource = [] { return QVector<ScreenFrame>{}; };
        QImage canvas(1000, 400, QImage::Format_RGB32);
        canvas.fill(Qt::white);
        {
            QPainter p(&canvas);
            QFont font(QStringLiteral("Arial"));
            font.setPixelSize(32);
            p.setFont(font);
            p.setPen(Qt::black);
            p.drawText(40, 80, QStringLiteral("alice.brown927@qq.com"));
            p.drawText(40, 170, QStringLiteral("https://pixpin.cn/docs/"));
            p.drawText(40, 260, QStringLiteral("ABC123"));
        }
        overlay->setImage(canvas);
        overlay->presetSelection(QRect(0, 0, 1000, 400));
        overlay->m_mode = Adjusting;
        const QString assets = QCoreApplication::applicationDirPath();
        if (OcrEngine::assetError(assets).isEmpty()) {
            overlay->selectAutoMosaic();
            check(waitUntil([&overlay] { return !overlay->m_autoPending; }),
                  "the overlay scan finishes");
            check(overlay->m_autoReveal, "the result frames are revealed as one batch");
            check(waitUntil([&overlay] { return !overlay->m_autoReveal; }, 5000),
                  "the result frames stay visible for two seconds");
            check(overlay->m_autoMosaic, "auto mode stays active after the batch reveal");
            check(overlay->m_autoLinkButton != nullptr,
                  "the auto strip stays available after detection");
            const Annotation *found = overlay->autoMosaicAnnotation();
            check(found && found->autoBlocks.size() >= 2,
                  "activation installs the recognized blocks");
            int email = 0, url = 0;
            for (const Annotation::AutoBlock &block : found->autoBlocks) {
                check(QRect(0, 0, 1000, 400).contains(block.rect), "blocks map into the capture");
                check(!block.included, "detected blocks start as uncovered candidates");
                if (block.key.startsWith(QStringLiteral("email:"))) ++email;
                if (block.key.startsWith(QStringLiteral("url:"))) ++url;
            }
            check(email > 0 && url > 0, "the overlay keeps e-mail and link blocks");
            const int cachedCount = found->autoBlocks.size();
            check(overlay->m_autoDetectedRect
                      == overlay->m_sel.normalized().intersected(overlay->rect()),
                  "the finished scan caches its selection");
            overlay->selectMosaicMode(false);
            const QRect cachedSelection = overlay->m_autoDetectedRect;
            overlay->selectAutoMosaic();
            // Rebuilding the strip can resize the canvas on some DPIs; a rescan is
            // correct only when the selection no longer matches the cached one.
            check(!overlay->m_autoPending
                      || cachedSelection != overlay->m_sel.normalized().intersected(overlay->rect()),
                  "returning to auto mode reuses the cached scan");
            found = overlay->autoMosaicAnnotation();
            check(found && found->autoBlocks.size() == cachedCount,
                  "the cached candidates survive a tool switch");
            const QPoint at = found->autoBlocks.first().rect.center();
            QMouseEvent click = mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
            QMouseEvent up = mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
            overlay->mousePressEvent(&click);
            overlay->mouseReleaseEvent(&up);
            check(overlay->autoMosaicAnnotation()->autoBlocks.first().included,
                  "clicking a candidate applies its mosaic");
            overlay->mousePressEvent(&click);
            overlay->mouseReleaseEvent(&up);
            check(!overlay->autoMosaicAnnotation()->autoBlocks.first().included,
                  "clicking the covered block removes its mosaic");
            for (int i = 0; i < 3 && overlay->autoMosaicAnnotation(); ++i) overlay->undoAnnotation();
            check(!overlay->autoMosaicAnnotation(), "undo removes the detected candidate batch");

            // A scan that failed (the OCR engine was busy) must stay retryable.
            OcrEngine::instance().busy.store(true);
            overlay->selectAutoMosaic();
            check(waitUntil([&overlay] { return !overlay->m_autoPending; }),
                  "a scan with a busy OCR engine finishes");
            check(overlay->m_autoDetectedRect.isNull(),
                  "a failed scan is not cached as if it had run");
            OcrEngine::instance().busy.store(false);
            overlay->selectAutoMosaic();
            check(overlay->m_autoPending, "entering auto mode retries the failed scan");
            overlay->cancelAutoMosaicDetection();
        }
        overlay->close();
    }

    {   // Native-to-logical mapping keeps blocks on the content at fractional DPR.
        auto overlay = testOverlay();
        AutoMosaic::Result result;
        result.blocks.append({QRect(60, 120, 300, 45), QStringLiteral("email:a@b.com"), Kind::Email});
        overlay->applyAutoMosaicResult(result, QRect(0, 0, 1000, 400), 1.5);
        const Annotation *mapped = overlay->autoMosaicAnnotation();
        check(mapped && mapped->autoBlocks.size() == 1
                  && mapped->autoBlocks.first().rect == QRect(40, 80, 200, 30),
              "native detections map back to logical blocks");
        overlay->close();
    }

    {   // Mosaic sub-strip entry and auto-only strip.
        auto overlay = testOverlay();
        overlay->m_frameSource = [] { return QVector<ScreenFrame>{}; };
        QImage canvas(1000, 600, QImage::Format_RGB32);
        canvas.fill(QColor(250, 250, 248));
        overlay->setImage(canvas);
        overlay->presetSelection(QRect(0, 0, 1000, 600));
        overlay->m_mode = Adjusting;
        check(overlay->m_autoMosaicButton == nullptr,
              "no standalone auto button is created on the main toolbar");
        overlay->m_toolBtn[int(ATool::Mosaic)]->click();
        QToolButton *autoButton = overlay->m_toolOptions->findChild<QToolButton *>(
            QStringLiteral("autoMosaic"));
        check(autoButton != nullptr, "the auto entry lives in the mosaic sub-strip");
        check(autoButton->property("lenssnipToolId").isNull(),
              "the sub-strip entry is not a main-toolbar tool");
        check(overlay->m_toolOptions->findChild<QToolButton *>(QStringLiteral("mosaicBrush")),
              "the regular mosaic strip still has its brush entry");
        check(autoButton->isChecked() == false, "the auto entry starts unarmed");
        autoButton->click();
        check(overlay->m_autoMosaic && overlay->m_atool == ATool::Mosaic,
              "the sub-strip entry arms the mosaic tool in auto mode");
        check(overlay->m_autoMosaicButton != nullptr && overlay->m_autoMosaicButton->isChecked(),
              "the sub-strip entry highlights while active");
        QToolButton *linkButton = overlay->m_toolOptions->findChild<QToolButton *>(
            QStringLiteral("autoMosaicLink"));
        check(overlay->m_toolOptions->findChild<QComboBox *>(QStringLiteral("autoMosaicMode"))
                  && overlay->m_toolOptions->findChild<QSlider *>(QStringLiteral("autoMosaicStrength"))
                  && linkButton,
              "the auto strip exposes mode, strength and link toggling");
        check(linkButton->isChecked() && linkButton->text() == QStringLiteral("联动遮挡"),
              "link toggling is one button and defaults on");
        check(overlay->m_toolOptions->findChild<QToolButton *>(QStringLiteral("autoMosaic")),
              "the auto strip keeps its mode button visible for leaving");
        overlay->selectMosaicMode(false);
        check(!overlay->m_autoMosaic, "choosing a manual mosaic mode leaves auto");
        check(overlay->m_toolOptions->findChild<QToolButton *>(QStringLiteral("autoMosaic")),
              "the auto entry returns to the manual mosaic strip");
        overlay->selectAutoMosaic();

        // Two identical e-mails must sync; a different identifier must not.
        overlay->m_anns.clear();
        Annotation ann;
        ann.tool = ATool::Mosaic;
        ann.autoMosaic = true;
        ann.rect = QRect(20, 20, 400, 240);
        ann.autoBlocks = {{QRect(40, 40, 260, 30), QStringLiteral("email:alice@example.com"), true},
                          {QRect(40, 120, 260, 30), QStringLiteral("email:alice@example.com"), true},
                          {QRect(40, 200, 260, 30), QStringLiteral("email:bob@example.com"), true}};
        overlay->m_anns.append(ann);
        overlay->m_autoMosaicSync = true;
        overlay->toggleAutoMosaicBlock(0, 0);
        check(!overlay->m_anns[0].autoBlocks[0].included
                  && !overlay->m_anns[0].autoBlocks[1].included
                  && overlay->m_anns[0].autoBlocks[2].included,
              "sync toggles every identical block and only those");
        overlay->undoAnnotation();
        check(overlay->m_anns[0].autoBlocks[0].included
                  && overlay->m_anns[0].autoBlocks[1].included,
              "one undo restores the synced group");
        overlay->m_autoMosaicSync = false;
        overlay->toggleAutoMosaicBlock(0, 0);
        check(!overlay->m_anns[0].autoBlocks[0].included
                  && overlay->m_anns[0].autoBlocks[1].included,
              "sync off toggles only the clicked block");
        overlay->undoAnnotation();

        // Hover and click: the block under the pointer is toggled, not moved.
        overlay->m_sel = QRect(0, 0, 1000, 600);
        overlay->m_autoHoverAnn = -1;
        overlay->m_autoHoverBlock = -1;
        QMouseEvent press = mouse(QEvent::MouseButtonPress, QPoint(80, 55), Qt::LeftButton, Qt::LeftButton);
        QMouseEvent release = mouse(QEvent::MouseButtonRelease, QPoint(80, 55), Qt::LeftButton, Qt::NoButton);
        overlay->mousePressEvent(&press);
        overlay->mouseReleaseEvent(&release);
        check(!overlay->m_anns[0].autoBlocks[0].included
                  && overlay->m_anns[0].autoBlocks[1].included,
              "a canvas click toggles only the block under the pointer");
        overlay->undoAnnotation();

        // A box over detected text toggles that text block instead of painting a
        // solid rectangle; starting the box also retires the candidate hover frame.
        overlay->m_autoMosaic = true;
        overlay->m_atool = ATool::Mosaic;
        overlay->m_mode = Adjusting;
        overlay->m_anns[0].autoBlocks[0].included = false;
        overlay->m_autoHoverAnn = -1;
        overlay->m_autoHoverBlock = -1;
        overlay->m_autoReveal = false;
        QMouseEvent hoverCandidate(QEvent::MouseMove, QPointF(80, 55), Qt::NoButton, Qt::NoButton,
                                   Qt::NoModifier);
        overlay->mouseMoveEvent(&hoverCandidate);
        check(overlay->m_autoHoverAnn == 0, "a candidate hover is active before the box drag");
        const int boxedBefore = overlay->m_anns[0].autoBlocks.size();
        QMouseEvent boxPress = mouse(QEvent::MouseButtonPress, QPoint(30, 30), Qt::LeftButton, Qt::LeftButton);
        QMouseEvent boxMove = mouse(QEvent::MouseMove, QPoint(320, 90), Qt::NoButton, Qt::LeftButton);
        QMouseEvent boxRelease = mouse(QEvent::MouseButtonRelease, QPoint(320, 90), Qt::LeftButton, Qt::NoButton);
        overlay->mousePressEvent(&boxPress);
        check(overlay->m_autoHoverAnn == -1, "starting a box retires the candidate hover frame");
        QImage traceLayer(500, 300, QImage::Format_ARGB32_Premultiplied);
        traceLayer.fill(Qt::transparent);
        {
            QPainter painter(&traceLayer);
            overlay->paintAutoMosaicOverlay(painter);
        }
        check(qAlpha(traceLayer.pixel(40, 40)) == 0,
              "starting a box removes the old hover frame from the window");
        overlay->mouseMoveEvent(&boxMove);
        overlay->mouseReleaseEvent(&boxRelease);
        check(overlay->m_anns[0].autoBlocks.size() == boxedBefore,
              "a box over text includes the text block without adding a rectangle");
        check(overlay->m_anns[0].autoBlocks[0].included,
              "the text inside the box is mosaicked");
        overlay->undoAnnotation();
        check(!overlay->m_anns[0].autoBlocks[0].included,
              "undo restores the boxed text toggle");

        // A drag that starts on a candidate becomes a box instead of a dead gesture.
        overlay->m_anns[0].autoBlocks[0].included = false;
        overlay->m_anns[0].autoBlocks[1].included = false;
        QMouseEvent fromCandidatePress = mouse(QEvent::MouseButtonPress, QPoint(80, 55),
                                               Qt::LeftButton, Qt::LeftButton);
        QMouseEvent fromCandidateMove = mouse(QEvent::MouseMove, QPoint(320, 200),
                                              Qt::NoButton, Qt::LeftButton);
        QMouseEvent fromCandidateRelease = mouse(QEvent::MouseButtonRelease, QPoint(320, 200),
                                                 Qt::LeftButton, Qt::NoButton);
        overlay->mousePressEvent(&fromCandidatePress);
        overlay->mouseMoveEvent(&fromCandidateMove);
        check(overlay->m_autoDragging && overlay->m_autoHoverAnn == -1,
              "a drag from a candidate becomes a box and retires its hover frame");
        overlay->mouseReleaseEvent(&fromCandidateRelease);
        check(overlay->m_anns[0].autoBlocks[0].included
                  && overlay->m_anns[0].autoBlocks[1].included,
              "the box started on a candidate mosaics the text it covers");
        overlay->undoAnnotation();

        // Drag on empty canvas adds one manual block; undo removes it.
        const int before = overlay->m_anns[0].autoBlocks.size();
        QMouseEvent dragPress = mouse(QEvent::MouseButtonPress, QPoint(500, 400), Qt::LeftButton, Qt::LeftButton);
        QMouseEvent dragMove = mouse(QEvent::MouseMove, QPoint(680, 500), Qt::NoButton, Qt::LeftButton);
        QMouseEvent dragRelease = mouse(QEvent::MouseButtonRelease, QPoint(680, 500), Qt::LeftButton, Qt::NoButton);
        overlay->mousePressEvent(&dragPress);
        overlay->mouseMoveEvent(&dragMove);
        overlay->mouseReleaseEvent(&dragRelease);
        check(overlay->m_anns[0].autoBlocks.size() == before + 1,
              "dragging on the canvas adds a manual block");
        check(overlay->m_anns[0].autoBlocks.last().key.isEmpty(),
              "manual blocks are outside the sync groups");
        const QPoint manualAt = overlay->m_anns[0].autoBlocks.last().rect.center();
        QMouseEvent manualPress = mouse(QEvent::MouseButtonPress, manualAt, Qt::LeftButton, Qt::LeftButton);
        QMouseEvent manualRelease = mouse(QEvent::MouseButtonRelease, manualAt, Qt::LeftButton, Qt::NoButton);
        overlay->mousePressEvent(&manualPress);
        overlay->mouseReleaseEvent(&manualRelease);
        check(!overlay->m_anns[0].autoBlocks.last().included,
              "clicking a manual block removes its mosaic");
        overlay->undoAnnotation();
        check(overlay->m_anns[0].autoBlocks.last().included,
              "undo restores the manual block mosaic");
        overlay->undoAnnotation();
        check(overlay->m_anns[0].autoBlocks.size() == before,
              "one more undo removes the manual block");

        // Esc (or any tool switch) during a box drag leaves no dashed preview.
        QMouseEvent abortPress = mouse(QEvent::MouseButtonPress, QPoint(500, 400),
                                       Qt::LeftButton, Qt::LeftButton);
        overlay->mousePressEvent(&abortPress);
        check(overlay->m_autoDragging, "a press on empty canvas starts the box preview");
        overlay->setToolNone();
        check(!overlay->m_autoDragging && overlay->m_autoDragRect.isNull(),
              "leaving auto mode clears the drag preview");

        // Per-block effect images are reused between repaints instead of being
        // rebuilt from the full capture on every click.
        overlay->m_anns[0].autoBlocks[0].included = true;
        QImage source = overlay->m_shot.toImage().convertToFormat(QImage::Format_ARGB32);
        source.setDevicePixelRatio(1);
        QImage target(500, 300, QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::transparent);
        {
            QPainter painter(&target);
            overlay->drawAnnotation(painter, overlay->m_anns[0], source, 0);
        }
        const qint64 effectKey = overlay->m_anns[0].autoBlocks[0].effect.cacheKey();
        check(!overlay->m_anns[0].autoBlocks[0].effect.isNull(),
              "included blocks build their own effect image");
        {
            QPainter painter(&target);
            overlay->drawAnnotation(painter, overlay->m_anns[0], source, 0);
        }
        check(overlay->m_anns[0].autoBlocks[0].effect.cacheKey() == effectKey,
              "block effects stay cached between repaints");

        // Candidates stay invisible until the pointer reaches them.
        overlay->m_autoMosaic = true;
        overlay->m_atool = ATool::Mosaic;
        overlay->m_mode = Adjusting;
        overlay->m_autoHoverAnn = -1;
        overlay->m_autoHoverBlock = -1;
        QMouseEvent hoverEvent(QEvent::MouseMove, QPointF(80, 55), Qt::NoButton, Qt::NoButton,
                               Qt::NoModifier);
        overlay->mouseMoveEvent(&hoverEvent);
        check(overlay->m_autoHoverAnn == 0 && overlay->m_autoHoverBlock == 0,
              "hovering a candidate records the block under the pointer");
        QImage hovered(overlay->size(), QImage::Format_RGB32);
        hovered.fill(Qt::black);
        overlay->render(&hovered);
        const QRgb wash = hovered.pixel(50, 50);
        check(wash != qRgb(255, 255, 255),
              "the hovered block paints a visible frame on the next repaint");
        QImage layer(500, 300, QImage::Format_ARGB32_Premultiplied);
        layer.fill(Qt::transparent);
        {
            QPainter painter(&layer);
            overlay->paintAutoMosaicOverlay(painter);
        }
        check(qAlpha(layer.pixel(40, 40)) > 0, "the hovered block gets a frame and wash");
        check(qAlpha(layer.pixel(40, 120)) == 0, "unhovered candidates stay invisible");
        QMouseEvent leaveEvent(QEvent::MouseMove, QPointF(1200, 700), Qt::NoButton, Qt::NoButton,
                               Qt::NoModifier);
        overlay->mouseMoveEvent(&leaveEvent);
        check(overlay->m_autoHoverAnn == -1 && overlay->m_autoHoverBlock == -1,
              "leaving the candidate area retires the hover frame");

        // Wheel adjusts the applied mosaic / blur strength.
        overlay->m_autoMosaicLevels[0] = overlay->m_autoMosaicLevels[1] = 6;
        overlay->m_autoMosaicStrength = 6;
        overlay->m_anns[0].mosaicStrength = 6;
        QWheelEvent wheel(QPointF(80, 55), QPointF(80, 55), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        overlay->wheelEvent(&wheel);
        check(overlay->m_autoMosaicStrength == 7 && overlay->m_anns[0].mosaicStrength == 7,
              "wheel tunes the applied mosaic strength");
        overlay->close();
    }

    if (!previewPath.isEmpty()) {
        // Deterministic paint previews: the scan state and the one-shot result
        // frames do not depend on how fast OCR happens to finish.
        auto overlay = testOverlay();
        QImage canvas(1200, 600, QImage::Format_RGB32);
        canvas.fill(QColor(242, 243, 246));
        {
            QPainter p(&canvas);
            QFont font(QStringLiteral("Microsoft YaHei UI"));
            font.setPixelSize(24);
            p.setFont(font);
            p.setPen(QColor(32, 34, 40));
            p.drawText(80, 145, QStringLiteral("zhang.wei@lumen.dev"));
            p.drawText(80, 225, QStringLiteral("+86 138 0013 8000"));
            p.setBrush(QColor(210, 120, 90));
            p.setPen(Qt::NoPen);
            p.drawEllipse(QRect(760, 90, 64, 64));
            p.setBrush(QColor(32, 34, 40));
            p.drawRect(QRect(760, 210, 120, 120));
        }
        overlay->setImage(canvas);
        overlay->presetSelection(QRect(40, 60, 1000, 400));
        overlay->m_autoMosaic = true;
        overlay->m_atool = ATool::Mosaic;
        overlay->m_mode = Adjusting;
        Annotation preview;
        preview.tool = ATool::Mosaic;
        preview.autoMosaic = true;
        preview.rect = QRect(40, 60, 1000, 400);
        preview.autoBlocks = {
            {QRect(76, 118, 360, 38), QStringLiteral("email:a"), false},
            {QRect(76, 198, 320, 38), QStringLiteral("phone:a"), true},
            {QRect(756, 86, 72, 72), QStringLiteral("image:a"), false},
            {QRect(756, 206, 128, 128), QStringLiteral("qr:a"), false}
        };
        overlay->m_anns.append(preview);
        overlay->updateToolOptions();
        overlay->m_autoPending = true;
        overlay->m_autoScanElapsed.start();
        QEventLoop wait;
        QTimer::singleShot(480, &wait, &QEventLoop::quit);
        wait.exec();
        QImage frame(overlay->size(), QImage::Format_RGB32);
        frame.fill(QColor(12, 14, 18));
        overlay->render(&frame);
        frame.copy(QRect(0, 0, 1120, 560)).save(previewPath + QStringLiteral("-scan.png"));
        overlay->m_autoPending = false;
        overlay->m_autoReveal = true;
        frame.fill(QColor(12, 14, 18));
        overlay->render(&frame);
        frame.copy(QRect(0, 0, 1120, 560)).save(previewPath + QStringLiteral("-result.png"));
        overlay->m_autoReveal = false;
        frame.fill(QColor(12, 14, 18));
        overlay->render(&frame);
        frame.copy(QRect(0, 0, 1120, 560)).save(previewPath + QStringLiteral("-idle.png"));
        overlay->close();
    }

    if (!previewPath.isEmpty()) std::printf("preview prefix: %s\n", qPrintable(previewPath));
    std::printf("PASS: auto mosaic detection and interaction checks\n");
    return 0;
}
