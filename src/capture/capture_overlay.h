#pragma once

#include "platform/window_detection.h"
#include "platform/output.h"
#include "capture/auto_mosaic.h"
#include "capture/pixel_inspector.h"
#include "capture/tool_layout_dialog.h"
#include "shared/widgets.h"
class OcrDialog;
#include <QObject>
#include <QHash>
#include <QStringList>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QColor>
#include <QFont>
#include <QTransform>
#include <QPaintEvent>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QTextDocument>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QSlider>
#include <QDialog>
#include <QColorDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QCursor>
#include <QTimer>
#include <QElapsedTimer>
#include <QPointer>
#include <QCloseEvent>
#include <QPolygonF>
#include <QFutureWatcher>
#include <atomic>
#include <functional>
#include <memory>
#include <qt_windows.h>

class QVariantAnimation;

// ---------- Fullscreen selection: frozen frame, adjustable region, and action toolbar. ----------
class CaptureOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit CaptureOverlay(const QVector<ScreenFrame> &frames,
                            const WindowTargets &nativeWindows = {},
                            const DetectionOptions &options = DetectionOptions(), bool liveDetection = false,
                            bool pinEdit = false, QWidget *embedParent = nullptr);
    ~CaptureOverlay() override;
    // Instant annotation tools, shared with callers that reopen existing
    // pixels (pin edit) in this overlay.
    enum class ATool { None, Rect, Ellipse, Arrow, Pen, Marker, Text, Number, Mosaic, Eraser, Watermark };
    // Arrow comment style, persisted per arrow and reused as the next dialog default.
    struct CommentStyle {
        QString family = QStringLiteral("Microsoft YaHei UI");
        qreal font = 0;                           // 0 = automatic size from the arrow width.
        int direction = 0;                        // 0 inline, 1 horizontal, 2 above, 3 below the upright shaft.
        bool bold = false;
        bool italic = false;
    };
    // Embedded mode: the same annotation engine as a child canvas inside a pin
    // window. The pin toolbar arms tools; the option strip detaches as a small
    // top-level bar the pin positions below its own toolbar.
    void armTool(ATool tool);                 // Arm / disarm and rebuild the strip.
    ATool armedTool() const { return m_atool; }
    QPixmap composited() const;               // Pin pixels with annotations painted in.
    void commitText();                        // Commit active text before external output.
    void setImage(const QImage &image);       // Refresh canvas pixels after a bake.
    void clearAll();                          // Drop annotations and history after a bake.
    bool hasAnnotations() const { return !m_anns.isEmpty() || editorActive(); }
    QWidget *optionsStrip() const { return m_toolOptions; }
    void undoAnnotation();
    void redoAnnotation();
    // Enters the overlay with an already committed selection (used for
    // re-annotating an existing image) and optionally arms a drawing tool.
    void presetSelection(const QRect &localSel, ATool tool = ATool::None);
    static int testOcr(const QString &previewPath);
    static int testBarcode(const QString &previewPath);
    static int testRoundRefresh(const QString &previewPath);
    static int testSelection();
    static int testSelectionBorders();
    static int testToolbarPlacement(const QString &previewPath);
    static int testThemeUi(const QString &previewPath);
    static int testColorPicker(const QString &previewPath);
    static int testMosaic(const QString &previewPath);
    static int testSmartErase(const QString &previewPath);
    static int testAutoMosaic(const QString &previewPath);
    static int testWindowDetection();
    static int testTextAnnotation();
    static int testArrowComment(const QString &previewPath = QString());
    static int testWatermark(const QString &previewPath = QString());
    static int testOutput();
signals:
    void grabbed(const QPixmap &pix, const QRect &globalRegion);
    void captured(const QPixmap &pix, const QRect &globalRegion);   // Any successful save/copy/pin output.
    void longRequested(const QRect &globalRegion);
    void undoAvailability(bool canUndo, bool canRedo);   // Embedded pin toolbar buttons.
    void stripRebuilt();                                 // Strip contents changed; reposition it.
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
private:
    bool m_eatContextMenu = false;                   // The right press already acted (finish/deselect).
    enum Handle : int { H_None = 0, H_New, H_Move, H_TL, H_TR, H_BL, H_BR, H_L, H_R, H_T, H_B };

    // ---------- instant annotation ----------
    struct Annotation {
        ATool tool = ATool::None;
        QColor color;
        int pen = 3;                                 // Stroke width in logical pixels.
        int mosaicStrength = 6;                      // Pixel block size (3-24), independent of brush width.
        bool mosaicBrush = false;
        bool mosaicBlur = false;
        bool mosaicSmart = false;                     // Content-aware erase, created by the eraser's smart mode.
        int mosaicLevels[2] = {6, 6};
        quint32 mosaicSeed = 1;
        mutable int mosaicCacheStrength = 0;
        mutable QImage mosaicCache, mosaicSource;
        QImage smartResult;                           // Full-size erased layer for the smart mode.
        bool smartPending = false, smartFailed = false;
        QRect smartRect;                              // Geometry the current result was computed for.
        QPolygonF smartStroke;
        int smartPen = -1;
        // Auto mosaic: one item owns every recognized block; manual blocks use an
        // empty key and never sync. The effect is cached per block so toggling and
        // wheel tuning never rebuild the whole capture.
        struct AutoBlock {
            QRect rect;
            QString key;
            bool included = true;
            mutable QImage effect;
            mutable int effectStrength = 0;
            mutable quint32 effectSeed = 0;
            mutable bool effectBlur = false;
            mutable qint64 effectSource = 0;
        };
        bool autoMosaic = false;
        QVector<AutoBlock> autoBlocks;
        QRect rect;                                  // Rect / Ellipse / Mosaic / Text / Number box.
        QPoint a, b;                                 // Arrow / straight highlighter endpoints.
        QPolygonF stroke;                            // Pen / brush polyline.
        // Arrow comment style, persisted per arrow and reused as the next dialog default.
        QString text;                                // Text / Number content.
        QString comment;                             // Arrow double-click comment, cut into the shaft.
        CommentStyle commentStyle;                   // Style captured with the comment.
        qreal font = 15;                             // Text / Number point size (1/72 inch).
        QString family = QStringLiteral("Microsoft YaHei UI");
        int arrowStyle = 0;                          // 0 straight, 1 straight bilateral, 2 line, 3 line bilateral,
                                                     // 4 hollow, 5 filled, 6 dimension line, 7 solid, 8 solid bilateral, 9 dimension arrow.
        int strokeStyle = 0;                         // Rect/Ellipse/Arrow/Pen line: 0 solid, 1 dash, 2 dot, 3 dash-dot, 4 dash-dot-dot.
        int numberStyle = 0;                         // 0 filled disc, 1 ring, 2 bare label.
        int numberFormat = 0;                        // 0 "1 2 3", 1 "I II III", 2 "A B C", 3 "a b c", 4 Chinese numerals.
        bool bold = false;                           // Text weight / slant / underline.
        bool italic = false;
        bool underline = false;
        qreal rot = 0;                               // Rotation around the box center (degrees).
        bool fill = false;                           // Rect / Ellipse solid fill; Marker / Eraser rectangle mode.
        bool markerEllipse = false;                  // Marker area drawn as a circle instead of a rectangle.
        qint64 createdAt = 0;                        // Watermark time token base (ms since epoch).
        int watermarkPosition = 0;                   // 0 tile; 1 bottom-right; 2 bottom-left; 3 top-right;
                                                     // 4 top-left; 5 top-center; 6 bottom-center; 7 center.
        qreal watermarkOpacity = 30;                 // 0..100 percent.
        qreal watermarkGap = 0;                      // Extra tile padding as a percentage of text height.
        qreal watermarkAngle = 315;                  // Tile texture rotation; 315 == -45 degrees.
    };
    static QRect annotationRect(const Annotation &ann);
    // Rotation helpers: item geometry stays in local space, the angle only transforms paint and hit tests.
    static QTransform annTransform(const Annotation &ann);
    static QRect rotatedBounds(const Annotation &ann);
    static QPoint mapToLocal(const Annotation &ann, const QPoint &pos);
    static QPoint mapFromLocal(const Annotation &ann, const QPoint &pos);
    // ---- object editing: hit tests, selection anchors, move/resize, undo/redo ----
    static qreal distToSegment(const QPointF &p, const QPointF &a, const QPointF &b);
    static bool annotationHit(const Annotation &ann, const QPoint &pos);
    static bool textInnerHit(const Annotation &ann, const QPoint &pos);
    int annotationAt(const QPoint &pos) const;
    int textAnnotationAt(const QPoint &pos) const;
    int arrowAnnotationAt(const QPoint &pos) const;
    void editArrowComment(int index);
    static CommentStyle defaultCommentStyle();
    static void saveCommentStyle(const CommentStyle &style);
    static QFont commentFont(const Annotation &ann);
    static qreal commentAngle(const Annotation &ann);
    static void commentPaths(const Annotation &ann, QPainterPath &text, QPainterPath &background);
    // Area anchors follow the shape contour; lines have endpoints and freehand strokes have none.
    QVector<QPoint> annAnchors(const Annotation &ann) const;
    Handle annHandleAt(const Annotation &ann, const QPoint &pos) const;
    void paintMosaicBorder(QPainter &p, const Annotation &ann, bool selected) const;
    static void paintHandle(QPainter &p, const QPointF &center);
    static QCursor mosaicBrushCursor(qreal width, qreal dpr);
    static QCursor eraserCursor(qreal width, qreal dpr);
    static QPainterPath strokeMask(const QPolygonF &stroke, qreal width);
    static QPainterPath eraserMask(const Annotation &ann);
    void updateAnnotationCursor(const QPoint &pos);
    static void translateAnnotation(Annotation &ann, const QPoint &d);
    void applyItemDrag(const QPoint &pos);
    QVector<Annotation> annotationSnapshot() const;
    void pushUndo();
    void syncUndoButtons();
    void normalizeNumbers();
    void deleteSelected();
    static QFont textFont(const Annotation &ann);
    static constexpr int TextMargin = 6;
    static int numberDiameter(const Annotation &ann);
    static void setupTextDocument(QTextDocument &doc, const Annotation &ann);
    static void fitTextAnnotation(Annotation &ann);
    // ---- native scene text editing: one layout for input, selection, and capture ----
    bool editorActive() const;
    Annotation editorAnnotation() const;
    void syncTextEditorGeometry();
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void beginTextEdit(const QPoint &pos, int existing = -1);
    void commitTextEdit();
    void applyEditorFont();
    void editorFontStep(int dir);
    void tipPosition(const QPoint &at);
    static QPoint keyDelta(int key);
    void nudgeSelected(int key);
    ATool optionsTool() const;
    bool mosaicBrush() const;
    bool markerAreaMode() const;
    int mosaicStrength() const;
    bool mosaicBlur() const;
    void switchMosaicEffect();
    void setMosaicStrength(int value);
    void selectMosaicMode(bool brush);
    // Auto mosaic: a mosaic sub-mode that scans the capture for identifiers.
    void selectAutoMosaic();
    void startAutoMosaicDetection();
    void cancelAutoMosaicDetection();
    void applyAutoMosaicResult(const AutoMosaic::Result &result, const QRect &selection, qreal dpr);
    Annotation *autoMosaicAnnotation();
    const Annotation *autoMosaicAnnotation() const;
    bool autoMosaicAt(const QPoint &pos, int *annIndex, int *blockIndex) const;
    void toggleAutoMosaicBlock(int annIndex, int blockIndex);
    void addAutoMosaicBlock(const QRect &rect);
    void clearAutoMosaicHover();
    void setAutoMosaicMode(bool blur);
    void setAutoMosaicStrength(int value);
    void paintAutoMosaicOverlay(QPainter &p) const;
    void paintAutoMosaicStatus(QPainter &p) const;
    void selectMarkerMode(bool fill, bool ellipse = false);
    void scheduleSmartErase(int index, const QImage &source);   // Async erase for the annotation at index.
    static QImage smartEraseMask(const Annotation &ann, const QSize &deviceSize, qreal dpr);
    void selectEraserMode(bool rect);
    void toggleEraserSmart();
    bool eraserRect() const;
    bool eraserSmart() const;
    // Rectangle mosaics and rectangle erasures: movable regions that keep their frame while their tool is armed.
    static bool regionItem(const Annotation &ann);
    qreal toolSize() const;
    void syncSizeInput();
    void commitSizeInput();
    void setToolSize(qreal value);
    qreal widthControlValue() const;
    void setWidthControl(int value);
    void tuneWidth(int dir, const QPoint &at);
    void beginAnnotation(const QPoint &pos);
    void updateAnnotation(const QPoint &pos, Qt::KeyboardModifiers mods = Qt::NoModifier);
    void commitAnnotation();
    void stopItemDrag();
    void endAnnotationDrag();
    void setToolNone();
    void selectTool(ATool tool);
    void syncToolButtons();
    // One color shared by every colored tool; recolors the live item or the selection.
    void pickToolColor(const QColor &color, bool undoable = true);
    void showColorPalette();
    void pickArrowStyle(int style);
    void pickArrowLineStyle(int style);
    void pickShapeStyle(int style);
    void toggleFill();
    void pickNumberOption(bool format, int value);
    static QString numberLabel(int n, int format);
    void showSizePopup(QToolButton *anchor, int minimum, int maximum);
    void setTextBold(bool on);
    void setTextItalic(bool on);
    void setTextUnderline(bool on);
    void setTextFamily(const QFont &font);
    void syncTextOptions();
    // Watermark: one whole-selection text overlay. It is controlled from the strip, not
    // selected as a regular item, so its geometry always follows the current selection.
    int watermarkIndex() const;
    Annotation *watermark();
    const Annotation *watermark() const;
    Annotation *ensureWatermarkItem();
    void ensureAutoWatermark();
    static QString watermarkString(const QString &text, qint64 createdAt);
    static void paintWatermark(QPainter &p, const Annotation &ann, const QRect &area);
    void setWatermarkText(const QString &text);
    void setWatermarkPosition(int position);
    void setWatermarkColor(const QColor &color, bool undoable = true);
    void setWatermarkFamily(const QString &family);
    void setWatermarkSize(int size, bool undoable = true);
    void setWatermarkOpacity(int opacity, bool undoable = true);
    void setWatermarkGap(int gap, bool undoable = true);
    void showWatermarkInputDialog(const QRect &workArea = QRect());
    void showWatermarkStylePopup(QToolButton *anchor);
    void showWatermarkColorDialog();
    void loadWatermarkSettings();
    void saveWatermarkSettings() const;
    // Options strip under the toolbar: contents follow the armed tool.
    void updateToolOptions();
    // Shared by the overlay (logical coords, origin 0, scale 1) and cropped() (native crop
    // coords): annotations are stored overlay-local and mapped through this transform.
    void paintAnnotations(QPainter &p, const QPointF &origin, qreal scale, const QRectF &clip,
                          bool includeEditor = true) const;
    static QImage mosaicTiles(const QImage &source, int block, quint32 seed);
    static QImage mosaicBlurred(const QImage &source, int radius);
    void drawAnnotation(QPainter &p, const Annotation &ann, const QImage &source, int index = -1) const;

    void startDetector();
    void setWindowTargets(const WindowTargets &windows);
    QRect localScreenRect(const QRect &global) const;
    QRect globalScreenRect(const QRect &local) const;
    QRect nativeDetectionRect(const QRect &local) const;
    void cancelDetection();
    void requestDetection(const QPoint &point, const QRect &previous = {}, bool force = false);
    void refineDetection();
    void recoverDetector();
    // Hover-driven repaints stay incremental: a full-desktop repaint per follow update
    // saturates the GUI thread during fast slides, and painting is stateless (frozen frame
    // plus mask), so the union of the old and new rectangles reproduces the surface.
    void updateHoverRegion(const QRect &oldRect, const QRect &newRect);
    void updateSelectionRegion(const QRect &oldSel, const QRect &newSel);
    void setHoverRect(const QRect &rect, bool animate);
    void queueHoverPaint();
    void applyHoverPaint(bool animate);
    void clearHover();
    void handleDetectedRect(const QRect &native, quint64 id);

    void updateInspector();
    static QPoint inspectorPosition(const QPoint &cursor, const QSize &size, const QRect &bounds);
    // A layered window's rect includes its self-drawn shadow. Probe the OS
    // hit-test surface to drop the invisible margin without app-specific hacks.
    QRect contentRectFor(const DetectedWindow &win, const QPoint &point) const;
    QRect trimLayeredContent(HWND hwnd, const QRect &native) const;

    void resetSelection();
    QRect windowAt(const QPoint &p) const;
    static QVector<QRect> handleRects(const QRect &r);
    bool onSelectionBorder(const QPoint &p) const;
    Handle hitHandle(const QPoint &p) const;
    static Qt::CursorShape cursorForHandle(Handle h);
    QRect resizeBy(Handle h, const QPoint &pos);
    void updateTip();
    void createToolBars();
    void buildToolLayout();
    void relayoutToolButtons();
    void loadToolLayout();
    void saveToolLayout();
    QVector<ToolLayoutItem> currentToolLayout() const;
    void applyToolLayout(const QVector<ToolLayoutItem> &items);
    QWidget *barForSide(const QString &side) const;
    QWidget *widgetForTool(const QString &id) const;
    QString toolSide(const QString &id) const;
    QStringList toolsForSide(const QString &side) const;
    QString activeToolSide() const;
    bool isToolBarWidget(const QObject *object) const;
    bool overToolChrome(const QPoint &point) const;
    void raiseToolBars();
    void updateLayoutCardRect();
    QRect avoidLayoutCard(const QRect &bar, const QRect &avail, const QString &side) const;
    void rebuildMorePanel();
    void toggleMorePanel(bool open);
    void placeMorePanel();
    void showToolLayoutDialog();
    void triggerToolButton(QToolButton *button);
    QString toolDisplayName(const QString &id) const;
    Icon toolIcon(const QString &id) const;
    void placeToolBars();
    void positionToolOptions();
    void hideToolBars();
    void showToolBars();
    QPixmap cropped() const;
    void buildShot(const QVector<ScreenFrame> &frames);
    QPainterPath selectionPath(const QRect &r) const;
    void applyRoundSettings(bool persist);
    enum class SizeUnit { Px, Dp };
    struct SizePreset { QString name; QRect global; };
    QRect nativeFromGlobal(const QRect &global) const;
    QRect globalFromUnit(int x, int y, int width, int height, SizeUnit unit) const;
    QRect unitFromLocal(const QRect &local, SizeUnit unit) const;
    QSize sizeInUnit(const QRect &local, SizeUnit unit) const;
    void setSizeUnit(SizeUnit unit, bool persist);
    void showCustomSizePopup();
    void updateCustomSizeInputs();
    void customFieldsChanged();
    void applyCustomPreset(int index);
    void saveCurrentPreset();
    void saveSizePreset(const QString &name);
    void loadSizePresets();
    void saveSizePresets() const;
    void refreshPresetCombo(const QString &selected = QString());
    static int testCustomSize();
    void beginDesktopPreview();
    void refreshScreenshot();
    void applyRefreshFrames(const QVector<ScreenFrame> &frames);
    void updateRefreshIcon();
private slots:
    void ocrCapture();
    void translateCapture();
    void barcodeCapture();
    void saveCapture();
    bool saveCaptureTo(const QString &path, const ImageSaveOptions &options = {});
    void reportCaptured(const QPixmap &pix, const QRect &globalRegion);
    void copyCapture();
    void showOutputError(const QString &message);
    void pinCapture();
    void longCapture();
private:
    void openTextCard(bool translate, bool barcode = false);
    enum Mode : int { Selecting, Adjusting };
    QPixmap m_shot;
    QPointer<OcrDialog> m_ocrDialog;
    mutable QImage m_mosaicFrame;
    QVector<ScreenFrame> m_frames;
    WindowTargets m_initialTargets;                 // Native snapshot taken before this window activated.
    QVector<DetectedWindow> m_windowRects;          // Detected windows (logical, overlay-local) for click-select and snapping.
    QHash<quintptr, QRect> m_nativeRects;            // hwnd -> raw native window rect before content trimming.
    mutable QHash<quintptr, QPair<QRect, QRect>> m_contentCache; // hwnd -> raw native, trimmed local.
    DetectionOptions m_detectionOptions;
    QPointer<WindowDetector> m_detector;
    quint64 m_queryId = 0;
    QPoint m_detectionPoint;
    QVector<QRect> m_hoverHistory;
    bool m_queryPending = false, m_waitingClick = false, m_expanding = false;
    static constexpr int kDetectionWatchdogMs = 1500;
    QTimer m_refineTimer;
    QPoint m_refinePoint;
    bool m_refineScheduled = false;
    bool m_refineTried = false;
    bool m_pinAfterDetection = false;
    bool m_dragStarted = false;
    QRect m_hover;                                  // Detected target under the cursor; drives selection and hints.
    QRect m_hoverPaint;                             // Animated hole rect (88 ms OutCirc), retargeted per frame.
    QVariantAnimation *m_hoverAnimation = nullptr;
    QTimer m_hoverPaintTimer;                       // Coalesces retargets to one animation start per frame.
    QRect m_pressWindow;                            // Window under the press point; a click without drag selects it.
    PixelInspector *m_inspector = nullptr;
    QPoint m_pressPos;
    QRect m_sel, m_pressSel;
    Handle m_drag = H_None;
    Mode m_mode = Selecting;
    QLabel *m_sizeTip = nullptr;
    QWidget *m_sizeBar = nullptr;
    QToolButton *m_sizeUnitButton = nullptr;
    QToolButton *m_sizeCornerButton = nullptr;
    QSpinBox *m_sizeRadiusSpin = nullptr;
    QWidget *m_sizeRadiusDivider = nullptr;
    QToolButton *m_sizeLockButton = nullptr;
    QWidget *m_tools = nullptr;
    QWidget *m_toolsTop = nullptr;
    QWidget *m_toolsLeft = nullptr;
    QWidget *m_toolsRight = nullptr;
    QVector<QWidget *> m_toolButtons;
    QStringList m_defaultToolOrder;
    QStringList m_toolOrder;
    QHash<QString, QString> m_toolSide;
    QToolButton *m_moreButton = nullptr;
    QWidget *m_morePanel = nullptr;
    QRect m_layoutCardRect;
    QWidget *m_layoutCardWidget = nullptr;
    ATool m_atool = ATool::None;
    QColor m_acolor = QColor("#ed1c24");             // Red first: the loudest marking color.
    // Indexed by ATool: None, Rect, Ellipse, Arrow, Pen, Marker, Text, Number, Mosaic, Eraser, Watermark.
    // Marker stores a fifth of its drawn width, so 3 renders as the default 15 logical px.
    int m_toolPens[11] = {0, 5, 5, 8, 5, 3, 15, 15, 15, 15, 20};
    int m_mosaicLevels[2] = {6, 6}, m_mosaicWidth = 15;
    bool m_mosaicBrush = true, m_mosaicBlur = false, m_mosaicDragUndo = false;
    // Auto mosaic tool state. The mode button lives in the mosaic sub-strip; the
    // annotation keeps the shared effect settings, each block caches its own image.
    QToolButton *m_autoMosaicButton = nullptr;
    QComboBox *m_autoModeCombo = nullptr;
    QSlider *m_autoStrengthSlider = nullptr;
    QToolButton *m_autoLinkButton = nullptr;
    bool m_autoMosaic = false, m_autoMosaicBlur = false, m_autoMosaicSync = true, m_autoDragUndo = false;
    int m_autoMosaicLevels[2] = {6, 6}, m_autoMosaicStrength = 6;
    QFutureWatcher<AutoMosaic::Result> *m_autoWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_autoCancel;
    int m_autoEpoch = 0;
    bool m_autoPending = false;
    bool m_autoReveal = false;
    QRect m_autoDetectedRect;                      // Selection that already produced the current candidates.
    QTimer m_autoSpinTimer;
    QElapsedTimer m_autoScanElapsed;
    int m_autoHoverAnn = -1, m_autoHoverBlock = -1;
    int m_autoPressAnn = -1, m_autoPressBlock = -1;
    bool m_autoPressMoved = false, m_autoDragging = false;
    QPoint m_autoDragStart;
    QRect m_autoDragRect;
    int m_smartEpoch = 0;                         // Bumped whenever the annotation list is rebuilt.
    qreal m_afont = 15;                              // Base text/number font size (pt).
    QString m_afamily = QStringLiteral("Microsoft YaHei UI");
    bool m_afill = false;                            // Default fill mode for rect / ellipse.
    bool m_spaceHeld = false;                        // Space turns the next drag into a move.
    bool m_nudging = false;                          // Coalesces WASD bursts into one undo step.
    QVector<Annotation> m_anns;
    Annotation m_ann;                                // Annotation under construction.
    bool m_live = false;
    bool m_pinEdit = false;                            // Re-annotating a pin: canvas fixed, no capture chrome.
    bool m_captured = false;                           // One history entry per capture session.
    bool m_embedded = false;                           // Child canvas inside a pin window, no session at all.
    bool m_fwdDrag = false;                            // Embedded: this drag moves the pin window.
    int m_selAnn = -1;                               // Annotation selected for editing.
    Annotation m_pressAnn;                           // Snapshot for move/resize drags.
    Handle m_itemDrag = H_None;
    bool m_eraserRect = false;                       // Eraser starts in brush mode.
    bool m_eraserSmart = false;                      // Magic eraser: content-aware fill instead of pixel erasure.
    int m_eraserWidth = 15;
    // Hover cursors rebuild only when the brush size or screen DPR changes.
    QCursor m_mosaicBrushCursorCache, m_eraserCursorCache;
    qreal m_mosaicCursorSize = 0, m_mosaicCursorDpr = 0;
    qreal m_eraserCursorSize = 0, m_eraserCursorDpr = 0;
    QVector<QVector<Annotation>> m_undoStack, m_redoStack;
    QTimer m_detectionWatchdog;
    QToolButton *m_undoButton = nullptr;             // Enabled state follows the stacks.
    QToolButton *m_redoButton = nullptr;
    QGraphicsTextItem *m_editor = nullptr;           // Native editable text with content-sized bounds.
    QGraphicsView *m_textView = nullptr;             // Masked to the text item; canvas stays clickable.
    bool m_textPreedit = false, m_textMoving = false;
    QPoint m_textMovePress;
    QPointF m_textMoveOrigin;
    int m_editAnn = -1;                              // Annotation being re-edited.
    QColor m_editColor = QColor("#ed1c24");
    qreal m_editFont = 15;
    QString m_editFamily;
    bool m_committingText = false;
    HIMC m_imeCtx = nullptr;                         // Detached IME context for text editing.
    QToolButton *m_shapeButton = nullptr;            // Merged rect / ellipse tool button.
    QVector<QToolButton *> m_toolBtn;                // Tool button per ATool id (shape shared).
    QWidget *m_toolOptions = nullptr;                // Per-tool option strip under the toolbar.
    QComboBox *m_sizeInput = nullptr;                 // Editable text point-size presets.
    QToolButton *m_sizeButton = nullptr;             // Width button; opens the slider popup.
    QPointer<QDialog> m_sizePopup;
    QPointer<QSlider> m_sizeSlider;
    QPointer<QLabel> m_sizeValue;
    bool m_sizeDragUndo = false;
    QPointer<QDialog> m_customSizePopup;
    QPointer<QSpinBox> m_customX, m_customY, m_customWidth, m_customHeight;
    QPointer<QComboBox> m_presetCombo;
    QVector<SizePreset> m_sizePresets;
    bool m_updatingCustom = false;
    SizeUnit m_sizeUnit = SizeUnit::Px;
    bool m_lockAspect = false;
    qreal m_aspect = 1.0;
    QToolButton *m_refreshButton = nullptr;
    QVariantAnimation *m_refreshAnimation = nullptr;
    qreal m_refreshSpinAngle = 0;
    bool m_roundEnabled = false;
    int m_roundRadius = 0;                         // Radius in logical pixels, 0..60.
    bool m_desktopPreview = false;                 // Hold-to-see-desktop state for the refresh button.
    bool m_refreshing = false;
    bool m_liveDetection = false;
    std::function<QVector<ScreenFrame>()> m_frameSource;   // Replaceable for deterministic tests.
    QSlider *m_mosaicSlider = nullptr;
    QLabel *m_mosaicStrengthLabel = nullptr;
    QToolButton *m_mosaicPreview = nullptr;
    ColorBoard *m_colorBoard = nullptr;
    QPointer<QColorDialog> m_colorDialog;
    bool m_shapeEllipse = false;                     // Current sub-shape of the shape button.
    int m_arrowStyle = 0;                            // Shape, see Annotation::arrowStyle.
    int m_arrowLineStyle = 0;                        // Arrow line style, see Annotation::strokeStyle.
    int m_penStyle = 0;                              // Pen line style, see Annotation::strokeStyle.
    int m_shapeStyle = 0;                            // Current rect / ellipse outline, see Annotation::strokeStyle.
    int m_numberStyle = 0, m_numberFormat = 0;       // See Annotation::numberStyle / numberFormat.
    bool m_markerFill = false, m_markerEllipse = false;   // Marker brush, rectangle, or circle.
    // Watermark defaults, persisted as the next watermark's starting style.
    QString m_wmText, m_wmFamily = QStringLiteral("Microsoft YaHei");
    QColor m_wmColor = QColor(QStringLiteral("#ed1c24"));
    int m_wmSize = 20, m_wmOpacity = 30, m_wmGap = 30, m_wmPosition = 0;
    qreal m_wmAngle = 315;
    bool m_autoWatermark = false, m_autoWatermarkAdded = false, m_wmTextUndo = false;
    qint64 m_captureTime = 0;
    QPointer<QDialog> m_watermarkStylePopup;
    QPointer<ColorBoard> m_watermarkColorBoard;
    QLineEdit *m_watermarkEdit = nullptr;
    bool m_abold = false, m_aitalic = false, m_aunderline = false;       // Text tool defaults.
    bool m_editBold = false, m_editItalic = false, m_editUnderline = false; // Live editor style.
};
