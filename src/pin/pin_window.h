#pragma once

#include "platform/output.h"
#include "text/engines/ocr_engine.h"
#include "shared/widgets.h"
#include "shared/geometry.h"
#include "capture/capture_overlay.h"
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QPaintEvent>
#include <QContextMenuEvent>
#include <QWidget>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QSizeF>
#include <QPointer>
#include <QMap>
#include <QPair>
#include <QtMath>
#include <qt_windows.h>

class QMenu;
class QLabel;
class QTimer;

// ---------- Pinned image window ----------
class PinWindow : public QWidget
{
    Q_OBJECT
public:
    explicit PinWindow(const QPixmap &pix, const QRect &displayRect);
    ~PinWindow() override;
    static int testPlacement();
    static int testThemeUi(const QString &previewPath);
    QMenu *createContextMenu();
    // The window keeps a transparent band around the image for its painted shadow.
    static constexpr int kShadowMargin = kPinShadowMargin;
    QRect imageRect() const { return rect().marginsRemoved(shadowMargins()); }
    QRect imageGeometry() const { return geometry().marginsRemoved(shadowMargins()); }
    // Shows the pin toolbar with the annotation canvas armed, as if editing had just been chosen;
    // canGoBack adds a back button before done that discards this pin.
    void openEditor(bool canGoBack = false) { m_canGoBack = canGoBack; toggleToolbar(true); armPinTool(-1); }
signals:
    void editorLeft(bool back);   // Only for openEditor(true): back pressed, or done/Esc kept the pin.
protected:
    void changeEvent(QEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    struct CharBox {
        QChar ch;
        QRectF rect; // In image logical coordinates
        int lineIdx = -1;
    };
private:
    static QMargins shadowMargins() { return QMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin); }
    void setImageGeometry(const QRect &r) { setGeometry(r.marginsAdded(shadowMargins())); }
    QPoint imagePos(const QPoint &windowPos) const { return windowPos - QPoint(kShadowMargin, kShadowMargin); }
    static QImage shadowImage(const QSize &window, const QColor &color, qreal dpr);
    QImage m_shadowCache;   // Rebuilt when size, DPR or color changes.
    void applyProcessedImage();
    void resetProcessing();
    void rotateBy(int degrees);
    void flipHorizontal();
    void flipVertical();
    void invertColors();
    void saveImage();
    void setOpacityPercent(int percent);
    void setBrightnessPercent(int percent);
    void setShadowEnabled(bool enabled);
    void toggleTextSelectable(bool enabled);
    void runOcrForTextSelection();
    void updateTextStatus();
    int charIndexAt(const QPoint &pos, bool proximity = true) const;
    QString selectedText() const;
    void toggleToolbar(bool enabled);
    void updateToolbarPosition();
    void openTranslate();   // The card takes the pin's place; the pin returns when it closes.
    void openBarcode();
    void openTextCard(bool barcode);
    // Reopens the pinned pixels inside the pin itself: the shared annotation
    // engine runs as a child canvas; tool is a CaptureOverlay::ATool value.
    void armPinTool(int tool);
    void positionStrip();                 // Option strip under the pin toolbar, left-aligned.
    void bakeAnnotations();               // Composite marks into pixels before processing/select/OCR.
    QPixmap annotatedPix() const;         // Display pixels including live annotations.
    QPixmap outputPix();                  // Commit active text, then return annotatedPix().
    QImage displayImage() const;          // m_pix at the current window size.
    // Text markup over the OCR selection, stored per character: highlight keeps
    // its own slot while the three line marks share one (they replace each
    // other), mirroring the reference pin's per-character map<type,color>.
    // Actions operate on the intersection with the selection, so selecting
    // more or fewer characters than were marked still hits the mark.
    enum TextDecoType : int { DecoHighlight = 0, DecoWavy, DecoLine, DecoStrike };
    void ensureSelToolbar();
    void updateSelToolbar();
    void drawTextDeco(int type, const QColor &color);   // Applies to selected chars.
    void clearTextDeco(int type = -1);                  // Removes from selected chars; -1 = every mark.
    bool selectionUniformDeco(int type, const QColor &color) const;
    void rebuildCharBoxes();                            // Rebuilds char boxes from the OCR lines.

    QPixmap m_originalPix;
    QPixmap m_pix;
    mutable QPixmap m_displayPix;   // Scaled paint cache.
    mutable qint64 m_displayKey = 0;
    mutable QSize m_displaySize;
    mutable qreal m_displayDpr = 0;
    double m_scale;
    QPoint m_dragOff;
    bool m_shadowOn = true;
    int m_rotation = 0;       // 0, 90, 180, 270
    bool m_flipH = false;
    bool m_flipV = false;
    bool m_inverted = false;
    int m_opacityPercent = 100;     // 10..100
    int m_brightnessPercent = 100;  // 0..200

    // Text selection support
    bool m_textSelectable = false;
    bool m_ocrRunning = false;
    QString m_textOcrError;
    QLabel *m_textStatus = nullptr;
    QLabel *m_scaleBadge = nullptr;   // Percentage shown at the image's top-left while wheeling.
    QTimer *m_scaleBadgeTimer = nullptr;
    bool m_ocrTipShown = false;
    int m_ocrGen = 0;
    OcrCancel m_ocrCancel;
    QVector<OcrLine> m_ocrLines;
    QVector<CharBox> m_charBoxes;
    QSizeF m_ocrImgSize;   // Raw pixel size of the image OCR boxed; boxes live in this space.
    int m_selAnchor = -1;
    int m_selLead = -1;
    bool m_selecting = false;
    bool m_selBarOpen = false;       // Shown only after a drag finishes; the close button clears it.
    // Per-character decorations keyed to the current OCR char boxes.
    QMap<int, QColor> m_decoHighlight;                  // charIdx -> fill color.
    QMap<int, QPair<int, QColor>> m_decoLine;           // charIdx -> {type, color}.
    QWidget *m_selToolbar = nullptr; // Floating markup toolbar under the selection.
    // Each markup tool remembers its own color (reference behavior); the last
    // tool clicked stays "current" and is the target of color/clear actions.
    int m_decoTool = 0;
    QColor m_decoToolColor[4];

    // Pin toolbar support
    bool m_toolbarOn = false;
    QWidget *m_toolbar = nullptr;
    QToolButton *m_undoBtn = nullptr;   // Pin-side undo/redo, driven by the canvas stacks.
    QToolButton *m_redoBtn = nullptr;
    QPointer<CaptureOverlay> m_canvas;  // Embedded annotation canvas (in-place editing).
    QToolButton *m_backBtn = nullptr;
    bool m_canGoBack = false;
};

// Place a slider flyout fully outside its parent; shared with tests.
QPoint flyoutOutsideParent(const QMenu *subMenu, const QMenu *parent, const QRect &item);
void separateFlyoutEdge(QMenu *menu);
