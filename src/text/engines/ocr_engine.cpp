#include "onnxruntime_c_api.h"
#include <QCoreApplication>
#include "text/engines/ocr_engine.h"
#include <QImage>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QTransform>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <QThread>
#include <QElapsedTimer>
#include <QMutex>
#include <QFileInfo>
#include <QDir>
#include <QPolygonF>
#include <qt_windows.h>
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <limits>
#include <cmath>

OcrEngine &OcrEngine::instance() { static OcrEngine engine; return engine; }

// Pins recognize in the background as soon as they open; lazy load/reset is not reentrant.
static QMutex ocrSerial;

OcrEngine::~OcrEngine()
{
    // shortcut: leave the ORT DLL and sessions to process exit. FreeLibrary
    // during static destruction can run TLS callbacks after Qt's thread pool
    // has gone away; reset() still handles explicit reloads and load failures.
}

QString OcrEngine::assetError(const QString &root) {
    for (const QString &file : {QStringLiteral("onnxruntime.dll"), QStringLiteral("models/PP-OCRv6_det_small.onnx"),
                               QStringLiteral("models/PP-OCRv6_rec_small.onnx")})
        if (!QFileInfo(root + QLatin1Char('/') + file).isFile())
            return tr("缺少 OCR 文件：%1\n请将运行时与模型放回程序目录后重试。").arg(file);
    return {};
}

QString OcrEngine::load(const QString &root) {
    if (m_det && m_rec) return {};
    const QString missing = assetError(root);
    if (!missing.isEmpty()) return missing;
    reset();
    try {
        const QString dll = QDir::toNativeSeparators(root + QStringLiteral("/onnxruntime.dll"));
        m_dll = LoadLibraryExW(reinterpret_cast<LPCWSTR>(dll.utf16()), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_dll) throw std::runtime_error("Cannot load the OCR runtime or its dependencies");
        const auto entry = reinterpret_cast<const OrtApiBase *(ORT_API_CALL *)()>(GetProcAddress(m_dll, "OrtGetApiBase"));
        if (!entry || !(m_api = entry()->GetApi(ORT_API_VERSION)))
            throw std::runtime_error("Incompatible OCR runtime API");
        m_version = QString::fromLatin1(entry()->GetVersionString());
        checked(m_api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "lenssnip-ocr", &m_env));
        OrtSessionOptions *raw = nullptr;
        checked(m_api->CreateSessionOptions(&raw));
        std::unique_ptr<OrtSessionOptions, decltype(m_api->ReleaseSessionOptions)> options(raw, m_api->ReleaseSessionOptions);
        checked(m_api->SetIntraOpNumThreads(raw, qBound(1, QThread::idealThreadCount(), 4)));
        checked(m_api->SetInterOpNumThreads(raw, 1));
        checked(m_api->SetSessionGraphOptimizationLevel(raw, ORT_ENABLE_ALL));
        checked(m_api->DisableCpuMemArena(raw));
        checked(m_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m_memory));
        checked(m_api->GetAllocatorWithDefaultOptions(&m_allocator));
        const QString det = QDir::toNativeSeparators(root + QStringLiteral("/models/PP-OCRv6_det_small.onnx"));
        const QString rec = QDir::toNativeSeparators(root + QStringLiteral("/models/PP-OCRv6_rec_small.onnx"));
        checked(m_api->CreateSession(m_env, reinterpret_cast<const wchar_t *>(det.utf16()), raw, &m_det));
        checked(m_api->CreateSession(m_env, reinterpret_cast<const wchar_t *>(rec.utf16()), raw, &m_rec));
        names(m_det, m_detInput, m_detOutput);
        names(m_rec, m_recInput, m_recOutput);
        OrtTypeInfo *type = nullptr;
        checked(m_api->SessionGetInputTypeInfo(m_rec,0,&type));
        std::unique_ptr<OrtTypeInfo,decltype(m_api->ReleaseTypeInfo)> inputType(type,m_api->ReleaseTypeInfo);
        const OrtTensorTypeAndShapeInfo *tensor = nullptr;
        checked(m_api->CastTypeInfoToTensorInfo(type,&tensor));
        if (!tensor) throw std::runtime_error("Recognition input must be a tensor");
        size_t rank = 0;
        checked(m_api->GetDimensionsCount(tensor,&rank));
        std::array<int64_t,4> dimensions{};
        if (rank != 4) throw std::runtime_error("Recognition input must be NCHW");
        checked(m_api->GetDimensions(tensor,dimensions.data(),4));
        if ((dimensions[1] != -1 && dimensions[1] != 3)
            || (dimensions[2] != -1 && dimensions[2] != 32 && dimensions[2] != 48))
            throw std::runtime_error("Unsupported recognition input dimensions");
        // Dynamic-height exports use the deployed model family's 48-pixel training input.
        m_recHeight = dimensions[2] == -1 ? 48 : int(dimensions[2]);
        OrtModelMetadata *metadata = nullptr;
        checked(m_api->SessionGetModelMetadata(m_rec, &metadata));
        std::unique_ptr<OrtModelMetadata, decltype(m_api->ReleaseModelMetadata)> info(metadata, m_api->ReleaseModelMetadata);
        char *dictionary = nullptr;
        checked(m_api->ModelMetadataLookupCustomMetadataMap(metadata, m_allocator, "character", &dictionary));
        if (!dictionary) throw std::runtime_error("The recognition model has no character dictionary");
        m_characters = QString::fromUtf8(dictionary).split(QLatin1Char('\n'));
        m_allocator->Free(m_allocator, dictionary);
        if (!m_characters.isEmpty() && m_characters.last().isEmpty()) m_characters.removeLast();
        for (QString &character : m_characters) if (character.endsWith(QLatin1Char('\r'))) character.chop(1);
        if (m_characters.isEmpty() || m_characters.contains(QString()))
            throw std::runtime_error("Invalid recognition dictionary");
        m_characters.prepend(QString());
        m_characters.append(QStringLiteral(" "));
        return {};
    } catch (const std::exception &error) {
        reset();
        return tr("OCR 加载失败：%1").arg(QString::fromUtf8(error.what()));
    }
}

QString OcrEngine::version() const { return m_version; }

int OcrEngine::dictionarySize() const { return int(m_characters.size()); }

QSize OcrEngine::detectionSize(const QSize &source) {
    const double ratio = qMin(1.0, 1440.0 / qMax(source.width(), source.height()));
    return QSize(qMax(32, qRound(source.width() * ratio / 32) * 32),
                 qMax(32, qRound(source.height() * ratio / 32) * 32));
}

OcrResult OcrEngine::detection(QImage image, const OcrCancel &cancel) {
    OcrResult result;
    QElapsedTimer timer;
    timer.start();
    try {
        if (cancel->load()) { result.cancelled = true; return result; }
        if (image.isNull()) throw std::runtime_error("Empty screenshot");
        result.error = load(QCoreApplication::applicationDirPath());
        if (!result.error.isEmpty()) return result;
        if (cancel->load()) { result.cancelled=true; return result; }
        image.setDevicePixelRatio(1);
        const QSize size = detectionSize(image.size());
        const QImage reduced = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QVector<int64_t> shape;
        const QVector<float> probability = run(m_det, m_detInput, m_detOutput, normalized(reduced, size.width()),
                                                {1, 3, size.height(), size.width()}, shape);
        if (shape.size() != 4 || shape[0] != 1 || shape[1] != 1
            || shape[2] <= 0 || shape[3] <= 0 || shape[2] > 1440 || shape[3] > 1440)
            throw std::runtime_error("Unexpected detection output shape");
        const QSize mapSize{int(shape[3]), int(shape[2])};
        result.boxes = textBoxes(probability, mapSize, image.size(), cancel);
        result.cancelled = cancel->load();
        if (result.cancelled) result.boxes.clear();
    } catch (const std::exception &error) {
        result.lines.clear();
        result.boxes.clear();
        result.error = tr("识别失败：%1").arg(QString::fromUtf8(error.what()));
    }
    result.elapsed = timer.elapsed();
    return result;
}

OcrResult OcrEngine::detect(QImage image, const OcrCancel &cancel) {
    const QMutexLocker lock(&ocrSerial);
    return detection(image, cancel);
}

OcrResult OcrEngine::recognize(QImage image, const OcrCancel &cancel) {
    const QMutexLocker lock(&ocrSerial);
    QElapsedTimer timer;
    timer.start();
    image.setDevicePixelRatio(1);   // Recognition crops read physical pixels; detection only resets its own copy.
    OcrResult result = detection(image, cancel);
    if (result.cancelled || !result.error.isEmpty()) return result;
    try {
        for (const QPolygonF &box : result.boxes) {
            if (cancel->load()) {
                result.cancelled = true;
                result.lines.clear();
                result.boxes.clear();
                return result;
            }
            const OcrLine line = recognizeLine(image, box, cancel);
            if (!line.text.trimmed().isEmpty() && line.score >= 0.5f) result.lines.append(line);
        }
        result.cancelled = cancel->load();
        if (result.cancelled) {
            result.lines.clear();
            result.boxes.clear();
        }
    } catch (const std::exception &error) {
        result.lines.clear();
        result.boxes.clear();
        result.error = tr("识别失败：%1").arg(QString::fromUtf8(error.what()));
    }
    result.elapsed = timer.elapsed();
    return result;
}

OcrLine OcrEngine::recognizeLine(const QImage &image, const QPolygonF &box, const OcrCancel &cancel)
{
    OcrLine line;
    line.box = box;
    if (cancel->load()) return line;
    const QImage crop = lineImage(image, box);
    if (crop.isNull()) return line;
    const int contentWidth = qMax(1, int(std::ceil(double(m_recHeight) * crop.width() / crop.height())));
    const int width = qMax(320, (contentWidth + 31) / 32 * 32);
    if (width > 8192) throw std::runtime_error("Text line is too wide; select a smaller region");
    QImage resized(contentWidth, m_recHeight, QImage::Format_RGB32);
    if (resized.isNull()) throw std::runtime_error("Cannot resize a detected text line");
    QPainter painter(&resized);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(resized.rect(), crop);
    painter.end();
    QVector<int64_t> shape;
    const QVector<float> logits = run(m_rec, m_recInput, m_recOutput, normalized(resized, width),
                                      {1, 3, m_recHeight, width}, shape);
    if (shape.size() != 3 || shape[0] != 1 || shape[2] != m_characters.size())
        throw std::runtime_error("Recognition output does not match its dictionary");
    QVector<float> centers;
    line = decode(logits, int(shape[1]), m_characters, &centers);
    line.box = box;
    // A step spans width/steps input pixels; the line content fills the first contentWidth of them.
    for (float &center : centers) center = center * width / float(shape[1]) / contentWidth;
    line.glyphs = glyphBoxes(box, centers);
    return line;
}

OcrResult OcrEngine::recognizeBoxes(QImage image, const QVector<QPolygonF> &boxes, const OcrCancel &cancel)
{
    const QMutexLocker lock(&ocrSerial);
    QElapsedTimer timer;
    timer.start();
    OcrResult result;
    image.setDevicePixelRatio(1);
    result.error = load(QCoreApplication::applicationDirPath());
    if (!result.error.isEmpty()) return result;
    try {
        result.lines.reserve(boxes.size());
        for (const QPolygonF &box : boxes) {
            if (cancel->load()) {
                result.cancelled = true;
                result.lines.clear();
                return result;
            }
            OcrLine line;
            try {
                line = recognizeLine(image, box, cancel);
            } catch (const std::exception &) {
                // One unreadable line must not erase the other lines of the page.
                line.box = box;
            }
            if (line.score < 0.5f) {
                line.text.clear();
                line.glyphs.clear();
            }
            result.lines.append(line);
        }
    } catch (const std::exception &error) {
        result.lines.clear();
        result.error = tr("识别失败：%1").arg(QString::fromUtf8(error.what()));
    }
    result.elapsed = timer.elapsed();
    return result;
}

OcrLine OcrEngine::decode(const QVector<float> &data, int steps, const QStringList &characters,
                          QVector<float> *centers) {
    OcrLine line;
    if (steps <= 0 || characters.isEmpty() || qint64(steps) * characters.size() != data.size())
        throw std::runtime_error("Invalid CTC tensor");
    int previous = -1, count = 0;
    QVector<int> first, last, length;   // Step run and QChar count of every emitted token.
    for (int step = 0; step < steps; ++step) {
        const float *row = data.constData() + step * characters.size();
        const int best = int(std::max_element(row, row + characters.size()) - row);
        if (best != 0 && best != previous) {
            line.text += characters[best];
            line.score += row[best];
            ++count;
            first.append(step); last.append(step); length.append(int(characters[best].size()));
        } else if (best != 0 && !last.isEmpty()) {
            last.last() = step;
        }
        previous = best;
    }
    if (count) line.score /= count;
    if (centers) {
        centers->clear();
        for (int i = 0; i < first.size(); ++i)
            for (int c = 0; c < length[i]; ++c) centers->append((first[i] + last[i] + 1) / 2.0f);
    }
    return line;
}

QVector<QPolygonF> OcrEngine::glyphBoxes(const QPolygonF &box, const QVector<float> &centers) {
    if (box.size() != 4 || centers.isEmpty()) return {};
    // Tall boxes read top to bottom (the crop was turned), others left to right.
    const QPointF across = box[1] - box[0], down = box[3] - box[0];
    const bool vertical = std::hypot(down.x(), down.y()) >= 1.5 * std::hypot(across.x(), across.y());
    const QPointF axis = vertical ? down : across, side = vertical ? across : down;
    // Neighbouring characters meet halfway between their centres; the ends mirror the inner half.
    const int n = int(centers.size());
    QVector<double> edge(n + 1);
    for (int i = 1; i < n; ++i) edge[i] = (centers[i-1] + centers[i]) / 2.0;
    edge[0] = n > 1 ? centers[0] - (edge[1] - centers[0]) : 0.0;
    edge[n] = n > 1 ? centers[n-1] + (centers[n-1] - edge[n-1]) : 1.0;
    QVector<QPolygonF> glyphs;
    for (int i = 0; i < n; ++i) {
        const double a = qBound(0.0, edge[i], 1.0), b = qBound(a, edge[i+1], 1.0);
        const QPointF from = box[0] + axis * a, to = box[0] + axis * b;
        glyphs.append(QPolygonF{from, to, to + side, from + side});
    }
    return glyphs;
}

QVector<QPolygonF> OcrEngine::textBoxes(const QVector<float> &prob, const QSize &size, const QSize &original,
                                   const OcrCancel &cancel) {
    if (size.isEmpty() || qint64(size.width()) * size.height() != prob.size() || original.isEmpty())
        throw std::runtime_error("Invalid detection probability map");
    QVector<quint8> visited(prob.size(), 0);
    QVector<QPolygonF> boxes;
    int candidates = 0;
    for (int seed = 0; seed < prob.size(); ++seed) {
        if ((seed & 4095) == 0 && cancel->load()) return {};
        if (visited[seed] || prob[seed] <= 0.3f) continue;
        QVector<int> pending{seed};
        QVector<QPointF> boundary;
        visited[seed] = 1;
        while (!pending.isEmpty()) {
            if ((pending.size() & 4095) == 0 && cancel->load()) return {};
            const int at = pending.takeLast(), x = at % size.width(), y = at / size.width();
            bool edge = false;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= size.width() || ny >= size.height()) { edge = true; continue; }
                const int next = ny * size.width() + nx;
                if (prob[next] <= 0.3f) { edge = true; continue; }
                if (!visited[next]) { visited[next] = 1; pending.append(next); }
            }
            if (edge) boundary.append(QPointF(x, y));
        }
        if (++candidates > 1000) break;
        if (boundary.size() < 3) continue;
        QPolygonF box = minimumBox(boundary);
        double width = QLineF(box[0], box[1]).length(), height = QLineF(box[0], box[3]).length();
        if (qMin(width, height) < 3) continue;
        QPainterPath mask;
        mask.addPolygon(box);
        const QRect bounds = box.boundingRect().toAlignedRect().intersected(QRect(QPoint(), size));
        double score = 0;
        int pixels = 0;
        for (int y = bounds.top(); y <= bounds.bottom(); ++y) for (int x = bounds.left(); x <= bounds.right(); ++x)
            if (mask.contains(QPointF(x, y))) { score += prob[y * size.width() + x]; ++pixels; }
        if (!pixels || score / pixels < 0.6) continue;
        const double distance = width * height * 1.6 / (2 * (width + height));
        const QPointF u = (box[1] - box[0]) / width, v = (box[3] - box[0]) / height;
        box[0] -= (u + v) * distance;
        box[1] += (u - v) * distance;
        box[2] += (u + v) * distance;
        box[3] += (v - u) * distance;
        for (QPointF &point : box) {
            point.setX(qBound(0.0, point.x() / size.width() * original.width(), double(original.width() - 1)));
            point.setY(qBound(0.0, point.y() / size.height() * original.height(), double(original.height() - 1)));
        }
        if (qMin(QLineF(box[0], box[1]).length(), QLineF(box[0], box[3]).length()) > 3) boxes.append(box);
    }
    std::stable_sort(boxes.begin(), boxes.end(), [](const QPolygonF &a, const QPolygonF &b) { return a[0].y() < b[0].y(); });
    return mergeLineBoxes(boxes);
}

QVector<QPolygonF> OcrEngine::mergeLineBoxes(QVector<QPolygonF> boxes) {
    std::stable_sort(boxes.begin(),boxes.end(),[](const QPolygonF &a,const QPolygonF &b) { return a[0].x()<b[0].x(); });
    QVector<QPolygonF> lines;
    for (int i=0; i<boxes.size(); ++i) {
        if (boxes[i].isEmpty()) continue;
        QPolygonF line = boxes[i];
        const QPointF u = (line[1]-line[0])/QLineF(line[0],line[1]).length();
        const QPointF v(-u.y(),u.x());
        const auto project = [&](const QPointF &p) { return QPointF(p.x()*u.x()+p.y()*u.y(),p.x()*v.x()+p.y()*v.y()); };
        const auto bounds = [&](const QPolygonF &box) {
            QPolygonF projected;
            for (const QPointF &point:box) projected.append(project(point));
            return projected.boundingRect();
        };
        QRectF row = bounds(line);
        for (int j=i+1; j<boxes.size(); ++j) {
            if (boxes[j].isEmpty()) continue;
            const QRectF next = bounds(boxes[j]);
            const double height = row.height(), other = next.height();
            if (next.left()<row.right()-height || next.left()-row.right()>height*1.5
                || next.top()<row.top()-height*0.5 || next.bottom()>row.bottom()+height*0.5
                || std::abs(height-other)>qMin(height,other)*0.5) continue;
            row = row.united(next);
            boxes[j].clear();
        }
        line.clear();
        for (const QPointF &p:{row.topLeft(),row.topRight(),row.bottomRight(),row.bottomLeft()})
            line.append(u*p.x()+v*p.y());
        lines.append(line);
    }
    std::stable_sort(lines.begin(),lines.end(),[](const QPolygonF &a,const QPolygonF &b) { return a[0].y()<b[0].y(); });
    return lines;
}

QVector<int> ocrCuts(const QImage &image, int limit)
{
    QVector<int> ends;
    const int H = image.height(), W = image.width();
    const auto uniform = [&](int y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 1; x < W; ++x)
            if (row[x] != row[0]) return false;
        return true;
    };
    int y = 0;
    while (H - y > limit) {
        int cut = y + limit;                    // Fall back to a hard split when no blank row is near the limit.
        for (int r = y + limit - 1; r > y + limit * 2 / 3; --r)
            if (uniform(r)) { cut = r + 1; break; }
        ends.append(cut);
        y = cut;
    }
    ends.append(H);
    return ends;
}

static OcrResult processChunked(const QImage &image, const OcrCancel &cancel, bool detectOnly)
{
    if (image.isNull() || image.height() <= 1440)
        return detectOnly ? OcrEngine::instance().detect(image, cancel)
                          : OcrEngine::instance().recognize(image, cancel);
    QElapsedTimer timer;
    timer.start();
    OcrResult all;
    int top = 0;
    for (int end : ocrCuts(image, 1440)) {
        const QImage chunk = image.copy(0, top, image.width(), end - top);
        const OcrResult part = detectOnly ? OcrEngine::instance().detect(chunk, cancel)
                                          : OcrEngine::instance().recognize(chunk, cancel);
        if (part.cancelled) {
            all.cancelled = true;
            all.lines.clear();
            all.boxes.clear();
            return all;
        }
        if (!part.error.isEmpty()) {
            all.error = part.error;
            return all;
        }
        for (QPolygonF box : part.boxes) {
            box.translate(0, top);
            all.boxes.append(box);
        }
        for (OcrLine line : part.lines) {
            line.box.translate(0, top);
            for (QPolygonF &glyph : line.glyphs) glyph.translate(0, top);
            all.lines.append(line);
        }
        top = end;
    }
    all.elapsed = timer.elapsed();
    return all;
}

OcrResult recognizeChunked(const QImage &image, const OcrCancel &cancel)
{
    return processChunked(image, cancel, false);
}

OcrResult detectChunked(const QImage &image, const OcrCancel &cancel)
{
    return processChunked(image, cancel, true);
}

void OcrEngine::checked(OrtStatus *status) {
    if (!status) return;
    const std::string message = m_api->GetErrorMessage(status);
    m_api->ReleaseStatus(status);
    throw std::runtime_error(message);
}

void OcrEngine::reset() {
    if (m_api) {
        if (m_det) m_api->ReleaseSession(m_det);
        if (m_rec) m_api->ReleaseSession(m_rec);
        if (m_memory) m_api->ReleaseMemoryInfo(m_memory);
        if (m_env) m_api->ReleaseEnv(m_env);
    }
    m_det = m_rec = nullptr; m_memory = nullptr; m_env = nullptr; m_api = nullptr; m_allocator = nullptr;
    if (m_dll) FreeLibrary(m_dll);
    m_dll = nullptr;
    m_characters.clear();
}

void OcrEngine::names(OrtSession *session, QByteArray &input, QByteArray &output) {
    size_t in = 0, out = 0;
    checked(m_api->SessionGetInputCount(session, &in));
    checked(m_api->SessionGetOutputCount(session, &out));
    if (in != 1 || out != 1) throw std::runtime_error("OCR models require one input and one output");
    char *name = nullptr;
    checked(m_api->SessionGetInputName(session, 0, m_allocator, &name));
    input = name; m_allocator->Free(m_allocator, name);
    checked(m_api->SessionGetOutputName(session, 0, m_allocator, &name));
    output = name; m_allocator->Free(m_allocator, name);
}

QVector<float> OcrEngine::run(OrtSession *session, const QByteArray &inputName, const QByteArray &outputName,
                   QVector<float> data, const std::array<int64_t, 4> &dimensions, QVector<int64_t> &shape) {
    OrtValue *raw = nullptr;
    checked(m_api->CreateTensorWithDataAsOrtValue(m_memory, data.data(), size_t(data.size()) * sizeof(float),
                                                 dimensions.data(), dimensions.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &raw));
    std::unique_ptr<OrtValue, decltype(m_api->ReleaseValue)> input(raw, m_api->ReleaseValue);
    const OrtValue *inputs[] = {raw};
    const char *in[] = {inputName.constData()}, *out[] = {outputName.constData()};
    OrtValue *outputRaw = nullptr;
    OrtStatus *status = m_api->Run(session, nullptr, in, inputs, 1, out, 1, &outputRaw);
    std::unique_ptr<OrtValue, decltype(m_api->ReleaseValue)> output(outputRaw, m_api->ReleaseValue);
    checked(status);
    OrtTensorTypeAndShapeInfo *infoRaw = nullptr;
    checked(m_api->GetTensorTypeAndShape(outputRaw, &infoRaw));
    std::unique_ptr<OrtTensorTypeAndShapeInfo, decltype(m_api->ReleaseTensorTypeAndShapeInfo)> info(infoRaw, m_api->ReleaseTensorTypeAndShapeInfo);
    ONNXTensorElementDataType type;
    checked(m_api->GetTensorElementType(infoRaw, &type));
    size_t count = 0, rank = 0;
    checked(m_api->GetTensorShapeElementCount(infoRaw, &count));
    checked(m_api->GetDimensionsCount(infoRaw, &rank));
    if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || !count || count > 64 * 1024 * 1024 || rank > 4)
        throw std::runtime_error("Invalid OCR tensor type or size");
    shape.resize(int(rank));
    checked(m_api->GetDimensions(infoRaw, shape.data(), rank));
    float *values = nullptr;
    checked(m_api->GetTensorMutableData(outputRaw, reinterpret_cast<void **>(&values)));
    QVector<float> result(values, values + count);
    for (float value : result) if (!std::isfinite(value)) throw std::runtime_error("Non-finite OCR output");
    return result;
}

QVector<float> OcrEngine::normalized(const QImage &input, int width) {
    const QImage image = input.convertToFormat(QImage::Format_RGB32);
    if (image.isNull() || width < image.width()) throw std::runtime_error("Invalid OCR input");
    const int plane = width * image.height();
    QVector<float> data(plane * 3, 0);
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int bgr[] = {qBlue(row[x]), qGreen(row[x]), qRed(row[x])};
            for (int channel = 0; channel < 3; ++channel)
                data[channel * plane + y * width + x] = bgr[channel] / 127.5f - 1.0f;
        }
    }
    return data;
}

QPolygonF OcrEngine::minimumBox(QVector<QPointF> points) {
    const auto cross = [](const QPointF &a, const QPointF &b, const QPointF &c) {
        return (b.x()-a.x())*(c.y()-a.y()) - (b.y()-a.y())*(c.x()-a.x());
    };
    std::sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) {
        return a.x() == b.x() ? a.y() < b.y() : a.x() < b.x();
    });
    QVector<QPointF> hull;
    for (const QPointF &p : points) {
        while (hull.size() > 1 && cross(hull[hull.size()-2], hull.last(), p) <= 0) hull.removeLast();
        hull.append(p);
    }
    const int lower = int(hull.size());
    for (int i = int(points.size()) - 2; i >= 0; --i) {
        while (hull.size() > lower && cross(hull[hull.size()-2], hull.last(), points[i]) <= 0) hull.removeLast();
        hull.append(points[i]);
    }
    hull.removeLast();
    double best = std::numeric_limits<double>::max(), angle = 0;
    for (int i = 0; i < hull.size(); ++i) {
        const QPointF edge = hull[(i+1) % hull.size()] - hull[i];
        const double a = std::atan2(edge.y(), edge.x()), c = std::cos(a), s = std::sin(a);
        double left = 1e30, top = 1e30, right = -1e30, bottom = -1e30;
        for (const QPointF &p : hull) {
            const double x = p.x()*c + p.y()*s, y = -p.x()*s + p.y()*c;
            left = qMin(left,x); right = qMax(right,x); top = qMin(top,y); bottom = qMax(bottom,y);
        }
        if ((right-left)*(bottom-top) < best) { best = (right-left)*(bottom-top); angle = a; }
    }
    while (angle > M_PI/4) angle -= M_PI/2;
    while (angle <= -M_PI/4) angle += M_PI/2;
    if (std::abs(angle) <= qDegreesToRadians(3.0)) angle = 0;
    const double c = std::cos(angle), s = std::sin(angle);
    double left = 1e30, top = 1e30, right = -1e30, bottom = -1e30;
    for (const QPointF &p : hull) {
        const double x = p.x()*c + p.y()*s, y = -p.x()*s + p.y()*c;
        left = qMin(left,x); right = qMax(right,x); top = qMin(top,y); bottom = qMax(bottom,y);
    }
    QPolygonF result;
    for (const QPointF &p : {QPointF(left,top), QPointF(right,top), QPointF(right,bottom), QPointF(left,bottom)})
        result.append(QPointF(p.x()*c-p.y()*s, p.x()*s+p.y()*c));
    return result;
}

QImage OcrEngine::lineImage(const QImage &image, const QPolygonF &box) {
    const int width = qMax(1, qRound(QLineF(box[0], box[1]).length()));
    const int height = qMax(1, qRound(QLineF(box[0], box[3]).length()));
    QPolygonF target{QPointF(0,0), QPointF(width,0), QPointF(width,height), QPointF(0,height)};
    QTransform transform;
    if (!QTransform::quadToQuad(box, target, transform)) return {};
    QImage crop(width, height, QImage::Format_RGB32);
    if (crop.isNull()) return {};
    crop.fill(Qt::white);
    QPainter painter(&crop);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setTransform(transform);
    painter.drawImage(QPoint(), image);
    painter.end();
    if (height >= width * 1.5) crop = crop.transformed(QTransform().rotate(-90));
    return crop;
}
