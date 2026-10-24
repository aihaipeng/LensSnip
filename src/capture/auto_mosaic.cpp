#include "capture/auto_mosaic.h"
#include "capture/capture_overlay.h"
#include "shared/settings.h"
#include "text/engines/barcode_engine.h"
#include "text/engines/ocr_engine.h"
#include <QFutureWatcher>
#include <QLinearGradient>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QtConcurrent>
#include <algorithm>

namespace {
constexpr int kAutoRevealMs = 2000;
constexpr int kAutoScanHalfMs = 1050;

const QRegularExpression &urlPattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"(\b(?:https?://|www\.)\S+)"),
                                            QRegularExpression::CaseInsensitiveOption);
    return pattern;
}
const QRegularExpression &emailPattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"(\b[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}\b)"));
    return pattern;
}
const QRegularExpression &phonePattern()
{
    // CN mobile numbers, with an optional +86/0086 prefix; separators only before
    // the subscriber digits, matching how chat lists print them.
    static const QRegularExpression pattern(QStringLiteral(R"((?:(?:\+|00)?86[\s\-]?)?1[3-9]\d{9})"));
    return pattern;
}

QString phoneKey(const QString &text)
{
    QString digits;
    for (const QChar c : text) if (c.isDigit()) digits.append(c);
    if (digits.startsWith(QStringLiteral("0086"))) digits.remove(0, 4);
    else if (digits.startsWith(QStringLiteral("86")) && digits.size() > 11) digits.remove(0, 2);
    return QStringLiteral("phone:") + digits;
}

QString keyFor(AutoMosaic::Kind kind, const QString &text)
{
    switch (kind) {
    case AutoMosaic::Kind::Email: return QStringLiteral("email:") + text.trimmed().toLower();
    case AutoMosaic::Kind::Url: return QStringLiteral("url:") + text.trimmed().toLower();
    case AutoMosaic::Kind::Phone: return phoneKey(text);
    case AutoMosaic::Kind::Qr: return QStringLiteral("qr:") + text.trimmed();
    default: return QString();
    }
}

bool overlapsMostly(const QRect &a, const QRect &b)
{
    const QRect both = a.intersected(b);
    if (both.isEmpty()) return false;
    const qint64 smaller = qMin(qint64(a.width()) * a.height(), qint64(b.width()) * b.height());
    return smaller > 0 && qint64(both.width()) * both.height() * 10 >= smaller * 7;
}

qint64 boxArea(const QRect &rect)
{
    return qint64(rect.width()) * rect.height();
}

// One detection box plus the tile that produced it. Seams between overlapping
// tiles clip a line into half boxes; the merge below keeps the complete box or
// unites two complementary halves before recognition.
struct Candidate {
    QRect rect;
    QPolygonF box;
    int tile = -1;
    int pieces = 1;
    bool top = false, bottom = false, left = false, right = false;
    bool clipped() const { return top || bottom || left || right; }
};

bool sameRow(const QRect &a, const QRect &b)
{
    const int overlap = qMin(a.bottom(), b.bottom()) - qMax(a.top(), b.top()) + 1;
    const int shorter = qMin(a.height(), b.height()), taller = qMax(a.height(), b.height());
    return shorter > 0 && overlap * 5 >= shorter * 3 && taller <= shorter * 2;
}

bool sameColumn(const QRect &a, const QRect &b)
{
    const int overlap = qMin(a.right(), b.right()) - qMax(a.left(), b.left()) + 1;
    const int shorter = qMin(a.width(), b.width()), wider = qMax(a.width(), b.width());
    return shorter > 0 && overlap * 5 >= shorter * 3 && wider <= shorter * 2;
}

bool uniteHalves(const Candidate &a, const Candidate &b, const QVector<QRect> &tiles, QRect *united)
{
    if (a.tile < 0 || b.tile < 0 || a.tile == b.tile || a.pieces + b.pieces > 3) return false;
    if (!tiles[a.tile].intersects(tiles[b.tile])) return false;
    const bool horizontal = (a.left && b.right) || (b.left && a.right);
    const bool vertical = (a.top && b.bottom) || (b.top && a.bottom);
    if (!horizontal && !vertical) return false;
    if (horizontal && !sameRow(a.rect, b.rect)) return false;
    if (vertical && !sameColumn(a.rect, b.rect)) return false;
    if (a.rect.intersected(b.rect).isEmpty()) return false;
    const QRect both = a.rect.united(b.rect);
    if (boxArea(both) > (boxArea(a.rect) + boxArea(b.rect)) * 2) return false;
    *united = both;
    return true;
}

QVector<Candidate> mergeCandidates(QVector<Candidate> candidates, const QVector<QRect> &tiles)
{
    // Complete boxes win over boxes trimmed by an artificial tile seam; ties keep
    // the larger box, then the earlier tile for deterministic output.
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (a.clipped() != b.clipped()) return !a.clipped();
        const qint64 left = boxArea(a.rect), right = boxArea(b.rect);
        if (left != right) return left > right;
        return a.tile < b.tile;
    });
    QVector<Candidate> merged;
    for (Candidate next : candidates) {
        bool duplicate = false;
        for (Candidate &kept : merged) {
            QRect united;
            if (uniteHalves(kept, next, tiles, &united)) {
                kept.rect = united;
                kept.box = QPolygonF(QRectF(united));
                kept.top = kept.top || next.top;
                kept.bottom = kept.bottom || next.bottom;
                kept.left = kept.left || next.left;
                kept.right = kept.right || next.right;
                kept.pieces += next.pieces;
                duplicate = true;
                break;
            }
            const QRect both = kept.rect.intersected(next.rect);
            if (both.isEmpty()) continue;
            const qint64 smaller = qMin(boxArea(kept.rect), boxArea(next.rect));
            if (boxArea(both) * 10 < smaller * 8) continue;   // Different lines that only graze.
            duplicate = true;
            break;
        }
        if (!duplicate) merged.append(next);
    }
    return merged;
}

QRect paddedBox(const QRect &rect, const QRect &bounds, int pad)
{
    return rect.adjusted(-pad, -pad, pad, pad).intersected(bounds);
}

int textPadding(const QRect &rect)
{
    return qMax(2, qRound(rect.height() * 0.08));
}

quint64 imageHash(const QImage &image, const QRect &rect)
{
    const QImage block = image.copy(rect).scaled(8, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    if (block.isNull()) return 0;
    int sum = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) sum += block.constScanLine(y)[x];
    const int average = sum / 64;
    quint64 bits = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            if (block.constScanLine(y)[x] >= average) bits |= quint64(1) << (y * 8 + x);
    return bits;
}

// Non-text visual elements (avatars, icons, colored blocks). Connected components
// against the dominant background are enough for flat UI screenshots; photos and
// full-size images are left to the manual drag.
// shortcut: components touching an edge or larger than 240 px are skipped.
QVector<AutoMosaic::Detected> imageCandidates(const QImage &image,
                                              const QVector<AutoMosaic::Detected> &known)
{
    QVector<AutoMosaic::Detected> result;
    if (image.isNull() || image.width() < 16 || image.height() < 16) return result;
    const QSize target = image.size().scaled(QSize(1600, 1600), Qt::KeepAspectRatio);
    const QImage small = (target == image.size() ? image
                                                 : image.scaled(target, Qt::IgnoreAspectRatio, Qt::FastTransformation))
                             .convertToFormat(QImage::Format_RGB32);
    const int W = small.width(), H = small.height();
    if (W < 16 || H < 16) return result;

    QHash<quint32, int> histogram;
    const int sample = qMax(1, qMin(W, H) / 200);
    for (int y = 0; y < H; y += sample)
        for (int x = 0; x < W; x += sample) {
            const QRgb p = small.pixel(x, y);
            const quint32 key = quint32(qRed(p) >> 4) << 8 | quint32(qGreen(p) >> 4) << 4
                | quint32(qBlue(p) >> 4);
            ++histogram[key];
        }
    quint32 dominant = 0;
    int best = -1;
    for (auto it = histogram.cbegin(); it != histogram.cend(); ++it)
        if (it.value() > best) { best = it.value(); dominant = it.key(); }
    const int bgR = int((dominant >> 8) & 0xF) * 17;
    const int bgG = int((dominant >> 4) & 0xF) * 17;
    const int bgB = int(dominant & 0xF) * 17;

    QVector<quint8> foreground(W * H, 0), seen(W * H, 0);
    for (int y = 0; y < H; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(small.constScanLine(y));
        for (int x = 0; x < W; ++x) {
            const QRgb p = row[x];
            const int diff = qAbs(qRed(p) - bgR) + qAbs(qGreen(p) - bgG) + qAbs(qBlue(p) - bgB);
            if (qAlpha(p) > 40 && diff > 60) foreground[y * W + x] = 1;
        }
    }

    const double sx = image.width() / double(W), sy = image.height() / double(H);
    QVector<int> stack;
    for (int index = 0; index < W * H; ++index) {
        if (!foreground[index] || seen[index]) continue;
        stack.clear();
        stack.append(index);
        seen[index] = 1;
        int minX = W, minY = H, maxX = -1, maxY = -1, count = 0;
        while (!stack.isEmpty()) {
            const int at = stack.takeLast();
            const int x = at % W, y = at / W;
            ++count;
            minX = qMin(minX, x); maxX = qMax(maxX, x);
            minY = qMin(minY, y); maxY = qMax(maxY, y);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    const int next = ny * W + nx;
                    if (foreground[next] && !seen[next]) {
                        seen[next] = 1;
                        stack.append(next);
                    }
                }
        }
        const int w = maxX - minX + 1, h = maxY - minY + 1;
        if (w < 12 || h < 12 || w > 240 || h > 240) continue;
        const double aspect = w / double(h);
        if (aspect < 0.4 || aspect > 2.5) continue;
        if (count < w * h * 0.2) continue;                 // Thin borders and hairlines.
        if (minX <= 0 || minY <= 0 || maxX >= W - 1 || maxY >= H - 1) continue;
        const QRect box(qRound(minX * sx), qRound(minY * sy),
                        qRound(w * sx), qRound(h * sy));
        if (box.width() < 8 || box.height() < 8) continue;
        bool covered = false;
        for (const AutoMosaic::Detected &other : known) {
            const QRect both = other.rect.intersected(box);
            if (!both.isEmpty() && qint64(both.width()) * both.height() * 2
                    >= qint64(box.width()) * box.height())
                { covered = true; break; }
        }
        if (covered) continue;
        bool duplicate = false;
        for (const AutoMosaic::Detected &other : result)
            if (overlapsMostly(other.rect, box)) { duplicate = true; break; }
        if (duplicate) continue;
        result.append({box, QStringLiteral("image:") + QString::number(imageHash(image, box), 16),
                       AutoMosaic::Kind::None});
        if (result.size() >= 100) break;
    }
    return result;
}
}

AutoMosaic::Kind AutoMosaic::classify(const QString &text)
{
    if (text.trimmed().isEmpty()) return Kind::None;
    if (urlPattern().match(text).hasMatch()) return Kind::Url;
    if (emailPattern().match(text).hasMatch()) return Kind::Email;
    if (phonePattern().match(text).hasMatch()) return Kind::Phone;
    return Kind::None;
}

AutoMosaic::Result AutoMosaic::detect(const QImage &image, const std::shared_ptr<std::atomic_bool> &cancel)
{
    Result result;
    if (image.isNull() || (cancel && cancel->load())) return result;
    const QRect bounds = image.rect();
    // The OCR engine dereferences its cancel token unconditionally.
    const auto activeCancel = cancel ? cancel : std::make_shared<std::atomic_bool>(false);

    // Text identifiers. A busy OCR card owns the engine; the QR pass still runs.
    // large captures are tiled so the detector keeps text at native size instead
    // of shrinking a 4K window to the 1440 px limit. Boxes from every tile are
    // merged in full-image coordinates before recognition.
    auto &engine = OcrEngine::instance();
    bool expected = false;
    if (engine.busy.compare_exchange_strong(expected, true)) {
        constexpr int maxTile = 1400, overlap = 320;
        const bool single = image.width() <= maxTile
            || qMax(image.width(), image.height()) <= 2000;
        QVector<QRect> tiles;
        if (single) {
            // A narrow tall capture is chunked at blank rows, never at an x seam.
            tiles.append(bounds);
        } else {
            for (int y = 0; y < image.height();) {
                const int h = qMin(maxTile, image.height() - y);
                for (int x = 0; x < image.width();) {
                    const int w = qMin(maxTile, image.width() - x);
                    tiles.append(QRect(x, y, w, h));
                    if (x + w >= image.width()) break;
                    x += maxTile - overlap;
                }
                if (y + h >= image.height()) break;
                y += maxTile - overlap;
            }
        }
        QVector<Candidate> candidates;
        for (int t = 0; t < tiles.size() && !activeCancel->load(); ++t) {
            const QRect tile = tiles[t];
            QImage part = image.copy(tile);
            part.setDevicePixelRatio(1);
            try {
                const OcrResult ocr = detectChunked(part, activeCancel);
                if (ocr.cancelled) break;
                if (!ocr.error.isEmpty()) { result.error = ocr.error; break; }
                for (const QPolygonF &box : ocr.boxes) {
                    const QRect local = box.boundingRect().toAlignedRect();
                    if (local.width() < 6 || local.height() < 6) continue;
                    Candidate candidate;
                    candidate.rect = local.translated(tile.topLeft()).intersected(bounds);
                    if (candidate.rect.width() < 6 || candidate.rect.height() < 6) continue;
                    candidate.box = box;
                    candidate.box.translate(tile.topLeft());
                    candidate.tile = single ? -1 : t;
                    candidate.top = tile.top() > bounds.top() && local.top() <= 2;
                    candidate.bottom = tile.bottom() < bounds.bottom()
                        && local.bottom() >= tile.height() - 3;
                    candidate.left = tile.left() > bounds.left() && local.left() <= 2;
                    candidate.right = tile.right() < bounds.right()
                        && local.right() >= tile.width() - 3;
                    candidates.append(candidate);
                }
            } catch (const std::exception &error) {
                result.error = QString::fromUtf8(error.what());
                break;
            } catch (...) {
                result.error = QStringLiteral("OCR worker failed");
                break;
            }
        }
        if (!activeCancel->load() && result.error.isEmpty() && !candidates.isEmpty()) {
            const QVector<Candidate> merged = mergeCandidates(candidates, tiles);
            QVector<QPolygonF> boxes;
            boxes.reserve(merged.size());
            for (const Candidate &candidate : merged) boxes.append(candidate.box);
            const OcrResult recognized = OcrEngine::instance().recognizeBoxes(image, boxes, activeCancel);
            if (!recognized.cancelled) {
                for (int i = 0; i < merged.size(); ++i) {
                    const QRect box = paddedBox(merged[i].rect, bounds, textPadding(merged[i].rect));
                    if (box.width() < 6 || box.height() < 6) continue;
                    const QString text = i < recognized.lines.size()
                        ? recognized.lines[i].text.trimmed() : QString();
                    // Unreadable detections inside one tile are usually texture
                    // false positives; keep them only when a seam clipped them.
                    if (text.isEmpty() && !merged[i].clipped()) continue;
                    const Kind kind = classify(text);
                    const QString key = kind == Kind::None
                        ? (text.isEmpty() ? QString() : QStringLiteral("text:") + text.toLower())
                        : keyFor(kind, text);
                    result.blocks.append({box, key, kind});
                }
                if (result.error.isEmpty() && !recognized.error.isEmpty())
                    result.error = recognized.error;
            }
        }
        engine.busy.store(false);
    } else {
        result.error = QStringLiteral("OCR busy");
    }
    if (cancel && cancel->load()) return result;

    // QR / matrix / 1D symbols. These are already privacy-relevant on their own.
    const BarcodeResult codes = BarcodeEngine::recognize(image, activeCancel);
    for (const BarcodeItem &item : codes.items) {
        QRect box = item.box.toAlignedRect().intersected(bounds);
        if (box.width() < 6 || box.height() < 6) continue;
        box = paddedBox(box, bounds, qMax(2, qRound(qMax(box.width(), box.height()) * 0.08)));
        const QString key = keyFor(Kind::Qr, item.text);
        bool duplicate = false;
        for (const Detected &seen : result.blocks)
            if (overlapsMostly(seen.rect, box)) { duplicate = true; break; }
        if (!duplicate) result.blocks.append({box, key, Kind::Qr});
    }

    // Icons, avatars and other non-text blocks. Text/QR boxes already present win.
    for (const Detected &image : imageCandidates(image, result.blocks)) {
        bool duplicate = false;
        for (const Detected &seen : result.blocks)
            if (overlapsMostly(seen.rect, image.rect)) { duplicate = true; break; }
        if (!duplicate) result.blocks.append(image);
    }

    std::sort(result.blocks.begin(), result.blocks.end(), [](const Detected &a, const Detected &b) {
        return a.rect.y() != b.rect.y() ? a.rect.y() < b.rect.y() : a.rect.x() < b.rect.x();
    });
    if (result.blocks.size() > 200) result.blocks.resize(200);
    return result;
}

bool AutoMosaic::testMerging()
{
    const auto make = [](const QRect &rect, int tile, bool top = false, bool bottom = false,
                         bool left = false, bool right = false) {
        Candidate candidate;
        candidate.rect = rect;
        candidate.box = QPolygonF(QRectF(rect));
        candidate.tile = tile;
        candidate.top = top;
        candidate.bottom = bottom;
        candidate.left = left;
        candidate.right = right;
        return candidate;
    };
    const QVector<QRect> rows{QRect(0, 0, 1400, 1400), QRect(1080, 1080, 1320, 1320)};
    const QVector<QRect> columns{QRect(0, 0, 1400, 1400), QRect(1080, 0, 1320, 1400)};

    // A complete box must beat the half box clipped by the horizontal seam.
    const QRect wholeRect(1100, 1385, 300, 30);
    const Candidate half = make(QRect(1100, 1385, 300, 15), 0, false, true);
    const Candidate whole = make(wholeRect, 1);
    QVector<Candidate> merged = mergeCandidates({half, whole}, rows);
    if (merged.size() != 1 || merged.first().rect != wholeRect) return false;
    merged = mergeCandidates({whole, half}, rows);
    if (merged.size() != 1 || merged.first().rect != wholeRect) return false;

    // Two halves of one long line are united instead of dropping either side.
    const Candidate left = make(QRect(200, 100, 1200, 30), 0, false, false, false, true);
    const Candidate right = make(QRect(1080, 100, 700, 30), 1, false, false, true, false);
    const QRect united(200, 100, 1580, 30);
    merged = mergeCandidates({left, right}, columns);
    if (merged.size() != 1 || merged.first().rect != united) return false;
    merged = mergeCandidates({right, left}, columns);
    if (merged.size() != 1 || merged.first().rect != united) return false;

    // Adjacent lines never unite, even when each touches a seam.
    merged = mergeCandidates({make(QRect(200, 100, 1200, 30), 0, false, false, false, true),
                              make(QRect(1080, 120, 700, 30), 1, false, false, true, false)},
                             columns);
    if (merged.size() != 2) return false;

    // Dense but separate rows stay separate candidates.
    merged = mergeCandidates({make(QRect(100, 100, 300, 30), 0),
                              make(QRect(100, 140, 300, 30), 0)},
                             columns);
    if (merged.size() != 2) return false;
    return true;
}

void CaptureOverlay::selectAutoMosaic()
{
    if (m_live) { m_live = false; commitAnnotation(); }
    selectTool(ATool::Mosaic);              // Clears any previous auto state and rebuilds the regular bar.
    m_autoMosaic = true;
    m_mosaicBrush = false;
    m_autoHoverAnn = -1;
    m_autoHoverBlock = -1;
    syncToolButtons();
    updateToolOptions();
    // Switching tools keeps this session's candidates; rescanning the same
    // selection would only repeat the same work and the same scan pass.
    const QRect selection = m_sel.normalized().intersected(QRect(0, 0, width(), height()));
    if (!m_autoDetectedRect.isNull() && selection == m_autoDetectedRect) {
        m_autoReveal = true;
        const int epoch = m_autoEpoch;
        QTimer::singleShot(kAutoRevealMs, this, [this, epoch] {
            if (epoch != m_autoEpoch) return;
            m_autoReveal = false;
            update();
        });
    } else {
        startAutoMosaicDetection();
    }
    update();
}

void CaptureOverlay::startAutoMosaicDetection()
{
    if (!m_autoMosaic || m_mode != Adjusting || m_sel.isNull() || m_shot.isNull()) return;
    if (m_autoWatcher && m_autoWatcher->isRunning()) return;
    ++m_autoEpoch;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_autoCancel = cancel;
    m_autoPending = true;
    m_autoReveal = false;
    m_autoScanElapsed.start();
    m_autoSpinTimer.start();
    update();

    const qreal dpr = m_shot.devicePixelRatioF() > 0 ? m_shot.devicePixelRatioF() : 1.0;
    const QRect selection = m_sel.normalized().intersected(QRect(0, 0, width(), height()));
    const QImage shot = m_shot.toImage();
    const QRect native = QRect(qRound(selection.left() * dpr), qRound(selection.top() * dpr),
                               qRound(selection.width() * dpr), qRound(selection.height() * dpr))
                             .intersected(shot.rect());
    QImage image = shot.copy(native);
    image.setDevicePixelRatio(1);
    if (image.isNull() || image.width() < 8 || image.height() < 8) {
        m_autoPending = false;
        m_autoReveal = false;
        m_autoSpinTimer.stop();
        update();
        return;
    }
    const int epoch = m_autoEpoch;
    auto *watcher = new QFutureWatcher<AutoMosaic::Result>(this);
    m_autoWatcher = watcher;
    connect(watcher, &QFutureWatcher<AutoMosaic::Result>::finished, this,
            [this, watcher, epoch, selection, dpr] {
        const AutoMosaic::Result result = watcher->result();
        watcher->deleteLater();
        if (m_autoWatcher == watcher) m_autoWatcher = nullptr;
        if (epoch != m_autoEpoch) return;
        m_autoPending = false;
        m_autoSpinTimer.stop();
        m_autoDetectedRect = selection;
        applyAutoMosaicResult(result, selection, dpr);
        update();
    });
    watcher->setFuture(QtConcurrent::run([image, cancel] { return AutoMosaic::detect(image, cancel); }));
}

void CaptureOverlay::cancelAutoMosaicDetection()
{
    ++m_autoEpoch;
    if (m_autoCancel) m_autoCancel->store(true);
    m_autoCancel.reset();
    m_autoPending = false;
    m_autoReveal = false;
    m_autoSpinTimer.stop();
    if (m_autoWatcher) {
        m_autoWatcher->disconnect(this);
        m_autoWatcher->deleteLater();
        m_autoWatcher = nullptr;
    }
}

CaptureOverlay::Annotation *CaptureOverlay::autoMosaicAnnotation()
{
    for (int i = m_anns.size() - 1; i >= 0; --i)
        if (m_anns[i].autoMosaic) return &m_anns[i];
    return nullptr;
}

const CaptureOverlay::Annotation *CaptureOverlay::autoMosaicAnnotation() const
{
    for (int i = m_anns.size() - 1; i >= 0; --i)
        if (m_anns[i].autoMosaic) return &m_anns[i];
    return nullptr;
}

void CaptureOverlay::applyAutoMosaicResult(const AutoMosaic::Result &result, const QRect &selection, qreal dpr)
{
    if (result.blocks.isEmpty() || dpr <= 0) return;
    Annotation *ann = autoMosaicAnnotation();
    QVector<Annotation::AutoBlock> fresh;
    for (const AutoMosaic::Detected &detected : result.blocks) {
        QRect logical(qRound(selection.left() + detected.rect.left() / dpr),
                      qRound(selection.top() + detected.rect.top() / dpr),
                      qRound(detected.rect.width() / dpr), qRound(detected.rect.height() / dpr));
        logical = logical.normalized().intersected(selection);
        if (logical.width() < 4 || logical.height() < 4) continue;
        const auto covered = [&logical](const QVector<Annotation::AutoBlock> &blocks) {
            for (const auto &block : blocks) {
                const QRect both = block.rect.intersected(logical);
                if (!both.isEmpty() && qint64(both.width()) * both.height() * 10
                        >= qint64(qMin(block.rect.width() * block.rect.height(),
                                       logical.width() * logical.height())) * 7) return true;
            }
            return false;
        };
        if (ann && covered(ann->autoBlocks)) continue;
        // Detection only proposes candidates; the user clicks a block to cover it.
        fresh.append({logical, detected.key, false});
    }
    if (fresh.isEmpty()) return;

    pushUndo();
    if (!ann) {
        Annotation created;
        created.tool = ATool::Mosaic;
        created.autoMosaic = true;
        created.mosaicBlur = m_autoMosaicBlur;
        created.mosaicStrength = m_autoMosaicStrength;
        created.mosaicLevels[0] = m_autoMosaicLevels[0];
        created.mosaicLevels[1] = m_autoMosaicLevels[1];
        created.mosaicSeed = QRandomGenerator::global()->generate();
        m_anns.append(created);
        ann = &m_anns.last();
    }
    ann->autoBlocks += fresh;
    QRect bounds;
    for (const auto &block : ann->autoBlocks) bounds = bounds.united(block.rect);
    ann->rect = bounds;
    m_selAnn = -1;                          // Blocks are toggled, never moved by the regular handles.
    placeToolBars();
    m_autoReveal = true;
    const int epoch = m_autoEpoch;
    QTimer::singleShot(kAutoRevealMs, this, [this, epoch] {
        if (epoch != m_autoEpoch) return;
        m_autoReveal = false;
        update();
    });
    update();
}

bool CaptureOverlay::autoMosaicAt(const QPoint &pos, int *annIndex, int *blockIndex) const
{
    for (int i = m_anns.size() - 1; i >= 0; --i) {
        const Annotation &ann = m_anns[i];
        if (!ann.autoMosaic) continue;
        for (int b = ann.autoBlocks.size() - 1; b >= 0; --b) {
            if (ann.autoBlocks[b].rect.adjusted(-2, -2, 2, 2).contains(pos)) {
                if (annIndex) *annIndex = i;
                if (blockIndex) *blockIndex = b;
                return true;
            }
        }
    }
    return false;
}

void CaptureOverlay::toggleAutoMosaicBlock(int annIndex, int blockIndex)
{
    if (annIndex < 0 || annIndex >= m_anns.size()) return;
    Annotation &ann = m_anns[annIndex];
    if (!ann.autoMosaic || blockIndex < 0 || blockIndex >= ann.autoBlocks.size()) return;
    pushUndo();
    const bool included = !ann.autoBlocks[blockIndex].included;
    const QString key = ann.autoBlocks[blockIndex].key;
    QRect dirty;
    if (m_autoMosaicSync && !key.isEmpty()) {
        for (auto &block : ann.autoBlocks)
            if (block.key == key) {
                block.included = included;
                dirty |= block.rect;
            }
    } else {
        ann.autoBlocks[blockIndex].included = included;
        dirty = ann.autoBlocks[blockIndex].rect;
    }
    if (!dirty.isNull()) update(dirty.adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::clearAutoMosaicHover()
{
    QRect dirty;
    if (m_autoHoverAnn >= 0 && m_autoHoverAnn < m_anns.size()
        && m_autoHoverBlock >= 0
        && m_autoHoverBlock < m_anns[m_autoHoverAnn].autoBlocks.size())
        dirty = m_anns[m_autoHoverAnn].autoBlocks[m_autoHoverBlock].rect;
    m_autoHoverAnn = -1;
    m_autoHoverBlock = -1;
    if (!dirty.isNull()) update(dirty.adjusted(-6, -6, 6, 6));
}

void CaptureOverlay::addAutoMosaicBlock(const QRect &rect)
{
    const QRect block = rect.normalized().intersected(m_sel);
    if (block.width() < 4 || block.height() < 4) return;
    Annotation *ann = autoMosaicAnnotation();
    if (ann) {
        QVector<int> textHits;
        bool covered = false;
        for (int i = 0; i < ann->autoBlocks.size(); ++i) {
            const auto &existing = ann->autoBlocks[i];
            const QRect both = existing.rect.intersected(block);
            const bool hit = existing.rect.contains(block.center()) || !both.isEmpty();
            if (!hit) continue;
            if (existing.included) { covered = true; continue; }
            textHits.append(i);
        }
        if (!textHits.isEmpty()) {
            // A box over detected text toggles those text blocks instead of
            // painting one solid rectangle across the background.
            pushUndo();
            QRect dirty;
            for (int i : textHits) {
                ann->autoBlocks[i].included = true;
                dirty |= ann->autoBlocks[i].rect;
            }
            if (!dirty.isNull()) update(dirty.adjusted(-4, -4, 4, 4));
            return;
        }
        if (covered) return;   // The box only repeats an already covered area.
    }
    pushUndo();
    if (!ann) {
        Annotation created;
        created.tool = ATool::Mosaic;
        created.autoMosaic = true;
        created.mosaicBlur = m_autoMosaicBlur;
        created.mosaicStrength = m_autoMosaicStrength;
        created.mosaicLevels[0] = m_autoMosaicLevels[0];
        created.mosaicLevels[1] = m_autoMosaicLevels[1];
        created.mosaicSeed = QRandomGenerator::global()->generate();
        m_anns.append(created);
        ann = &m_anns.last();
    }
    ann->autoBlocks.append({block, QString(), true});
    ann->rect = ann->rect.united(block);
    m_selAnn = -1;
    placeToolBars();
    update(block.adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::setAutoMosaicMode(bool blur)
{
    if (m_autoMosaicBlur == blur) return;
    m_autoMosaicLevels[m_autoMosaicBlur ? 1 : 0] = m_autoMosaicStrength;
    m_autoMosaicBlur = blur;
    m_autoMosaicStrength = m_autoMosaicLevels[blur ? 1 : 0];
    Annotation *ann = autoMosaicAnnotation();
    QRect dirty;
    if (ann) {
        pushUndo();
        ann->mosaicBlur = blur;
        ann->mosaicStrength = m_autoMosaicStrength;
        ann->mosaicCacheStrength = 0;
        for (const auto &block : ann->autoBlocks)
            if (block.included) dirty |= block.rect;
    }
    if (m_autoStrengthSlider) {
        const QSignalBlocker lock(m_autoStrengthSlider);   // The combo is mid-click; do not rebuild the strip.
        m_autoStrengthSlider->setValue(m_autoMosaicStrength);
    }
    LensSnipSettings settings;
    settings.setValue(QStringLiteral("Capture/AutoMosaicBlur"), blur);
    settings.setValue(QStringLiteral("Capture/AutoMosaicStrength"), m_autoMosaicLevels[0]);
    settings.setValue(QStringLiteral("Capture/AutoMosaicBlurStrength"), m_autoMosaicLevels[1]);
    if (!dirty.isNull()) update(dirty.adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::setAutoMosaicStrength(int value)
{
    value = qBound(3, value, 24);
    m_autoMosaicLevels[m_autoMosaicBlur ? 1 : 0] = value;
    if (value == m_autoMosaicStrength) return;
    m_autoMosaicStrength = value;
    QRect dirty;
    Annotation *ann = autoMosaicAnnotation();
    if (ann) {
        const bool dragging = m_autoStrengthSlider && m_autoStrengthSlider->isSliderDown();
        if (!dragging || !m_autoDragUndo) pushUndo();
        m_autoDragUndo = dragging;
        ann->mosaicStrength = value;
        ann->mosaicSeed = QRandomGenerator::global()->generate();
        ann->mosaicCacheStrength = 0;
        for (const auto &block : ann->autoBlocks)
            if (block.included) dirty |= block.rect;
    }
    if (m_autoStrengthSlider) {
        const QSignalBlocker lock(m_autoStrengthSlider);
        m_autoStrengthSlider->setValue(value);
    }
    LensSnipSettings settings;
    settings.setValue(QStringLiteral("Capture/AutoMosaicStrength"), m_autoMosaicLevels[0]);
    settings.setValue(QStringLiteral("Capture/AutoMosaicBlurStrength"), m_autoMosaicLevels[1]);
    if (!dirty.isNull()) update(dirty.adjusted(-4, -4, 4, 4));
}

void CaptureOverlay::paintAutoMosaicOverlay(QPainter &p) const
{
    if (!m_autoMosaic || m_atool != ATool::Mosaic || m_mode != Adjusting) return;
    const Annotation *autoAnn = autoMosaicAnnotation();
    if (!autoAnn) return;

    // The result is shown as one batch for a short moment: the user sees the
    // whole candidate set, then only the hovered frame remains visible.
    if (m_autoReveal) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        for (const Annotation::AutoBlock &block : autoAnn->autoBlocks) {
            const QRect r = block.rect.normalized();
            if (r.isEmpty()) continue;
            p.fillRect(r, QColor(94, 106, 210, 24));
            p.setPen(QPen(QColor(143, 155, 255, 210), 1.2, Qt::SolidLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5));
        }
        p.restore();
    }

    if (m_autoHoverAnn < 0 || m_autoHoverAnn >= m_anns.size()) return;
    const Annotation &ann = m_anns[m_autoHoverAnn];
    if (!ann.autoMosaic || m_autoHoverBlock < 0 || m_autoHoverBlock >= ann.autoBlocks.size()) return;
    const Annotation::AutoBlock &block = ann.autoBlocks[m_autoHoverBlock];
    const QRect r = block.rect.normalized();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(r, block.included ? QColor(80, 190, 120, 70) : QColor(229, 57, 53, 55));
    const QColor edge = block.included ? QColor(96, 220, 140) : QColor(255, 110, 120);
    p.setPen(QPen(edge, 1.6, Qt::SolidLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(r).adjusted(0, 0, -1, -1));
    if (r.width() >= 10 && r.height() >= 10) {
        const int tick = qMin(6, qMin(r.width(), r.height()) / 3);
        p.drawLine(r.topLeft(), r.topLeft() + QPoint(tick, 0));
        p.drawLine(r.topLeft(), r.topLeft() + QPoint(0, tick));
        p.drawLine(r.topRight(), r.topRight() + QPoint(-tick, 0));
        p.drawLine(r.topRight(), r.topRight() + QPoint(0, tick));
        p.drawLine(r.bottomLeft(), r.bottomLeft() + QPoint(tick, 0));
        p.drawLine(r.bottomLeft(), r.bottomLeft() + QPoint(0, -tick));
        p.drawLine(r.bottomRight(), r.bottomRight() + QPoint(-tick, 0));
        p.drawLine(r.bottomRight(), r.bottomRight() + QPoint(0, -tick));
    }
    p.restore();
}

void CaptureOverlay::paintAutoMosaicStatus(QPainter &p) const
{
    if (!m_autoMosaic || m_atool != ATool::Mosaic || m_mode != Adjusting || m_sel.isNull()) return;
    const QRect sel = m_sel.normalized();
    const qint64 elapsed = m_autoScanElapsed.isValid() ? m_autoScanElapsed.elapsed() : 0;

    if (m_autoPending) {
        p.save();
        p.setClipRect(sel);
        p.setRenderHint(QPainter::Antialiasing, false);

        // A sparse analysis grid gives the frozen capture a scanning surface
        // without hiding the content underneath.
        const QPen minor(QColor(143, 155, 255, 10), 1);
        const QPen major(QColor(143, 155, 255, 24), 1);
        for (int x = sel.left() + 24; x < sel.right(); x += 24) {
            p.setPen((x - sel.left()) % 96 == 0 ? major : minor);
            p.drawLine(QPointF(x + 0.5, sel.top()), QPointF(x + 0.5, sel.bottom()));
        }
        for (int y = sel.top() + 24; y < sel.bottom(); y += 24) {
            p.setPen((y - sel.top()) % 96 == 0 ? major : minor);
            p.drawLine(QPointF(sel.left(), y + 0.5), QPointF(sel.right(), y + 0.5));
        }

        const qint64 phase = elapsed % (kAutoScanHalfMs * 2);
        const bool down = phase < kAutoScanHalfMs;
        const qreal raw = down ? phase / qreal(kAutoScanHalfMs)
                               : (phase - kAutoScanHalfMs) / qreal(kAutoScanHalfMs);
        const qreal eased = raw < 0.5 ? 4 * raw * raw * raw
                                      : 1 - qPow(-2 * raw + 2, 3) / 2;
        const qreal edge = down ? eased * sel.height() : (1.0 - eased) * sel.height();
        const qreal bandH = qMin<qreal>(220, sel.height());
        const QRectF band = down
            ? QRectF(sel.left(), sel.top() + edge - bandH, sel.width(), bandH)
            : QRectF(sel.left(), sel.top() + edge, sel.width(), bandH);

        QLinearGradient gradient(band.topLeft(), band.bottomLeft());
        if (down) {
            gradient.setColorAt(0.00, QColor(94, 106, 210, 0));
            gradient.setColorAt(0.45, QColor(94, 106, 210, 9));
            gradient.setColorAt(0.82, QColor(94, 106, 210, 33));
            gradient.setColorAt(0.96, QColor(143, 155, 255, 61));
            gradient.setColorAt(1.00, QColor(168, 177, 255, 133));
        } else {
            gradient.setColorAt(0.00, QColor(168, 177, 255, 133));
            gradient.setColorAt(0.04, QColor(143, 155, 255, 61));
            gradient.setColorAt(0.18, QColor(94, 106, 210, 33));
            gradient.setColorAt(0.55, QColor(94, 106, 210, 9));
            gradient.setColorAt(1.00, QColor(94, 106, 210, 0));
        }
        p.fillRect(band, gradient);

        static const QPixmap checker = [] {
            QPixmap tile(28, 28);
            tile.fill(Qt::transparent);
            QPainter painter(&tile);
            const QColor cell(168, 177, 255, 18);
            painter.fillRect(0, 0, 14, 14, cell);
            painter.fillRect(14, 14, 14, 14, cell);
            return tile;
        }();
        p.save();
        p.setClipRect(band.toRect());
        p.setOpacity(0.55);
        p.fillRect(band, QBrush(checker));
        p.restore();

        const QPointF from(sel.left(), sel.top() + edge);
        const QPointF to(sel.right(), sel.top() + edge);
        p.setPen(QPen(QColor(94, 106, 210, 80), 7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(from, to);
        p.setPen(QPen(QColor(200, 205, 255, 235), 1.2));
        p.drawLine(from, to);
        p.restore();
    }

}
