#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

// Automatic privacy detection for the auto mosaic tool: OCR finds phone numbers,
// e-mail addresses and links, the barcode engine finds QR / matrix / 1D symbols.
// The result is a list of image-pixel rectangles; the overlay maps them to its
// logical canvas and lets the user toggle each block.
namespace AutoMosaic {

enum class Kind { None, Phone, Email, Url, Qr };

struct Detected {
    QRect rect;      // Source-image pixels.
    QString key;     // Normalized content; identical keys toggle together.
    Kind kind = Kind::None;
};

struct Result {
    QVector<Detected> blocks;
    QString error;
};

// Returns the privacy kind of one recognized text line, or None when the line
// carries no recognized identifier.
Kind classify(const QString &text);

// Blocking worker: run it off the GUI thread. Honours `cancel` between stages.
Result detect(const QImage &image, const std::shared_ptr<std::atomic_bool> &cancel = {});

// Self-check for the tiled candidate merge; needs no OCR assets.
bool testMerging();

}
