#include "text/engines/ocr_engine.h"
#include "platform/output.h"
#include "tests/test_support.h"
#include "shared/widgets.h"
#include "pin/pin_window.h"
#include "longshot/long_capture.h"
#include "text/cards/ocr_dialog.h"
#include <QImage>
#include <QVector>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QFont>
#include <QPainter>
#include <QtMath>
#include <QPolygonF>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>

void OcrEngine::testAlgorithms() {
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
    };
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const QSize size(240,160);
    QVector<float> probability(size.width()*size.height(),0);
    for (int row = 0; row < 3; ++row)
        for (int y = 15+row*45; y < 25+row*45; ++y)
            for (int x = 40; x < 100; ++x) probability[y*size.width()+x] = 0.95f;
    const auto boxes = textBoxes(probability,size,size,cancel);
    check(boxes.size() == 3,"DB synthetic map returns three text rows");
    for (int row = 0; row < 3; ++row) {
        const double expansion = 59.0*9*1.6/(2*(59+9));
        const QRectF expected(40-expansion,15+row*45-expansion,59+2*expansion,9+2*expansion);
        const QRectF actual = boxes[row].boundingRect();
        check(QLineF(actual.topLeft(),expected.topLeft()).length() < 2
                  && QLineF(actual.bottomRight(),expected.bottomRight()).length() < 2,
              "DB unclip geometry and row ordering retain sub-two-pixel accuracy");
    }
    probability.fill(0.4f);
    check(textBoxes(probability,size,size,cancel).isEmpty(),"DB rejects low-scoring regions");
    cancel->store(true);
    check(textBoxes(probability,size,size,cancel).isEmpty(),"DB stops on cancellation");
    const QStringList dictionary{QString(),QStringLiteral("A"),QStringLiteral("B"),QStringLiteral(" ")};
    const int tokens[] = {1,1,0,1,2,0,3};
    QVector<float> logits(7*4,0);
    for (int i = 0; i < 7; ++i) logits[i*4+tokens[i]]=0.95f;
    const OcrLine line = decode(logits,7,dictionary);
    check(line.text == QStringLiteral("AAB ") && std::abs(line.score-0.95f)<0.00001f,
          "CTC removes repeated tokens and blanks but retains blank-separated repeats and spaces");
    QImage red(1,1,QImage::Format_RGB32); red.fill(Qt::red);
    const auto input = normalized(red,2);
    check(input == QVector<float>({-1,0,-1,0,1,0}),"detection and recognition use BGR, minus-one-to-one normalization and zero padding");
    const QSize reduced = detectionSize(QSize(2000,1000));
    check(reduced == QSize(1440,736),"large detection input shrinks and aligns to multiples of 32");
    QVector<float> centers;
    decode(logits,7,dictionary,&centers);
    check(centers == QVector<float>({1.0f,3.5f,4.5f,6.5f}),"CTC reports the middle of each emitted token's step run");
    const QPolygonF row{QPointF(0,0),QPointF(100,0),QPointF(100,20),QPointF(0,20)};
    const auto glyphs = glyphBoxes(row,{0.125f,0.375f,0.625f});   // Binary-exact fractions.
    check(glyphs.size()==3 && glyphs[0][0]==QPointF(0,0) && glyphs[0][1]==QPointF(25,0)
              && glyphs[1][1]==QPointF(50,0) && glyphs[2][1]==QPointF(75,0) && glyphs[1][2]==QPointF(50,20),
          "glyph boxes split a line halfway between character centres and keep the line height");
    const QPolygonF column{QPointF(0,0),QPointF(20,0),QPointF(20,100),QPointF(0,100)};
    const auto stacked = glyphBoxes(column,{0.25f,0.75f});
    check(stacked.size()==2 && stacked[0][1]==QPointF(0,50) && stacked[0][3]==QPointF(20,0) && stacked[1][2]==QPointF(20,100),
          "tall line boxes split top to bottom");
    bool invalid = false;
    try { decode(QVector<float>{1},1,dictionary); } catch (const std::exception &) { invalid=true; }
    check(invalid,"CTC rejects malformed tensor dimensions");
    const QPolygonF first{QPointF(10,10),QPointF(60,10),QPointF(60,30),QPointF(10,30)};
    const QPolygonF second{QPointF(75,11),QPointF(125,11),QPointF(125,31),QPointF(75,31)};
    const QPolygonF below{QPointF(10,55),QPointF(60,55),QPointF(60,75),QPointF(10,75)};
    const auto merged = mergeLineBoxes({below,second,first});
    check(merged.size()==2 && merged[0].boundingRect()==QRectF(10,10,115,21)
              && merged[1].boundingRect()==QRectF(10,55,50,20),
          "same-row word boxes merge in reading order without merging another row");
    const OcrResult cancelled = instance().detect(red,cancel);
    check(cancelled.cancelled && cancelled.lines.isEmpty() && cancelled.boxes.isEmpty(),
          "detect-only pre-cancellation returns no rows or boxes");
    const OcrResult cancelledChunks = detectChunked(red,cancel);
    check(cancelledChunks.cancelled && cancelledChunks.lines.isEmpty() && cancelledChunks.boxes.isEmpty(),
          "chunked detect-only preserves cancellation semantics");
    cancel->store(false);
    const OcrResult invalidDetect = instance().detect(QImage(),cancel);
    check(!invalidDetect.error.isEmpty() && invalidDetect.lines.isEmpty() && invalidDetect.boxes.isEmpty(),
          "detect-only rejects empty images without recognition assets");
    const OcrResult invalidChunks = detectChunked(QImage(),cancel);
    check(!invalidChunks.error.isEmpty() && invalidChunks.lines.isEmpty() && invalidChunks.boxes.isEmpty(),
          "chunked detect-only preserves empty-image errors");
    std::puts("PASS: OCR probability boxes, normalization, CTC, resize and cancellation checks");
}

int OcrEngine::testLanguages(const QString &previewPath) {
    auto &engine = instance();
    const QString missing = assetError(QCoreApplication::applicationDirPath());
    if (!missing.isEmpty()) { std::printf("UNAVAILABLE: %s\n",qPrintable(missing)); return 0; }
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    struct Sample { const char *name; QString text; bool supported = true; };
    const QVector<Sample> samples{
        {"Simplified-Chinese",QStringLiteral("截图文字识别")},
        {"Traditional-Chinese",QStringLiteral("繁體中文測試")},
        {"English",QStringLiteral("Hello World 123")},
        {"Japanese",QStringLiteral("こんにちは世界")},
        {"Korean",QStringLiteral("안녕하세요 세계"),false},
        {"Spanish",QStringLiteral("Hola señor 123")},
        {"Portuguese",QStringLiteral("Olá mundo 123")},
        {"German",QStringLiteral("Grüße Welt 123")},
        {"French",QStringLiteral("Bonjour été 123")},
        {"Russian",QStringLiteral("Привет мир 123"),false},
        {"Arabic",QStringLiteral("مرحبا بالعالم"),false},
        {"Greek",QStringLiteral("Καλημέρα κόσμε"),false},
        {"Mixed",QStringLiteral("截图 繁體 日本語 Hello été")}
    };
    int failures = 0, matchesSupported = 0;
    for (qreal dpr : {1.0,1.5,2.0}) for (const auto &sample : samples) {
        QImage image(qRound(800*dpr),qRound(180*dpr),QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.scale(dpr,dpr);
        QFont font(QStringLiteral("Microsoft YaHei")); font.setPixelSize(40); painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(QRectF(32,20,736,140),Qt::AlignVCenter|Qt::AlignLeft,sample.text);
        painter.end();
        image.setDevicePixelRatio(dpr);
        const OcrResult result = engine.recognize(image,cancel);
        const bool matches = result.error.isEmpty() && !result.cancelled && result.text()==sample.text;
        if (sample.supported) { if (!matches) ++failures; else ++matchesSupported; }
        std::printf("%s language=%s dpr=%.1f elapsed=%lldms expected=[%s] actual=[%s] error=[%s]\n",
                    !sample.supported ? "UNSUPPORTED" : matches ? "PASS" : "FAIL",sample.name,dpr,result.elapsed,
                    qPrintable(sample.text),qPrintable(result.text()),qPrintable(result.error));
        if (!previewPath.isEmpty() && !image.save(previewPath+QStringLiteral("-%1-%2.png").arg(QLatin1String(sample.name)).arg(dpr))) return 1;
    }
    std::printf("LANGUAGE SUMMARY: %d/27 supported exact matches; %d supported mismatches; 12 unsupported probes (not passes)\n",matchesSupported,failures);
    return failures ? 1 : 0;
}
