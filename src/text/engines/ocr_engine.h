#pragma once

#include <QCoreApplication>
#include <QImage>
#include <QVector>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QtMath>
#include <QPolygonF>
#include <qt_windows.h>
#include <array>
#include <atomic>
#include <memory>

struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtMemoryInfo;
struct OrtAllocator;
struct OrtStatus;

// ---------- Offline text recognition and image/text comparison. ----------
struct OcrLine {
    QString text;
    QPolygonF box;
    QVector<QPolygonF> glyphs;   // One quad per QChar of text in image pixels; empty when unknown.
    float score = 0;
};
struct OcrResult {
    QVector<OcrLine> lines;
    QVector<QPolygonF> boxes;   // All detected line boxes in source-image coordinates, before recognition filtering.
    QString error;
    bool cancelled = false;
    qint64 elapsed = 0;
    QString text() const {
        QStringList rows;
        for (const OcrLine &line : lines) rows.append(line.text);
        return rows.join(QLatin1Char('\n'));
    }
};
using OcrCancel = std::shared_ptr<std::atomic_bool>;

class OcrEngine
{
    Q_DECLARE_TR_FUNCTIONS(OcrEngine)
public:
    static OcrEngine &instance();
    ~OcrEngine();
    std::atomic_bool busy{false};
    static QString assetError(const QString &root);
    QString load(const QString &root);
    QString version() const;
    int dictionarySize() const;
    static void testAlgorithms();
    static int testLanguages(const QString &previewPath);
    static QSize detectionSize(const QSize &source);
    OcrResult recognize(QImage image, const OcrCancel &cancel);
    OcrResult detect(QImage image, const OcrCancel &cancel);
    // Recognition for boxes produced by detect(): one entry per box, in order.
    // Boxes that cannot be read keep an empty text so callers can still cover them.
    OcrResult recognizeBoxes(QImage image, const QVector<QPolygonF> &boxes, const OcrCancel &cancel);
    // centers: per QChar of the text, the middle of its CTC steps, in steps.
    static OcrLine decode(const QVector<float> &data, int steps, const QStringList &characters,
                          QVector<float> *centers = nullptr);
    // Splits a line box into one quad per character; centers are fractions along the reading axis.
    static QVector<QPolygonF> glyphBoxes(const QPolygonF &box, const QVector<float> &centers);
    static QVector<QPolygonF> textBoxes(const QVector<float> &prob, const QSize &size, const QSize &original,
                                       const OcrCancel &cancel);
private:
    OcrResult detection(QImage image, const OcrCancel &cancel);
    OcrLine recognizeLine(const QImage &image, const QPolygonF &box, const OcrCancel &cancel);
    static QVector<QPolygonF> mergeLineBoxes(QVector<QPolygonF> boxes);
    OcrEngine() = default;
    OcrEngine(const OcrEngine &) = delete;
    void checked(OrtStatus *status);
    void reset();
    void names(OrtSession *session, QByteArray &input, QByteArray &output);
    QVector<float> run(OrtSession *session, const QByteArray &inputName, const QByteArray &outputName,
                       QVector<float> data, const std::array<int64_t, 4> &dimensions, QVector<int64_t> &shape);
    static QVector<float> normalized(const QImage &input, int width);
    static QPolygonF minimumBox(QVector<QPointF> points);
    static QImage lineImage(const QImage &image, const QPolygonF &box);
    HMODULE m_dll = nullptr;
    const OrtApi *m_api = nullptr;
    OrtEnv *m_env = nullptr;
    OrtSession *m_det = nullptr, *m_rec = nullptr;
    OrtMemoryInfo *m_memory = nullptr;
    OrtAllocator *m_allocator = nullptr;
    QByteArray m_detInput, m_detOutput, m_recInput, m_recOutput;
    QStringList m_characters;
    QString m_version;
    int m_recHeight = 48;
};

// Row ends for splitting a tall image into recognition-sized chunks, preferring uniform rows.
QVector<int> ocrCuts(const QImage &image, int limit);
// Tall captures keep the OCR detector's native text size in every chunk.
OcrResult recognizeChunked(const QImage &image, const OcrCancel &cancel);
OcrResult detectChunked(const QImage &image, const OcrCancel &cancel);
