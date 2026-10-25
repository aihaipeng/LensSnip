#include "shared/dialog_chrome.h"
#include "shared/export_dialog.h"
#include "capture/capture_overlay.h"
#include "capture/tool_layout_dialog.h"
#include <QWindow>
#include "platform/output.h"
#include "shared/settings.h"
#include "shared/widgets.h"
#include "text/cards/ocr_dialog.h"
#include "text/engines/ocr_engine.h"
#include <QGuiApplication>
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QColor>
#include <QPaintEvent>
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <QAbstractItemView>
#include <QDialog>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QStandardPaths>
#include <QCursor>
#include <QMessageBox>
#include <QInputDialog>
#include <QTimer>
#include <QVariantAnimation>
#include <QEventLoop>
#include <QThread>
#include <QSettings>
#include <QPointer>
#include <QCloseEvent>
#include <QFrame>
#include <QPalette>
#include <QStandardItemModel>
#include <QStyle>
#include <QVariant>
#include <qt_windows.h>
#include <dwmapi.h>
#include <imm.h>
#include <algorithm>

namespace {
// Keep the normal combo popup behavior while painting the current text centered.
class CenteredCombo : public QComboBox
{
public:
    explicit CenteredCombo(QWidget *parent = nullptr) : QComboBox(parent)
    {
        setItemDelegate(new PopupItemDelegate(this));
    }
    void showPopup() override
    {
        prepareComboPopup(this);
        QComboBox::showPopup();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox opt;
        initStyleOption(&opt);
        const QString text = opt.currentText;
        opt.currentText.clear();
        painter.drawComplexControl(QStyle::CC_ComboBox, opt);
        const QRect field = style()->subControlRect(QStyle::CC_ComboBox, &opt,
                                                    QStyle::SC_ComboBoxEditField, this);
        painter.drawText(field, Qt::AlignCenter,
                         fontMetrics().elidedText(text, Qt::ElideRight, field.width()));
    }
};
} // namespace

CaptureOverlay::CaptureOverlay(const QVector<ScreenFrame> &frames,
                        const WindowTargets &nativeWindows,
                        const DetectionOptions &options, bool liveDetection, bool pinEdit, QWidget *embedParent)
    : m_frames(frames), m_detectionOptions(options), m_pinEdit(pinEdit), m_embedded(embedParent != nullptr)
{
    m_liveDetection = liveDetection;
    m_initialTargets = nativeWindows;
    loadWatermarkSettings();
    {
        LensSnipSettings settings;
        m_autoMosaicBlur = settings.value(QStringLiteral("Capture/AutoMosaicBlur"), false).toBool();
        m_autoMosaicLevels[0] = qBound(3, settings.value(QStringLiteral("Capture/AutoMosaicStrength"), 6).toInt(), 24);
        m_autoMosaicLevels[1] = qBound(3, settings.value(QStringLiteral("Capture/AutoMosaicBlurStrength"), 6).toInt(), 24);
        m_autoMosaicStrength = m_autoMosaicLevels[m_autoMosaicBlur ? 1 : 0];
        m_autoMosaicSync = settings.value(QStringLiteral("Capture/AutoMosaicSync"), true).toBool();
    }
    m_autoSpinTimer.setInterval(33);
    connect(&m_autoSpinTimer, &QTimer::timeout, this, [this] { update(); });
    if (m_embedded) {
        // A child canvas inside the pin: no session chrome, no freeze, no
        // foreground claim. The single frame is the pin's current pixels.
        setParent(embedParent);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setGeometry(QRect(QPoint(0, 0), embedParent->size()));
        m_shot = QPixmap(frames.isEmpty() ? QSize(1, 1) : frames.first().image.size());
        m_shot.fill(Qt::transparent);
        if (!frames.isEmpty()) {
            QPainter painter(&m_shot);
            painter.drawImage(QPoint(0, 0), frames.first().image);
        }
    } else {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_DeleteOnClose);
    // Let the hold-to-refresh button reveal the live desktop without repainting the frozen frame.
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setGeometry(QGuiApplication::primaryScreen()->virtualGeometry());
    QRect nativeDesktop;
    for (const GdiDisplay &display : gdiDisplays()) nativeDesktop |= display.native;
    if (!nativeDesktop.isEmpty()) {
        // Position the native window before freezing its local coordinate system.
        if (!SetWindowPos(reinterpret_cast<HWND>(winId()), nullptr, nativeDesktop.x(), nativeDesktop.y(),
                          nativeDesktop.width(), nativeDesktop.height(), SWP_NOZORDER | SWP_NOACTIVATE)) {
            QMessageBox::warning(nullptr, QStringLiteral("LensSnip"), tr("无法定位截图覆盖层"));
            deleteLater();
            return;
        }
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    LensSnipSettings settings;
    m_roundEnabled = settings.value(QStringLiteral("Capture/roundEnabled"), false).toBool();
    m_roundRadius = qBound(0, settings.value(QStringLiteral("Capture/roundRadius"), 0).toInt(), 60);
    const QString savedUnit = settings.value(QStringLiteral("Capture/sizeUnit"), QStringLiteral("px")).toString();
    m_sizeUnit = savedUnit.compare(QStringLiteral("dp"), Qt::CaseInsensitive) == 0
                      || savedUnit.compare(QStringLiteral("dip"), Qt::CaseInsensitive) == 0
                  ? SizeUnit::Dp : SizeUnit::Px;
    m_frameSource = [] { return freezeScreens(); };
    buildShot(m_frames);
    }
    m_inspector = new PixelInspector(this);

    // Size strip: the region size, its unit, and the aspect lock stay together.
    m_sizeBar = new QWidget(this);
    m_sizeBar->setObjectName(QStringLiteral("sizeBar"));
    m_sizeBar->setFixedHeight(32);
    m_sizeBar->setStyleSheet(QStringLiteral(
        "QWidget#sizeBar{background:#18191d;border:1px solid #2b2d33;border-radius:0;}"
        "QLabel{color:#f4f5f8;background:transparent;border:none;font-size:14px;padding:0 9px;}"
        "QToolButton{color:#f4f5f8;background:transparent;border:none;font-size:13px;padding:0;text-align:center;}"
        "QToolButton:hover{background:#222326;border-radius:0;}"
        "QToolButton:pressed{background:#2b2d33;border-radius:0;}"
        "QFrame#sizeBarDivider{background:#2b2d33;border:none;}"));
    auto *sizeBarLayout = new QHBoxLayout(m_sizeBar);
    sizeBarLayout->setContentsMargins(0, 0, 0, 0);
    sizeBarLayout->setSpacing(0);

    m_sizeTip = new QLabel(m_sizeBar);
    m_sizeTip->setCursor(Qt::PointingHandCursor);
    m_sizeTip->setAccessibleName(tr("自定义截图"));
    m_sizeTip->setAccessibleDescription(tr("点击调整区域大小"));
    m_sizeTip->installEventFilter(this);
    sizeBarLayout->addWidget(m_sizeTip);

    const auto addDivider = [&] {
        auto *line = new QFrame(m_sizeBar);
        line->setObjectName(QStringLiteral("sizeBarDivider"));
        line->setFixedSize(1, 16);
        sizeBarLayout->addWidget(line, 0, Qt::AlignVCenter);
    };

    addDivider();
    m_sizeUnitButton = new QToolButton(m_sizeBar);
    m_sizeUnitButton->setObjectName(QStringLiteral("sizeUnit"));
    m_sizeUnitButton->setText(QStringLiteral("px"));
    m_sizeUnitButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_sizeUnitButton->setCursor(Qt::PointingHandCursor);
    m_sizeUnitButton->setToolTip(tr("更改尺寸单位"));
    m_sizeUnitButton->setAccessibleName(tr("尺寸单位"));
    m_sizeUnitButton->setFixedWidth(38);
    connect(m_sizeUnitButton, &QToolButton::clicked, this, [this] {
        setSizeUnit(m_sizeUnit == SizeUnit::Px ? SizeUnit::Dp : SizeUnit::Px, true);
        updateCustomSizeInputs();
    });
    sizeBarLayout->addWidget(m_sizeUnitButton);

    addDivider();
    m_sizeCornerButton = new QToolButton(m_sizeBar);
    m_sizeCornerButton->setObjectName(QStringLiteral("sizeCorner"));
    m_sizeCornerButton->setCheckable(true);
    const bool rounded = m_roundEnabled;
    m_sizeCornerButton->setChecked(rounded);
    m_sizeCornerButton->setIcon(barIcon(rounded ? Icon::CornerRounded : Icon::CornerSquare));
    m_sizeCornerButton->setIconSize(QSize(20, 20));
    m_sizeCornerButton->setCursor(Qt::PointingHandCursor);
    m_sizeCornerButton->setToolTip(tr("切换直角/圆角"));
    m_sizeCornerButton->setAccessibleName(tr("圆角截图"));
    m_sizeCornerButton->setFixedWidth(34);
    connect(m_sizeCornerButton, &QToolButton::clicked, this, [this](bool checked) {
        m_roundEnabled = checked;
        if (checked && m_roundRadius <= 0) m_roundRadius = 24;
        applyRoundSettings(true);
        updateTip();
    });
    sizeBarLayout->addWidget(m_sizeCornerButton);

    m_sizeRadiusDivider = new QFrame(m_sizeBar);
    m_sizeRadiusDivider->setObjectName(QStringLiteral("sizeBarDivider"));
    m_sizeRadiusDivider->setFixedSize(1, 16);
    m_sizeRadiusDivider->setVisible(rounded);
    sizeBarLayout->addWidget(m_sizeRadiusDivider, 0, Qt::AlignVCenter);

    auto *radiusBox = new QWidget(m_sizeBar);
    radiusBox->setObjectName(QStringLiteral("sizeRadiusBox"));
    radiusBox->setFixedWidth(56);
    radiusBox->setVisible(rounded);
    auto *radiusLayout = new QHBoxLayout(radiusBox);
    radiusLayout->setContentsMargins(0, 0, 0, 0);
    radiusLayout->setSpacing(0);
    m_sizeRadiusSpin = new QSpinBox(radiusBox);
    m_sizeRadiusSpin->setObjectName(QStringLiteral("sizeRadius"));
    m_sizeRadiusSpin->setRange(0, 60);
    m_sizeRadiusSpin->setValue(m_roundRadius);
    m_sizeRadiusSpin->setAlignment(Qt::AlignCenter);
    m_sizeRadiusSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_sizeRadiusSpin->setFrame(false);
    m_sizeRadiusSpin->setFixedHeight(28);
    m_sizeRadiusSpin->setStyleSheet(QStringLiteral(
        "QSpinBox{color:#f4f5f8;background:transparent;border:none;padding:0;font-size:14px;}"
        "QSpinBox:focus{background:#222326;border-radius:4px;}"));
    m_sizeRadiusSpin->installEventFilter(this);
    for (QWidget *child : m_sizeRadiusSpin->findChildren<QWidget *>())
        child->installEventFilter(this);
    radiusLayout->addWidget(m_sizeRadiusSpin, 1, Qt::AlignVCenter);
    auto *radiusSteps = new QWidget(radiusBox);
    auto *radiusStepLayout = new QVBoxLayout(radiusSteps);
    radiusStepLayout->setContentsMargins(0, 0, 0, 0);
    radiusStepLayout->setSpacing(0);
    const auto makeRadiusStep = [radiusSteps, this](const QString &name, Icon icon, auto step) {
        auto *button = new QToolButton(radiusSteps);
        button->setObjectName(name);
        button->setIcon(makeIcon(icon, 9, true));
        button->setIconSize(QSize(9, 9));
        button->setFixedSize(14, 15);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(QStringLiteral(
            "QToolButton{background:transparent;border:none;padding:0;}"
            "QToolButton:hover{background:#222326;border-radius:2px;}"));
        QObject::connect(button, &QToolButton::clicked, m_sizeRadiusSpin, step);
        return button;
    };
    radiusStepLayout->addWidget(makeRadiusStep(QStringLiteral("sizeRadiusUp"), Icon::ChevronUp, &QSpinBox::stepUp));
    radiusStepLayout->addWidget(makeRadiusStep(QStringLiteral("sizeRadiusDown"), Icon::ChevronDown, &QSpinBox::stepDown));
    radiusLayout->addWidget(radiusSteps, 0, Qt::AlignVCenter);
    sizeBarLayout->addWidget(radiusBox);
    connect(m_sizeRadiusSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_roundRadius = value;
        applyRoundSettings(true);
        updateTip();
    });

    addDivider();
    m_sizeLockButton = new QToolButton(m_sizeBar);
    m_sizeLockButton->setObjectName(QStringLiteral("sizeLockAspect"));
    m_sizeLockButton->setCheckable(true);
    m_sizeLockButton->setIcon(barIcon(Icon::AspectLock));
    m_sizeLockButton->setIconSize(QSize(20, 20));
    m_sizeLockButton->setCursor(Qt::PointingHandCursor);
    m_sizeLockButton->setToolTip(tr("锁定长宽比"));
    m_sizeLockButton->setAccessibleName(tr("锁定长宽比"));
    m_sizeLockButton->setFixedWidth(34);
    connect(m_sizeLockButton, &QToolButton::toggled, this, [this](bool locked) {
        m_lockAspect = locked;
        // The strip is usable while the editor is closed, so derive the ratio
        // from the live selection instead of stale editor fields.
        if (locked && !m_sel.isNull() && m_sel.height() > 0)
            m_aspect = qreal(m_sel.width()) / m_sel.height();
        updateTip();
    });
    sizeBarLayout->addWidget(m_sizeLockButton);
    m_sizeBar->hide();

    // The overlay paints matching solid backgrounds for both toolbar containers.
    qApp->installEventFilter(this);                  // Swallows hover tooltips of every capture control.
    m_tools = new QWidget(this);
    m_tools->installEventFilter(this);
    m_tools->setCursor(Qt::ArrowCursor);
    // Dark bar language: compact buttons, neutral hover, accent glyph when checked, 1px dividers.
    const QString barQss = QStringLiteral(
        "QToolButton{color:#f4f5f8;background:transparent;border:none;border-radius:4px;padding:5px;}"
        "QToolButton:hover{background-color:#222326;}"
        "QToolButton:pressed{background-color:#2b2d33;}"
        "QToolButton:checked{background:transparent;}"
        "QToolButton:checked:hover{background-color:#222326;}"
        "QToolButton#toolMoreButton{padding:0px;border:none;background:transparent;}"
        "QToolButton#toolMoreButton:hover{background-color:#222326;}"
        "QToolButton#toolMoreButton:checked{background-color:#2b2d33;}"
        "QFrame#barSeparator{background-color:#2b2d33;margin:0px 6px;border:none;}"
        "QFrame#barSeparatorH{background-color:#2b2d33;margin:6px 0px;border:none;}");
    m_tools->setStyleSheet(barQss);
    m_toolsTop = new QWidget(this);
    m_toolsLeft = new QWidget(this);
    m_toolsRight = new QWidget(this);
    m_toolsTop->setObjectName(QStringLiteral("toolBarTop"));
    m_toolsLeft->setObjectName(QStringLiteral("toolBarLeft"));
    m_toolsRight->setObjectName(QStringLiteral("toolBarRight"));
    for (QWidget *bar : {m_toolsTop, m_toolsLeft, m_toolsRight}) {
        bar->installEventFilter(this);
        bar->setCursor(Qt::ArrowCursor);
        bar->setStyleSheet(barQss);
        bar->hide();
    }
    // Capture output actions; a pin edit session replaces them with one
    // confirm button that applies the result back to the pin.
    QList<QWidget *> outputButtons;
    if (!pinEdit) {
        auto *save = barButton(Icon::Save, tr("保存到文件 (Ctrl+S)"));
        auto *copy = barButton(Icon::Copy, tr("复制到剪贴板 (Ctrl+C)"));
        auto *pin = barButton(Icon::Pin, tr("贴在屏幕上 (Enter / 双击)"));
        auto *scroll = barButton(Icon::LongShot, tr("长截图：手动滚动拼接长图"));
        auto *ocr = barButton(Icon::Ocr, tr("文字识别 (OCR)"));
        ocr->setObjectName(QStringLiteral("ocrButton"));
        ocr->setAccessibleName(tr("文字识别 OCR"));
        const QString ocrMissing = OcrEngine::assetError(QCoreApplication::applicationDirPath());
        if (!ocrMissing.isEmpty()) { ocr->setEnabled(false); ocr->setToolTip(ocrMissing); }
        auto *translate = barButton(Icon::Translate, tr("识别并翻译文字"));
        translate->setAccessibleName(tr("翻译"));
        if (!ocrMissing.isEmpty()) { translate->setEnabled(false); translate->setToolTip(ocrMissing); }
        auto *barcode = barButton(Icon::Barcode, tr("识别二维码/条形码"));
        barcode->setObjectName(QStringLiteral("barcodeButton"));
        barcode->setAccessibleName(tr("识别二维码/条形码"));
        m_refreshButton = barButton(Icon::Refresh, tr("刷新截图（按住持续刷新）"));
        m_refreshButton->setObjectName(QStringLiteral("refreshButton"));
        m_refreshButton->setAccessibleName(tr("刷新截图"));
        m_refreshButton->setCheckable(true);
        m_refreshAnimation = new QVariantAnimation(m_refreshButton);
        m_refreshAnimation->setDuration(1000);
        m_refreshAnimation->setStartValue(0.0);
        m_refreshAnimation->setEndValue(360.0);
        m_refreshAnimation->setLoopCount(-1);
        connect(m_refreshAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_refreshSpinAngle = value.toReal();
            updateRefreshIcon();
        });
        connect(m_refreshButton, &QToolButton::pressed, this, &CaptureOverlay::beginDesktopPreview);
        connect(m_refreshButton, &QToolButton::released, this, [this] {
            if (m_refreshAnimation) m_refreshAnimation->stop();
            m_refreshSpinAngle = 0;
            m_refreshButton->setIcon(barIcon(Icon::Refresh));
            m_refreshButton->setChecked(false);
            if (!m_desktopPreview) return;
            m_desktopPreview = false;
            refreshScreenshot();
        });
        connect(m_refreshButton, &QToolButton::clicked, this, [this] { m_refreshButton->setChecked(false); });
        connect(save, &QToolButton::clicked, this, &CaptureOverlay::saveCapture);
        connect(copy, &QToolButton::clicked, this, &CaptureOverlay::copyCapture);
        connect(pin, &QToolButton::clicked, this, &CaptureOverlay::pinCapture);
        connect(scroll, &QToolButton::clicked, this, &CaptureOverlay::longCapture);
        connect(ocr, &QToolButton::clicked, this, &CaptureOverlay::ocrCapture);
        connect(translate, &QToolButton::clicked, this, &CaptureOverlay::translateCapture);
        connect(barcode, &QToolButton::clicked, this, &CaptureOverlay::barcodeCapture);
        outputButtons = {scroll, ocr, translate, barcode, pin, m_refreshButton, save, copy};
    }
    // Instant annotation tools: draw inside the committed selection; every capture
    // action composes them in. One shape button covers rect / ellipse; per-tool
    // options (sub-shape, arrow style, colors, text weight) live on the strip below.
    auto *undo = barButton(Icon::Undo, tr("撤销 (Ctrl+Z)"));
    connect(undo, &QToolButton::clicked, this, &CaptureOverlay::undoAnnotation);
    m_undoButton = undo;
    auto *redo = barButton(Icon::Redo, tr("重做 (Ctrl+Y)"));
    connect(redo, &QToolButton::clicked, this, &CaptureOverlay::redoAnnotation);
    m_redoButton = redo;
    syncUndoButtons();                             // Empty stacks: both start greyed out.
    m_shapeButton = new QToolButton;
    m_shapeButton->setIcon(barIcon(Icon::Rect));
    m_shapeButton->setIconSize(QSize(Bar::icon, Bar::icon));
    m_shapeButton->setToolTip(tr("形状：矩形/椭圆 (R=矩形 / O=椭圆)"));
    m_shapeButton->setCheckable(true);
    m_shapeButton->setCursor(Qt::PointingHandCursor);
    connect(m_shapeButton, &QToolButton::clicked, this, [this] {
        const ATool shape = m_shapeEllipse ? ATool::Ellipse : ATool::Rect;
        selectTool(m_atool == shape ? ATool::None : shape);
        setCursor(Qt::CrossCursor);
        update();
    });
    m_toolBtn.fill(nullptr, int(ATool::Watermark) + 1);
    m_toolBtn[int(ATool::Rect)] = m_shapeButton;
    m_toolBtn[int(ATool::Ellipse)] = m_shapeButton;
    struct { ATool tool; Icon icon; const char *tip; } const tools[] = {
        {ATool::Arrow, Icon::Arrow, QT_TRANSLATE_NOOP("CaptureOverlay", "箭头 (A)")},
        {ATool::Pen, Icon::Pen, QT_TRANSLATE_NOOP("CaptureOverlay", "画笔 (P)")},
        {ATool::Marker, Icon::Marker, QT_TRANSLATE_NOOP("CaptureOverlay", "荧光笔 (H)")},
        {ATool::Text, Icon::Text, QT_TRANSLATE_NOOP("CaptureOverlay", "文字 (T)")},
        {ATool::Number, Icon::Number, QT_TRANSLATE_NOOP("CaptureOverlay", "序号 (N)")},
        {ATool::Mosaic, Icon::Mosaic, QT_TRANSLATE_NOOP("CaptureOverlay", "马赛克 (M)")},
        {ATool::Eraser, Icon::Eraser, QT_TRANSLATE_NOOP("CaptureOverlay", "橡皮 (E)")},
        {ATool::Watermark, Icon::Watermark, QT_TRANSLATE_NOOP("CaptureOverlay", "水印 (W)")},
    };
    for (const auto &t : tools) {
        auto *button = barButton(t.icon, tr(t.tip));
        button->setCheckable(true);
        const ATool tool = t.tool;
        connect(button, &QToolButton::clicked, this, [this, tool] {
            selectTool(tool == m_atool ? ATool::None : tool);   // Reclick disarms, like the shortcut key.
            update();
        });
        m_toolBtn[int(tool)] = button;
    }
    // Drawing tools | undo, redo | capture actions ending with pin, save, copy.
    if (pinEdit) {
        auto *done = barButton(Icon::Check, tr("完成编辑 (Enter)"));
        connect(done, &QToolButton::clicked, this, &CaptureOverlay::pinCapture);
        m_toolButtons = {m_shapeButton, m_toolBtn[int(ATool::Arrow)], m_toolBtn[int(ATool::Pen)],
                         m_toolBtn[int(ATool::Marker)], m_toolBtn[int(ATool::Mosaic)],
                         m_toolBtn[int(ATool::Text)], m_toolBtn[int(ATool::Number)],
                         m_toolBtn[int(ATool::Eraser)], m_toolBtn[int(ATool::Watermark)], undo, redo, done};
    } else {
        m_toolButtons = {m_shapeButton, m_toolBtn[int(ATool::Arrow)], m_toolBtn[int(ATool::Pen)],
                         m_toolBtn[int(ATool::Marker)], m_toolBtn[int(ATool::Mosaic)],
                         m_toolBtn[int(ATool::Text)], m_toolBtn[int(ATool::Number)],
                         m_toolBtn[int(ATool::Eraser)], m_toolBtn[int(ATool::Watermark)], undo, redo};
        m_toolButtons += outputButtons;
    }
    const QStringList toolIds = pinEdit
        ? QStringList{QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("pen"),
                      QStringLiteral("marker"), QStringLiteral("mosaic"),
                      QStringLiteral("text"), QStringLiteral("number"), QStringLiteral("eraser"),
                      QStringLiteral("watermark"), QStringLiteral("undo"), QStringLiteral("redo"),
                      QStringLiteral("done")}
        : QStringList{QStringLiteral("shape"), QStringLiteral("arrow"), QStringLiteral("pen"),
                      QStringLiteral("marker"), QStringLiteral("mosaic"),
                      QStringLiteral("text"), QStringLiteral("number"), QStringLiteral("eraser"),
                      QStringLiteral("watermark"), QStringLiteral("undo"), QStringLiteral("redo"),
                      QStringLiteral("scroll"), QStringLiteral("ocr"), QStringLiteral("translate"),
                      QStringLiteral("barcode"), QStringLiteral("pin"), QStringLiteral("refresh"),
                      QStringLiteral("save"), QStringLiteral("copy")};
    const bool canEditLayout = !pinEdit && !m_embedded;
    m_defaultToolOrder = toolIds;
    for (int i = 0; i < m_toolButtons.size(); ++i) {
        QWidget *button = m_toolButtons[i];
        const QString id = toolIds.value(i);
        button->setProperty("lenssnipToolId", id);
        const bool history = id == QStringLiteral("undo") || id == QStringLiteral("redo");
        const bool output = id == QStringLiteral("scroll") || id == QStringLiteral("ocr")
            || id == QStringLiteral("translate") || id == QStringLiteral("barcode")
            || id == QStringLiteral("pin") || id == QStringLiteral("refresh")
            || id == QStringLiteral("save") || id == QStringLiteral("copy")
            || id == QStringLiteral("done");
        button->setProperty("lenssnipToolGroup",
                            history ? QStringLiteral("history")
                                    : output ? QStringLiteral("output") : QStringLiteral("tools"));
        button->setFocusPolicy(Qt::NoFocus);
        m_toolOrder << id;
        m_toolSide.insert(id, QStringLiteral("bottom"));
    }
    if (canEditLayout) {
        m_moreButton = new QToolButton(m_tools);
        m_moreButton->setObjectName(QStringLiteral("toolMoreButton"));
        m_moreButton->setIcon(barIcon(Icon::More, 20));
        m_moreButton->setIconSize(QSize(20, 20));
        m_moreButton->setFixedSize(20, Bar::button);
        m_moreButton->setCheckable(true);
        m_moreButton->setCursor(Qt::PointingHandCursor);
        m_moreButton->setFocusPolicy(Qt::NoFocus);
        m_moreButton->setToolTip(tr("更多工具"));
        m_moreButton->setAccessibleName(tr("更多工具"));
        connect(m_moreButton, &QToolButton::toggled, this, &CaptureOverlay::toggleMorePanel);

        m_morePanel = new QWidget(this);
        m_morePanel->setObjectName(QStringLiteral("toolMorePanel"));
        m_morePanel->setAttribute(Qt::WA_StyledBackground);
        m_morePanel->setStyleSheet(QStringLiteral(
            "#toolMorePanel{background:#18191d;border:1px solid #2b2d33;border-radius:8px;}"
            "QToolButton{color:#f4f5f8;background:transparent;border:none;border-radius:4px;padding:5px;}"
            "QToolButton:hover{background:#222326;}"
            "QToolButton:pressed{background:#2b2d33;}"
            "QToolButton#toolMoreAdjust{color:#f4f5f8;background:#222326;font-size:12px;"
            "padding:0px;border-radius:6px;}"
            "QToolButton#toolMoreAdjust:hover{background:#2b2d33;}"));
        auto *moreLayout = new QHBoxLayout(m_morePanel);
        moreLayout->setContentsMargins(8, 7, 8, 7);
        moreLayout->setSpacing(4);
        m_morePanel->installEventFilter(this);
        m_morePanel->hide();
        m_moreButton->setProperty("lenssnipToolId", QStringLiteral("more"));
        m_toolOrder << QStringLiteral("more");
        m_toolSide.insert(QStringLiteral("more"), QStringLiteral("bottom"));
        loadToolLayout();
    }

    const QString optionsQss = barQss + QStringLiteral(
        "QLabel{color:#f4f5f8;font-size:13px;border:none;}"
        "QComboBox{color:#f4f5f8;background:#0c0d10;border:none;border-radius:12px;"
        "padding:1px 4px 1px 12px;font-size:13px;}"
        "QComboBox:hover{background:#222326;}"
        "QComboBox:focus{background:#222326;}"
        "QComboBox::drop-down{border:none;width:22px;background:transparent;}"
        "QComboBox::down-arrow{image:none;width:0px;height:0px;}"
        "QComboBox QAbstractItemView{outline:0px;color:#f4f5f8;background:#18191d;border:none;"
        "padding:0px;selection-background-color:#5e6ad2;selection-color:#ffffff;}"
        "QLineEdit{color:#f4f5f8;background:transparent;border:none;"
        "selection-background-color:#5e6ad2;selection-color:#ffffff;}"
        "QCheckBox{color:#f4f5f8;font-size:13px;spacing:6px;}"
        "QCheckBox::indicator{width:14px;height:14px;border:1px solid #6b7280;"
        "border-radius:3px;background:#0c0d10;}"
        "QCheckBox::indicator:checked{background:#5e6ad2;border-color:#5e6ad2;}"
        "QToolButton:focus{border:1px solid #828fff;border-radius:6px;}");
    if (m_embedded) {
        // The option strip detaches as a small top-level rounded bar;
        // the pin positions it below its own toolbar.
        m_toolOptions = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_toolOptions->setObjectName(QStringLiteral("pinStrip"));
        m_toolOptions->setAttribute(Qt::WA_StyledBackground);
        m_toolOptions->setStyleSheet(QStringLiteral(
            "#pinStrip{background:#18191d;border:none;border-radius:8px;}") + optionsQss);
    } else {
        m_toolOptions = new QWidget(this);
        m_toolOptions->setStyleSheet(optionsQss);
    }
    m_toolOptions->installEventFilter(this);
    m_toolOptions->setCursor(Qt::ArrowCursor);
    m_toolOptions->hide();
    buildToolLayout();
    for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight}) {
        if (!bar) continue;
        bar->adjustSize();
        bar->hide();
    }

    if (m_embedded) {
        // Whole canvas is the committed selection; drawing works immediately.
        presetSelection(rect(), ATool::None);
        return;
    }
    show();
    setWindowTargets(nativeWindows);
    // Claim foreground synchronously (hotkey handling grants activation rights)
    // and detach the IME so plain C/Shift always reach this window.
    HWND handle = reinterpret_cast<HWND>(winId());
    SetForegroundWindow(handle);
    SetFocus(handle);
    m_imeCtx = ImmAssociateContext(handle, nullptr);   // Detached; re-attached for text editing.
    setFocus();
    updateInspector();
    if (liveDetection) {
        m_detectionWatchdog.setSingleShot(true);
        connect(&m_detectionWatchdog, &QTimer::timeout, this, &CaptureOverlay::recoverDetector);
        m_refineTimer.setSingleShot(true);
        connect(&m_refineTimer, &QTimer::timeout, this, &CaptureOverlay::refineDetection);
        m_hoverPaintTimer.setSingleShot(true);
        m_hoverPaintTimer.setInterval(16);   // One display frame; avoid restarting the animation before it can tick.
        connect(&m_hoverPaintTimer, &QTimer::timeout, this, [this] { applyHoverPaint(true); });
        startDetector();
        requestDetection(mapFromGlobal(QCursor::pos()));
    }
    // The first hover result can arrive while the layered window is still being
    // exposed, leaving only its hole painted. Repaint the whole surface once the
    // event loop has completed the show.
    QTimer::singleShot(100, this, [this] {
        if (isVisible() && !m_desktopPreview) update();
    });
}

void CaptureOverlay::buildShot(const QVector<ScreenFrame> &frames)
{
    qreal scale = 1;
    for (const ScreenFrame &frame : frames)
        scale = qMax(scale, frame.image.devicePixelRatio() > 1 ? frame.image.devicePixelRatio()
                        : qreal(frame.image.width()) / frame.logicalRect.width());
    m_shot = QPixmap(qRound(width() * scale), qRound(height() * scale));
    m_shot.setDevicePixelRatio(scale);
    m_shot.fill(Qt::black);
    QPainter painter(&m_shot);
    for (const ScreenFrame &frame : frames) {
        const QPointF local = windowHandle()->mapFromGlobal(QPointF(frame.logicalRect.topLeft()));
        const QSizeF size = frame.image.devicePixelRatio() > 1
            ? frame.image.deviceIndependentSize() : QSizeF(frame.logicalRect.size());
        painter.drawImage(QRectF(local, size), frame.image);
    }
}

CaptureOverlay::~CaptureOverlay()
{
    if (m_detector) m_detector->stop();
    if (m_embedded) delete m_toolOptions;   // The detached strip has no parent.
}

void CaptureOverlay::paintEvent(QPaintEvent *event)
{
    QPainter p(this);
    const auto paintBars = [&] {
        for (QWidget *glass : {m_toolsTop, m_toolsLeft, m_toolsRight, m_tools, m_toolOptions}) {
            if (m_embedded) break;                   // Detached or unused bars paint themselves.
            if (!glass->isVisible()) continue;
            const QRectF bar = glass->geometry();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 17));    // 1px light rim lifts the bar off dark captures.
            p.drawRoundedRect(bar.adjusted(-1, -1, 1, 1), Bar::radius + 1, Bar::radius + 1);
            p.setBrush(Bar::surface);
            p.drawRoundedRect(bar, Bar::radius, Bar::radius);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    };
    if (m_desktopPreview) {                       // Hold-to-refresh: keep the frame over the live desktop.
        if (!m_sel.isNull()) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(Bar::accent, 2));
            p.setBrush(QBrush(Qt::NoBrush));
            if (m_roundEnabled && m_roundRadius > 0 && !m_embedded) p.drawPath(selectionPath(m_sel));
            else p.drawRect(QRectF(m_sel).adjusted(0.5, 0.5, -0.5, -0.5));
            p.setRenderHint(QPainter::Antialiasing, false);
            if (!m_pinEdit)
                for (const QRect &h : handleRects(m_sel))
                    paintHandle(p, h.center() + QPoint(1, 1));
        }
        paintBars();
        return;
    }
    // Embedded: a transparent layer of marks only; the pin paints its own
    // pixels, border and text markups underneath, unchanged.
    if (!m_embedded) p.drawPixmap(0, 0, m_shot);   // Frozen frame covers the virtual desktop.
    // The mask hole follows the animated target, falling back to the committed selection.
    const QRect hover = !m_hoverPaint.isNull() ? m_hoverPaint : m_hover;
    const QRect hole = !hover.isNull() ? hover : m_sel;
    if (hole.isNull()) {
        p.fillRect(rect(), QColor(0, 0, 0, 120));
        return;
    }
    if (!m_pinEdit) {                           // Pin editing draws on the pin as-is:
        const QColor dim(0, 0, 0, 120);         // no dim mask and no selection frame.
        if (m_roundEnabled && m_roundRadius > 0 && !m_embedded) {
            QPainterPath mask;
            mask.setFillRule(Qt::OddEvenFill);
            mask.addRect(QRectF(rect()));
            mask.addPath(selectionPath(hole));
            p.fillPath(mask, dim);
        } else {
            // Four rects are cheaper than a full-desktop odd-even path and are
            // clipped to the dirty region during selection drags.
            const QRect r = hole.normalized();
            p.fillRect(QRect(0, 0, width(), r.top()), dim);
            p.fillRect(QRect(0, r.bottom() + 1, width(), height() - r.bottom() - 1), dim);
            p.fillRect(QRect(0, r.top(), r.left(), r.height()), dim);
            p.fillRect(QRect(r.right() + 1, r.top(), width() - r.right() - 1, r.height()), dim);
        }
    }
    if (m_mode == Adjusting && !m_sel.isNull()) {
        // Marks stay visible outside the frame while the frame is being adjusted;
        // cropped()/composited output still clips them to the final selection.
        paintAnnotations(p, QPointF(0, 0), 1.0, QRectF(event->rect()), false);
        paintAutoMosaicOverlay(p);
        paintAutoMosaicStatus(p);
        if (m_autoMosaic && m_atool == ATool::Mosaic && m_autoDragging && !m_autoDragRect.isNull()) {
            p.setPen(QPen(Bar::accent, 1, Qt::DashLine));
            p.setBrush(QColor(91, 140, 255, 30));
            p.drawRect(m_autoDragRect.normalized());
            p.setBrush(Qt::NoBrush);
        }
    }
    if (m_live && regionItem(m_ann))              // Thin frame shows the dragged region.
        paintMosaicBorder(p, m_ann, false);
    if (m_mode == Adjusting && !m_live
        && (editorActive() || (m_selAnn >= 0 && m_selAnn < m_anns.size()
                              && !m_anns[m_selAnn].mosaicBrush
                              && m_anns[m_selAnn].tool != ATool::Pen
                              && !(m_anns[m_selAnn].tool == ATool::Eraser && !m_anns[m_selAnn].fill)
                              ))) {   // The freshly drawn item keeps its handles until deselected.
        const Annotation ann = editorActive() ? editorAnnotation() : m_anns[m_selAnn];
        if (ann.tool == ATool::Mosaic) {
            paintMosaicBorder(p, ann, true);
        } else {
        p.save();
        if (!qFuzzyIsNull(ann.rot)) p.setTransform(annTransform(ann), true);
        for (const QPoint &anchor : annAnchors(ann))
            paintHandle(p, anchor);
        p.restore();
        }
    }
    if (!m_pinEdit && !hole.isNull()) {            // The region frame stays above the marks.
        p.setPen(QPen(Bar::accent, 2));
        p.setBrush(QBrush(Qt::NoBrush));
        if (m_roundEnabled && m_roundRadius > 0 && !m_embedded) {
            p.setRenderHint(QPainter::Antialiasing);
            p.drawPath(selectionPath(hole));
            p.setRenderHint(QPainter::Antialiasing, false);
        } else {
            p.drawRect(QRectF(hole).adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
    if (m_mode == Adjusting && !m_sel.isNull() && m_hover.isNull() && !m_pinEdit)  // Eight resize handles.
        for (const QRect &h : handleRects(m_sel))
            paintHandle(p, h.center() + QPoint(1, 1));
    if (m_pinEdit && !m_embedded && !m_sel.isNull()) {   // The pin keeps its resting hairline; overlay chrome, never baked.
        p.setPen(QPen(QColor(0, 0, 0, 90), 1));
        p.drawRect(m_sel.adjusted(0, 0, -1, -1));
    }
    paintBars();
}

void CaptureOverlay::mousePressEvent(QMouseEvent *e)
{
    cancelDetection();
    m_nudging = false;
    const bool wasEditing = editorActive();
    const int editingAnn = m_editAnn;
    const Annotation editingState = wasEditing ? editorAnnotation() : Annotation{};
    const Handle editingHandle = wasEditing && editingState.tool == ATool::Text
        && !editingState.text.trimmed().isEmpty() ? annHandleAt(editingState, e->pos()) : H_None;
    if (wasEditing) commitTextEdit();           // Canvas clicks finish input; controls keep it live.
    if (e->button() == Qt::RightButton) {       // Right click ends input, then the ladder.
        m_eatContextMenu = m_live || wasEditing || m_selAnn >= 0 || m_atool != ATool::None;
        stopItemDrag();
        if (m_live) { m_live = false; commitAnnotation(); }
        else if (wasEditing) return;
        else if (m_selAnn >= 0) { m_selAnn = -1; updateToolOptions(); update(); }
        else if (m_atool != ATool::None) return;   // An armed tool stays; reclicking its button disarms it.
        else if (m_mode == Adjusting) {
            if (m_embedded) return;              // No session to reset; annotations simply stay.
            if (m_pinEdit) close(); else resetSelection();
        } else { close(); }
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (editingHandle != H_None) {               // A text corner still resizes after committing the editor.
        const int index = editingAnn >= 0 ? editingAnn : m_anns.size() - 1;
        if (index >= 0 && index < m_anns.size() && m_anns[index].tool == ATool::Text) {
            m_atool = ATool::Text;
            m_selAnn = index;
            m_pressAnn = m_anns[index];
            m_itemDrag = editingHandle;
            m_pressPos = e->pos();
            m_dragStarted = false;
            syncToolButtons();
            if (m_toolOptions && m_toolOptions->findChild<QComboBox *>(QStringLiteral("annotationFont")))
                syncTextOptions();
            updateAnnotationCursor(e->pos());
            update();
            return;
        }
    }
    setFocus();                                   // Any click restores keyboard handling.
    m_pressPos = e->pos();
    m_dragStarted = false;
    m_inspector->hide();
    // The selection frame and everything outside it always resize the region, even while
    // a drawing tool is armed. Marks only receive presses strictly inside the frame.
    const Handle frameHandle = hitHandle(e->pos());
    if (m_mode == Adjusting && !m_embedded && !m_pinEdit && !m_spaceHeld
        && !m_sel.isNull() && frameHandle != H_None
        && (frameHandle != H_Move || onSelectionBorder(e->pos()))) {
        m_selAnn = -1;
        updateToolOptions();
        m_pressWindow = QRect();
        m_drag = frameHandle;
        m_pressSel = m_sel;
        const QRect oldSel = m_sel;
        m_sel = resizeBy(frameHandle, e->pos());
        hideToolBars();
        updateTip();
        updateSelectionRegion(oldSel, m_sel);
        updateInspector();
        return;
    }
    if (m_mode == Adjusting && m_atool == ATool::Mosaic && m_autoMosaic && !m_spaceHeld
        && m_sel.contains(e->pos())) {
        int ann = -1, block = -1;
        if (autoMosaicAt(e->pos(), &ann, &block)) {
            m_autoPressAnn = ann;
            m_autoPressBlock = block;
            m_autoPressMoved = false;
        } else {
            // Starting a manual box retires the hover frame; otherwise it stays
            // painted while the pointer drags somewhere else.
            clearAutoMosaicHover();
            m_autoDragging = true;
            m_autoDragStart = e->pos();
            m_autoDragRect = QRect(e->pos(), QSize(1, 1));
            update(m_autoDragRect.adjusted(-2, -2, 2, 2));
        }
        return;
    }
    if (m_mode == Adjusting && !m_spaceHeld
        && (m_atool == ATool::Mosaic || m_atool == ATool::Eraser || m_atool == ATool::None)) {
        const bool regionTool = m_atool == ATool::None || (m_atool == ATool::Mosaic ? !m_mosaicBrush : m_eraserRect);
        const auto ownRegion = [this](const Annotation &ann) {   // An armed tool only grabs its own regions.
            return regionItem(ann) && (m_atool == ATool::None || ann.tool == m_atool
                || (m_atool == ATool::Eraser && ann.tool == ATool::Mosaic && ann.mosaicSmart));
        };
        if (regionTool && m_selAnn >= 0 && m_selAnn < m_anns.size() && ownRegion(m_anns[m_selAnn])) {
            const Annotation &ann = m_anns[m_selAnn];
            const Handle handle = annHandleAt(ann, e->pos());
            if (handle != H_None || annotationHit(ann, mapToLocal(ann, e->pos()))) {
                m_itemDrag = handle == H_None ? H_Move : handle;
                m_pressAnn = ann;
                updateAnnotationCursor(e->pos());
                return;
            }
        }
        const int hit = regionTool ? annotationAt(e->pos()) : -1;
        if (hit >= 0 && ownRegion(m_anns[hit])) {
            m_selAnn = hit;
            m_pressAnn = m_anns[hit];
            const Handle handle = annHandleAt(m_pressAnn, e->pos());
            m_itemDrag = handle == H_None ? H_Move : handle;
            updateToolOptions();
            updateAnnotationCursor(e->pos());
            update();
            return;
        }
    }
    if (m_selAnn >= 0 && m_selAnn < m_anns.size()
        && (m_anns[m_selAnn].tool == ATool::Mosaic || m_anns[m_selAnn].tool == ATool::Eraser)) {
        m_selAnn = -1;
        updateToolOptions();
        update();
    }
    if (m_mode == Adjusting && !m_spaceHeld
        && (m_atool == ATool::Text || m_atool == ATool::None)) {
        // Text is directly re-openable even after it was locked: click the body to
        // edit, click the control ring to select it for move/resize.
        if (m_selAnn >= 0 && m_selAnn < m_anns.size()
            && m_anns[m_selAnn].tool == ATool::Text) {
            const Handle h = annHandleAt(m_anns[m_selAnn], e->pos());
            if (h != H_None) {
                m_itemDrag = h;
                m_pressAnn = m_anns[m_selAnn];
                updateAnnotationCursor(e->pos());
                return;
            }
        }
        const int hit = textAnnotationAt(e->pos());
        if (hit >= 0) {
            const Annotation &ann = m_anns[hit];
            const Handle h = annHandleAt(ann, e->pos());
            if (hit == m_selAnn && h != H_None) {
                m_itemDrag = h;
                m_pressAnn = ann;
                updateAnnotationCursor(e->pos());
                return;
            }
            if (textInnerHit(ann, e->pos())) {
                beginTextEdit(e->pos(), hit);
                return;
            }
            const bool needStrip = m_atool != ATool::Text || !m_toolOptions
                || !m_toolOptions->findChild<QComboBox *>(QStringLiteral("annotationFont"));
            m_atool = ATool::Text;
            m_selAnn = hit;
            m_pressAnn = ann;
            m_itemDrag = H_Move;
            m_acolor = ann.color;
            m_afont = ann.font;
            m_afamily = ann.family;
            m_abold = ann.bold;
            m_aitalic = ann.italic;
            m_aunderline = ann.underline;
            syncToolButtons();
            if (needStrip) updateToolOptions(); else syncTextOptions();
            updateAnnotationCursor(e->pos());
            update();
            return;
        }
    }
    if (m_mode == Adjusting && m_atool != ATool::None && !m_spaceHeld
        && m_selAnn >= 0 && m_selAnn < m_anns.size()) {   // The fresh item stays adjustable with its tool armed.
        const Handle h = annHandleAt(m_anns[m_selAnn], e->pos());
        if (h != H_None || annotationAt(e->pos()) == m_selAnn) {
            m_itemDrag = h == H_None ? H_Move : h;
            m_pressAnn = m_anns[m_selAnn];
            updateAnnotationCursor(e->pos());
            return;
        }
    }
    if (m_mode == Adjusting && m_atool != ATool::None
        && !m_spaceHeld && hitHandle(e->pos()) == H_Move) { // A tool acts inside the selection.
        if (m_atool == ATool::Text) { beginTextEdit(e->pos()); return; }
        if (m_atool == ATool::Number) {          // Click drops the next sequence number.
            pushUndo();
            Annotation ann;
            ann.tool = ATool::Number;
            ann.color = m_acolor;
            ann.font = m_afont;
            ann.numberStyle = m_numberStyle;
            ann.numberFormat = m_numberFormat;
            const int s = numberDiameter(ann);
            ann.rect = QRect(e->pos() - QPoint(s / 2, s / 2), QSize(s, s));
            m_anns.append(ann);
            normalizeNumbers();
            m_selAnn = m_anns.size() - 1;        // Auto-select fresh annotations.
            syncSizeInput();
            update(ann.rect.adjusted(-8, -8, 8, 8));
            return;
        }
        beginAnnotation(e->pos());
        return;
    }
    if (m_mode == Adjusting && (m_atool == ATool::None || m_spaceHeld)) {   // Editing mode.
        if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
            const Handle h = annHandleAt(m_anns[m_selAnn], e->pos());
            if (h != H_None) {                   // Corner/endpoint handles resize in place.
                m_itemDrag = h;
                m_pressAnn = m_anns[m_selAnn];
                return;
            }
        }
        const int hit = annotationAt(e->pos());
        if (hit >= 0) {                          // Click selects and drags the annotation.
            m_selAnn = hit;
            m_itemDrag = H_Move;
            m_pressAnn = m_anns[hit];
            updateToolOptions();
            update();
            return;
        }
        m_selAnn = -1;
        updateToolOptions();
    }
    if (m_mode == Adjusting) {
        if (m_embedded) {   // Nothing to draw on: the press moves the pin itself.
            m_fwdDrag = true;
            const QPoint local = mapTo(parentWidget(), e->position().toPoint());
            QMouseEvent fwd(e->type(), QPointF(local), e->windowPos(), e->screenPos(),
                            e->button(), e->buttons(), e->modifiers());
            QApplication::sendEvent(parent(), &fwd);
            return;
        }
        if (m_pinEdit) return;   // The pin canvas is fixed: no re-selecting, moving or resizing it.
        m_drag = hitHandle(e->pos());
        m_pressSel = m_sel;
        if (m_drag != H_None && m_drag != H_Move && !m_sel.contains(e->pos())) {
            const QRect oldSel = m_sel;
            m_sel = resizeBy(m_drag, e->pos());  // Outside presses extend the existing edge or corner.
            hideToolBars();
            updateTip();
            updateSelectionRegion(oldSel, m_sel);
        } else if (m_drag == H_None) {
            resetSelection();
            m_drag = H_New;
        }
    } else {
        resetSelection();                       // A fresh press always starts from a clean state.
        m_drag = H_New;
    }
    // A click commits the frame the user can currently see. Re-querying here
    // would discard a wheel-expanded parent and replace it with a different
    // element under the cursor.
    const bool hovered = m_drag == H_New && m_hover.contains(e->pos());
    m_pressWindow = m_drag == H_New ? (hovered ? m_hover : windowAt(e->pos())) : QRect();
    if (m_drag == H_New && !hovered) requestDetection(e->pos());
    updateInspector();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent *e)
{
    if (m_fwdDrag && (e->buttons() & Qt::LeftButton)) {   // A forwarded pin drag continues.
        const QPoint local = mapTo(parentWidget(), e->position().toPoint());
        QMouseEvent fwd(e->type(), QPointF(local), e->windowPos(), e->screenPos(),
                        e->button(), e->buttons(), e->modifiers());
        QApplication::sendEvent(parent(), &fwd);
        return;
    }
    updateInspector();
    const QPoint pos = e->pos();
    if (m_waitingClick) return;
    if (m_autoMosaic && m_atool == ATool::Mosaic && m_mode == Adjusting) {
        if (m_autoDragging) {
            const QRect next = QRect(m_autoDragStart, pos).normalized();
            const QRect dirty = m_autoDragRect.united(next);
            m_autoDragRect = next;
            update(dirty.adjusted(-2, -2, 2, 2));
            return;
        }
        if (m_autoPressAnn >= 0) {
            if (!m_autoPressMoved
                && (pos - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
                // A press on a candidate becomes a box when the pointer actually
                // moves; otherwise a click keeps toggling that candidate.
                m_autoPressMoved = true;
                m_autoPressAnn = -1;
                m_autoPressBlock = -1;
                clearAutoMosaicHover();
                m_autoDragging = true;
                m_autoDragStart = m_pressPos;
                m_autoDragRect = QRect(m_autoDragStart, pos).normalized();
                update(m_autoDragRect.adjusted(-2, -2, 2, 2));
            }
            return;
        }
        // hitHandle() reports H_Move for every interior point; only the resize
        // handles and the border keep their selection duty.
        if (m_sel.contains(pos) && hitHandle(pos) == H_Move) {
            int ann = -1, block = -1;
            const bool hit = autoMosaicAt(pos, &ann, &block);
            if ((hit ? ann : -1) != m_autoHoverAnn || (hit ? block : -1) != m_autoHoverBlock) {
                QRect dirty;
                if (m_autoHoverAnn >= 0 && m_autoHoverAnn < m_anns.size()
                    && m_autoHoverBlock >= 0
                    && m_autoHoverBlock < m_anns[m_autoHoverAnn].autoBlocks.size())
                    dirty |= m_anns[m_autoHoverAnn].autoBlocks[m_autoHoverBlock].rect;
                if (hit) dirty |= m_anns[ann].autoBlocks[block].rect;
                m_autoHoverAnn = hit ? ann : -1;
                m_autoHoverBlock = hit ? block : -1;
                if (!dirty.isNull()) update(dirty.adjusted(-6, -6, 6, 6));
            }
            setCursor(hit ? Qt::PointingHandCursor : Qt::CrossCursor);
            return;
        }
        // Leaving the candidate area, or moving onto the selection border,
        // retires the hover frame instead of leaving it pinned, click-style.
        if (m_autoHoverAnn >= 0) clearAutoMosaicHover();
    }
    if (m_live && m_ann.tool == ATool::Mosaic && m_ann.mosaicBrush) updateAnnotationCursor(pos);
    if (m_live) {                               // Live annotation follows the cursor.
        const QRect dirty = annotationRect(m_ann);
        updateAnnotation(pos, e->modifiers());
        update(dirty.united(annotationRect(m_ann)).adjusted(-24, -24, 24, 24));
        m_sizeBar->hide();                      // Drawing shows no floating text.
        return;
    }
    if (m_itemDrag != H_None) {                  // Move or resize the selected annotation.
        if (m_selAnn < 0 || m_selAnn >= m_anns.size()) { stopItemDrag(); return; }
        if (!m_dragStarted) {
            if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance())
                return;
            m_dragStarted = true;
            pushUndo();                          // Snapshot before the first modification.
            hideToolBars();
            m_inspector->hide();
        }
        const int margin = 24;
        const QRect dirty = rotatedBounds(m_anns[m_selAnn]).adjusted(-margin, -margin, margin, margin);
        applyItemDrag(pos);
        update(dirty.united(rotatedBounds(m_anns[m_selAnn])).adjusted(-margin, -margin, margin, margin));
        return;
    }
    if (m_drag == H_None) {                     // Hover: cursor shape, size hint, window detection.
        if (m_mode == Adjusting) {
            updateAnnotationCursor(pos);
            if (hitHandle(pos) != H_None) {
                cancelDetection();
                clearHover();                   // Repaint only the retired hover rectangle.
            }
            // Hover re-detection only runs before a selection is committed: afterwards
            // the highlight would chase the cursor and visually unlock the selection.
        } else requestDetection(pos);
        return;
    }
    if (m_drag == H_New && !m_dragStarted
        && (pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
    m_dragStarted = true;
    cancelDetection();
    hideToolBars();                              // The status bar steps aside during drags.
    if (!m_hover.isNull())
        clearHover();                            // Repaint only the retired hover rectangle.
    const QRect oldSel = m_sel;
    if (m_drag == H_New) {                      // New selection.
        if (m_mode == Selecting)
            m_mode = Adjusting;                 // Dragging enters adjustment mode.
        m_sel = QRect(m_pressPos, pos).normalized();
    } else {
        m_sel = resizeBy(m_drag, pos);
    }
    updateInspector();
    updateTip();
    updateSelectionRegion(oldSel, m_sel);
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_fwdDrag) {                              // End of a forwarded pin drag.
        m_fwdDrag = false;
        const QPoint local = mapTo(parentWidget(), e->position().toPoint());
        QMouseEvent fwd(e->type(), QPointF(local), e->windowPos(), e->screenPos(),
                        e->button(), e->buttons(), e->modifiers());
        QApplication::sendEvent(parent(), &fwd);
        return;
    }
    if (m_autoMosaic && m_atool == ATool::Mosaic && (m_autoDragging || m_autoPressAnn >= 0)) {
        if (m_autoDragging) {
            m_autoDragging = false;
            const QRect rect = m_autoDragRect.normalized();
            m_autoDragRect = QRect();
            // Clear the dashed preview even when the drag is too small to add a block.
            if (!rect.isNull()) update(rect.adjusted(-4, -4, 4, 4));
            if (rect.width() >= 4 && rect.height() >= 4) addAutoMosaicBlock(rect);
        } else if (!m_autoPressMoved) {
            toggleAutoMosaicBlock(m_autoPressAnn, m_autoPressBlock);
        }
        m_autoPressAnn = -1;
        m_autoPressBlock = -1;
        m_autoPressMoved = false;
        return;
    }
    if (m_live) {                               // Commit the annotation; the tool stays active.
        m_live = false;
        commitAnnotation();
        return;
    }
    if (m_itemDrag != H_None) {                 // A click only selects; a drag edits.
        const bool moved = m_dragStarted;
        m_itemDrag = H_None;
        m_dragStarted = false;
        if (moved) {
            placeToolBars();
            showToolBars();
        }
        update();
        updateAnnotationCursor(e->pos());
        return;
    }
    if (m_drag == H_None)
        return;
    if (m_drag == H_New && !m_dragStarted) {
        m_drag = H_None;
        if (m_detector && m_queryPending) { m_waitingClick = true; return; }
        m_sel = m_pressWindow;
    }
    m_drag = H_None;
    m_sel = m_sel.normalized();
    if (m_sel.width() < 4 || m_sel.height() < 4) {
        if (m_pressWindow.isEmpty()) {          // Plain click on empty space: nothing selected.
            resetSelection();
            return;
        }
        m_sel = m_pressWindow;                  // Click without drag: select the whole window.
    }
    m_mode = Adjusting;
    ensureAutoWatermark();
    clearHover();
    setCursor(Qt::SizeAllCursor);
    placeToolBars();
    showToolBars();
    updateTip();
    updateInspector();
    update();
    if (m_pinAfterDetection) { m_pinAfterDetection = false; pinCapture(); }
}

void CaptureOverlay::wheelEvent(QWheelEvent *e)
{
    if (m_embedded && !editorActive() && m_atool == ATool::None && m_selAnn < 0) {
        // Nothing armed and nothing selected: the wheel belongs to the pin
        // (zoom / opacity), as it did before any annotation existed.
        const QPoint local = mapTo(parentWidget(), e->position().toPoint());
        QWheelEvent fwd(QPointF(local), e->globalPosition(), e->pixelDelta(), e->angleDelta(),
                        e->buttons(), e->modifiers(), e->phase(), e->inverted(), e->source());
        QApplication::sendEvent(parent(), &fwd);
        return;
    }
    if (!e->angleDelta().y()) return;
    if (editorActive()) {                        // Wheel retunes the live text editor.
        editorFontStep(e->angleDelta().y() > 0 ? 1 : -1);
        return;
    }
    if (m_autoMosaic && m_atool == ATool::Mosaic && m_mode == Adjusting) {
        // Wheel tunes the mosaic / blur strength of the covered blocks.
        setAutoMosaicStrength(m_autoMosaicStrength + (e->angleDelta().y() > 0 ? 1 : -1));
        return;
    }
    if (m_mode == Adjusting && m_drag == H_None && m_itemDrag == H_None
        && m_selAnn < 0 && m_atool == ATool::None && !m_sel.isNull()
        && !m_live && !m_pinEdit) {
        // Locked frame: one notch adds or removes one pixel per side around the
        // center. The aspect lock only constrains the other dimension; it never
        // blocks the wheel resize itself.
        const int step = e->angleDelta().y() > 0 ? 1 : -1;
        const QPoint center = m_sel.center();
        QRect next = m_sel.adjusted(-step, -step, step, step);
        if (next.width() >= 2 && next.height() >= 2) {
            if (m_lockAspect && m_aspect > 0)
                next.setHeight(qMax(2, qRound(next.width() / m_aspect)));
            next.moveCenter(center);
            next = next.intersected(rect());
            if (next.width() >= 2 && next.height() >= 2) {
                cancelDetection();
                m_sel = next;
                placeToolBars();
                showToolBars();
                updateTip();
                update();
                updateInspector();
            }
        }
        return;
    }
    if (m_mode == Adjusting && m_drag == H_None && m_itemDrag == H_None) {
        tuneWidth(e->angleDelta().y() > 0 ? 1 : -1, e->position().toPoint());
        if (m_selAnn >= 0 || m_atool != ATool::None) return;
    }
    if (m_drag != H_None || m_mode != Selecting || !m_detectionOptions.elements) return;
    const QPoint point = e->position().toPoint();
    if (e->angleDelta().y() > 0) {
        if (m_hoverHistory.isEmpty() && !m_hover.isNull()) m_hoverHistory.append(m_hover);
        requestDetection(point, m_hover);
    } else if (e->angleDelta().y() < 0) {
        cancelDetection();
        if (m_hoverHistory.size() > 1) {
            m_hoverHistory.removeLast();
            setHoverRect(m_hoverHistory.last(), true);
            updateTip();
        } else requestDetection(point);
    }
}

void CaptureOverlay::closeEvent(QCloseEvent *e)
{
    if (m_colorDialog) m_colorDialog->close();
    if (m_customSizePopup) m_customSizePopup->close();
    cancelDetection();
    cancelAutoMosaicDetection();
    if (m_detector) m_detector->stop();
    QWidget::closeEvent(e);
}

void CaptureOverlay::contextMenuEvent(QContextMenuEvent *e)
{
    // A right click that finished or deselected an item, or hit an armed tool, opens no menu;
    // otherwise the embedding pin shows its own.
    if (m_eatContextMenu || m_atool != ATool::None) e->accept(); else e->ignore();
    m_eatContextMenu = false;
}

void CaptureOverlay::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (editorActive()) return;                  // A text double-click never pins the capture.
    if (m_mode == Adjusting && (m_atool == ATool::None || m_atool == ATool::Text)) {
        const int hit = textAnnotationAt(e->pos());  // Double-click also reopens locked text.
        if (hit >= 0) {
            m_selAnn = hit;
            m_itemDrag = H_None;
            beginTextEdit(e->pos(), hit);
            return;
        }
    }
    if (m_queryPending && (m_waitingClick || m_drag == H_New))
        m_pinAfterDetection = true;
    else if (m_embedded) {
        const QPoint local = mapTo(parentWidget(), e->position().toPoint());
        QMouseEvent fwd(e->type(), QPointF(local), e->windowPos(), e->screenPos(),
                        e->button(), e->buttons(), e->modifiers());
        QApplication::sendEvent(parent(), &fwd);   // The pin keeps its double-click actions.
    }
    else if (m_mode == Adjusting && m_atool == ATool::None && m_sel.contains(e->pos()))
        pinCapture();
}

void CaptureOverlay::keyPressEvent(QKeyEvent *e)
{
    if (editorActive()) {                        // Unhandled editing keys must not trigger capture actions.
        if (e->key() == Qt::Key_Escape) commitTextEdit();
        else if (e->modifiers() & Qt::ControlModifier) {
            if (e->key() == Qt::Key_S) saveCapture();
            else if (e->key() == Qt::Key_T) pinCapture();
            else if (e->key() == Qt::Key_Z) {
                if (e->modifiers() & Qt::ShiftModifier) redoAnnotation();
                else undoAnnotation();
            } else if (e->key() == Qt::Key_Y) redoAnnotation();
        }
        return;
    }
    if (!m_live && m_itemDrag == H_None && m_drag == H_None
        && e->key() == Qt::Key_A && e->modifiers() == Qt::ControlModifier) {
        const QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
        const QRect current = localScreenRect((screen ? screen : QGuiApplication::primaryScreen())->geometry());
        cancelDetection();
        m_sel = m_sel == current ? rect() : current;
        if (m_lockAspect && m_sel.height() > 0) m_aspect = qreal(m_sel.width()) / m_sel.height();
        m_mode = Adjusting;
        ensureAutoWatermark();
        clearHover();
        placeToolBars(); showToolBars(); updateTip(); update();
        updateInspector();
        return;
    }
    const QPoint delta = keyDelta(e->key());
    const bool arrow = e->key() >= Qt::Key_Left && e->key() <= Qt::Key_Down;
    const Qt::KeyboardModifiers movementModifiers = e->modifiers() & ~Qt::KeypadModifier;
    if (!delta.isNull() && m_selAnn < 0 && !m_live && m_itemDrag == H_None) {
        if (movementModifiers == Qt::NoModifier && (!arrow || m_mode == Selecting)) {
            const QRect bounds = QGuiApplication::primaryScreen()->virtualGeometry();
            const QPoint next = QCursor::pos() + delta;
            QCursor::setPos(qBound(bounds.left(),next.x(),bounds.right()),
                            qBound(bounds.top(),next.y(),bounds.bottom()));
            if (m_drag != H_None) {
                const QPoint local = windowHandle() ? windowHandle()->mapFromGlobal(QPointF(QCursor::pos())).toPoint()
                                                    : mapFromGlobal(QCursor::pos());
                QMouseEvent move(QEvent::MouseMove,local,QCursor::pos(),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
                mouseMoveEvent(&move);
            } else updateInspector();
            return;
        }
        if (arrow && m_mode == Adjusting && m_drag == H_None && !m_sel.isEmpty()
            && movementModifiers == Qt::NoModifier && !m_pinEdit) {   // The pin canvas stays put.
            cancelDetection();
            m_sel.translate(delta);
            m_sel.moveTo(qBound(0,m_sel.x(),width()-m_sel.width()),qBound(0,m_sel.y(),height()-m_sel.height()));
            placeToolBars(); showToolBars(); updateTip(); update();
            updateInspector();
            return;
        }
    }
    if ((e->modifiers() & Qt::ControlModifier)
        && !(e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier))) {
        if (e->key() == Qt::Key_T && m_mode == Adjusting && !m_live && !editorActive()
            && !m_embedded) {                      // Embedded canvas has no capture outputs.
            pinCapture();                        // Pin to screen (Ctrl+T).
            return;
        }
    }
    switch (e->key()) {
    case Qt::Key_F5:
        if (!m_embedded && !m_pinEdit) refreshScreenshot();
        break;
    case Qt::Key_Escape:
        if (m_live) { m_live = false; endAnnotationDrag(); }
        else if (editorActive()) commitTextEdit();
        else if (m_embedded) {                    // The pin owns the rest: hide its bar, then close.
            QKeyEvent fwd(e->type(), e->key(), e->modifiers(), e->text(), e->isAutoRepeat(), 1);
            QApplication::sendEvent(parent(), &fwd);
        }
        else if (m_atool != ATool::None) setToolNone();
        else if (m_selAnn >= 0) { stopItemDrag(); m_selAnn = -1; updateToolOptions(); update(); }
        else close();
        break;
    case Qt::Key_Space:
        if (!e->isAutoRepeat()) m_spaceHeld = true;   // Space turns drags into moves.
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        if (!m_live && !editorActive() && m_mode == Adjusting)
            tuneWidth(1, mapFromGlobal(QCursor::pos()));
        break;
    case Qt::Key_Minus:
        if (!m_live && !editorActive() && m_mode == Adjusting)
            tuneWidth(-1, mapFromGlobal(QCursor::pos()));
        break;
    case Qt::Key_F:                              // Fill mode for shapes and the selection.
        if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat()
            && !m_live && !editorActive() && m_mode == Adjusting) {
            const ATool tool = optionsTool();
            if (tool == ATool::Rect || tool == ATool::Ellipse || tool == ATool::Marker)
                toggleFill();                    // Same toggle as the strip's fill button.
        }
        break;
    case Qt::Key_Delete:
        if (m_selAnn >= 0) deleteSelected();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_mode == Adjusting && !m_live && !m_embedded) pinCapture();
        break;
    case Qt::Key_C:
        if (e->modifiers() & Qt::ControlModifier) {
            // Embedded canvas has no capture outputs, same as Ctrl+T above.
            if (m_mode == Adjusting && !m_live && !m_embedded) copyCapture();
        } else if (!(e->modifiers() & (Qt::AltModifier | Qt::MetaModifier))
                   && m_inspector->isVisible() && !m_inspector->colorText().isEmpty()) {
            if (!copyTextToClipboard(m_inspector->colorText()))
                showOutputError(tr("复制失败，剪贴板可能被占用，请重试"));
            else m_sizeBar->hide();
        }
        break;
    case Qt::Key_Z:
        if (e->modifiers() & Qt::ControlModifier) {
            if (e->modifiers() & Qt::ShiftModifier) redoAnnotation();
            else undoAnnotation();
        }
        break;
    case Qt::Key_Y:
        if (e->modifiers() & Qt::ControlModifier) redoAnnotation();
        break;
    case Qt::Key_Shift:
        if (!e->isAutoRepeat()) m_inspector->toggleFormat();
        break;
    case Qt::Key_P:
        if (!e->isAutoRepeat()) m_inspector->toggleCoordinates();
        break;
    case Qt::Key_S:
        if (e->modifiers() & Qt::ControlModifier && !m_live && !m_embedded) saveCapture();
        else if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat())
            nudgeSelected(Qt::Key_S);            // WASD nudges the selection.
        break;
    default:
        if (e->modifiers() == Qt::NoModifier && !e->isAutoRepeat()
            && !m_live && !editorActive()) {
            if (m_selAnn >= 0 && m_selAnn < m_anns.size()) {
                nudgeSelected(e->key());        // Arrows / WASD move the selected item.
                return;
            }
            const int key = e->key();           // Single letters arm annotation tools.
            const ATool tool = key == Qt::Key_R ? ATool::Rect
                : key == Qt::Key_O ? ATool::Ellipse
                : key == Qt::Key_A ? ATool::Arrow
                : key == Qt::Key_P ? ATool::Pen
                : key == Qt::Key_H ? ATool::Marker
                : key == Qt::Key_T ? ATool::Text
                : key == Qt::Key_N ? ATool::Number
                : key == Qt::Key_M ? ATool::Mosaic
                : key == Qt::Key_E ? ATool::Eraser
                : key == Qt::Key_W ? ATool::Watermark : ATool::None;
            if (tool != ATool::None)
                selectTool(tool == m_atool ? ATool::None : tool);   // Same key toggles off.
        }
        break;
    }
}

void CaptureOverlay::keyReleaseEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space) m_spaceHeld = false;
}

void CaptureOverlay::startDetector()
{
    qRegisterMetaType<WindowTargets>("WindowTargets");
    m_detectionWatchdog.stop();
    m_refineTimer.stop();
    m_hoverPaintTimer.stop();
    m_refineScheduled = false;
    m_refineTried = false;
    m_queryId = 0;
    m_detector = WindowDetector::create(reinterpret_cast<HWND>(winId()), m_detectionOptions, m_initialTargets);
    if (!m_detector) {
        m_queryPending = false;
        if (!m_initialTargets.isEmpty()) setWindowTargets(m_initialTargets);
        else setWindowTargets(detectWindowRects(reinterpret_cast<HWND>(winId()), m_detectionOptions.excludedPrograms));
        return;
    }
    const QPointer<WindowDetector> detector = m_detector;
    connect(detector, &QThread::finished, detector, &QObject::deleteLater);
    connect(detector, &WindowDetector::gotTargets, this, [this, detector](const WindowTargets &targets, quint64 id) {
        // Arrives in query order from one worker; the newest table lands last.
        Q_UNUSED(id);
        if (detector == m_detector && isVisible()) setWindowTargets(targets);
    }, Qt::QueuedConnection);
    connect(detector, &WindowDetector::gotRect, this, [this, detector](const QRect &rect, quint64 id) {
        if (detector == m_detector) {
            if (m_queryPending) m_detectionWatchdog.start(kDetectionWatchdogMs);
            handleDetectedRect(rect, id);
        }
    }, Qt::QueuedConnection);
    detector->start(QThread::LowestPriority);
}

void CaptureOverlay::setWindowTargets(const WindowTargets &windows)
{
    m_windowRects.clear();
    m_nativeRects.clear();
    for (const DetectedWindow &win : windows) {
        QRect logical = logicalFromNative(m_frames, win.native);
        const QScreen *center = QGuiApplication::screenAt(logical.center());
        bool crosses = false;
        for (const QScreen *screen : QGuiApplication::screens())
            if (screen != center && screen->geometry().intersects(logical)) crosses = true;
        if (center && (win.maximized || !crosses)) logical &= center->geometry();
        if (logical.isEmpty()) continue;
        DetectedWindow target(localScreenRect(logical));
        target.hwnd = win.hwnd;
        target.program = win.program;
        m_nativeRects.insert(quintptr(win.hwnd), win.native);
        for (const QRect &child : win.nativeChildren) {
            const QRect clipped = logical.intersected(logicalFromNative(m_frames, child));
            if (!clipped.isEmpty()) target.nativeChildren.append(localScreenRect(clipped));
        }
        m_windowRects.append(target);
    }
}

QRect CaptureOverlay::localScreenRect(const QRect &global) const
{
    if (global.isEmpty()) return {};
    if (!windowHandle()) return QRect(mapFromGlobal(global.topLeft()), global.size());   // Embedded child canvas.
    const QPointF top = windowHandle()->mapFromGlobal(QPointF(global.topLeft()));
    const QPointF end = windowHandle()->mapFromGlobal(QPointF(global.x() + global.width(), global.y() + global.height()));
    return QRect(QPoint(qRound(top.x()), qRound(top.y())),
                 QSize(qRound(end.x()) - qRound(top.x()), qRound(end.y()) - qRound(top.y())));
}

QRect CaptureOverlay::globalScreenRect(const QRect &local) const
{
    if (local.isEmpty()) return {};
    if (!windowHandle()) return QRect(mapToGlobal(local.topLeft()), local.size());   // Embedded child canvas.
    const QPointF top = windowHandle()->mapToGlobal(QPointF(local.topLeft()));
    const QPointF end = windowHandle()->mapToGlobal(QPointF(local.x() + local.width(), local.y() + local.height()));
    return QRect(QPoint(qRound(top.x()), qRound(top.y())),
                 QSize(qRound(end.x()) - qRound(top.x()), qRound(end.y()) - qRound(top.y())));
}

QRect CaptureOverlay::nativeDetectionRect(const QRect &local) const
{
    if (local.isEmpty()) return {};
    const QRect global = globalScreenRect(local);
    for (const ScreenFrame &frame : m_frames)
        if (frame.logicalRect.contains(global.center())) {
            const double sx = double(frame.pixelRect.width()) / frame.logicalRect.width();
            const double sy = double(frame.pixelRect.height()) / frame.logicalRect.height();
            return QRect(qRound(frame.pixelRect.x() + (global.x() - frame.logicalRect.x()) * sx),
                         qRound(frame.pixelRect.y() + (global.y() - frame.logicalRect.y()) * sy),
                         qRound(global.width() * sx), qRound(global.height() * sy));
        }
    return {};
}

void CaptureOverlay::cancelDetection()
{
    if (m_detector) m_detector->cancel();
    m_detectionWatchdog.stop();
    m_refineTimer.stop();
    m_hoverPaintTimer.stop();
    m_refineScheduled = false;
    m_refineTried = false;
    m_queryId = 0;
    m_queryPending = m_waitingClick = false;
    m_pinAfterDetection = false;
}

void CaptureOverlay::requestDetection(const QPoint &point, const QRect &previous, bool force)
{
    if (!force && m_drag == H_None && point == m_detectionPoint && previous.isEmpty() && m_queryId != 0) return;
    if (point != m_detectionPoint) {
        m_refineTimer.stop();
        m_refineScheduled = false;
        m_refineTried = false;
    }
    if (previous.isEmpty()) m_hoverHistory.clear();
    m_detectionPoint = point;
    if (!m_detector) { setHoverRect(windowAt(point), false); updateTip(); return; }
    const QRect nativePoint = nativeDetectionRect(QRect(point, QSize(1, 1)));
    if (nativePoint.isEmpty()) return;
    m_queryPending = true;
    m_expanding = !previous.isEmpty();
    m_queryId = m_detector->query(nativePoint.topLeft(), nativeDetectionRect(previous));
    if (!m_detectionWatchdog.isActive()) m_detectionWatchdog.start(kDetectionWatchdogMs);
}

void CaptureOverlay::refineDetection()
{
    if (!m_refineScheduled) return;
    m_refineScheduled = false;
    if (!m_detectionOptions.elements || m_mode != Selecting || m_waitingClick
        || m_drag != H_None || m_expanding || m_queryPending || m_hover.isNull()
        || !isVisible() || m_detectionPoint != m_refinePoint) return;
    QRect window;
    for (const DetectedWindow &win : m_windowRects)
        if (win.native.contains(m_detectionPoint)) { window = win.native; break; }
    if (window.isEmpty() || rectArea(m_hover) * 4 < rectArea(window) * 3) return;
    // Chromium builds its accessibility tree after the first hover. Re-query
    // the same point once so a container result can refine without mouse motion.
    m_refineTried = true;
    requestDetection(m_refinePoint, {}, true);
}

void CaptureOverlay::recoverDetector()
{
    const QPointer<WindowDetector> detector = m_detector;
    const quint64 id = m_queryId;
    if (!detector || !m_queryPending || !isVisible()) return;
    if (m_initialTargets.isEmpty())
        setWindowTargets(detectWindowRects(reinterpret_cast<HWND>(winId()), m_detectionOptions.excludedPrograms));
    for (const DetectedWindow &win : m_windowRects)
        if (win.native.contains(m_detectionPoint) && !win.program.isEmpty()) {
            // One slow query is load, not a crash: degrade this session only,
            // and never downgrade element walks for the whole capture.
            if (windowHung(win.hwnd)
                && !m_detectionOptions.disabledUiPrograms.contains(win.program, Qt::CaseInsensitive)) {
                m_detectionOptions.disabledUiPrograms.append(win.program);
            }
            break;
        }
    // A slow provider is not a hung one. Keep the element already under the
    // cursor so one over-budget walk cannot widen the highlight to the page or
    // persist a UI downgrade for the rest of the session.
    const QRect lastHover = m_hover;
    m_queryPending = false;
    m_detectionWatchdog.stop();
    detector->stop();
    if (!lastHover.isNull() && lastHover.contains(m_detectionPoint))
        handleDetectedRect(nativeDetectionRect(lastHover), id);
    else
        handleDetectedRect(nativeDetectionRect(windowAt(m_detectionPoint)), id);
    if (isVisible()) startDetector();
    else m_detector = nullptr;
}

void CaptureOverlay::updateHoverRegion(const QRect &oldRect, const QRect &newRect)
{
    QRect region;
    if (!newRect.isNull()) region = newRect.adjusted(-2, -2, 2, 2);
    if (!oldRect.isNull()) region |= oldRect.adjusted(-2, -2, 2, 2);
    if (!region.isNull()) update(region);
}

void CaptureOverlay::updateSelectionRegion(const QRect &oldSel, const QRect &newSel)
{
    // Selection painting is stateless, so the union reproduces both the retired
    // and current frame without repainting the whole virtual desktop.
    const int margin = 16;
    QRect region;
    if (!oldSel.isNull()) region |= oldSel.adjusted(-margin, -margin, margin, margin);
    if (!newSel.isNull()) region |= newSel.adjusted(-margin, -margin, margin, margin);
    if (!region.isNull()) update(region);
}

void CaptureOverlay::applyHoverPaint(bool animate)
{
    const QRect target = m_hover;
    const bool running = m_hoverAnimation
        && m_hoverAnimation->state() == QAbstractAnimation::Running;
    if (m_hoverPaint == target) {
        // The target can return to the pixel currently shown while an older
        // animation is still heading somewhere else; stop it instead of letting
        // it drift away from the real target.
        if (running && m_hoverAnimation->endValue().toRectF().toRect() != target)
            m_hoverAnimation->stop();
        return;
    }
    const QRect old = m_hoverPaint;
    if (!m_hoverAnimation) {
        m_hoverAnimation = new QVariantAnimation(this);
        m_hoverAnimation->setDuration(88);                     // Reference mask transition.
        m_hoverAnimation->setEasingCurve(QEasingCurve::OutCirc);
        connect(m_hoverAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            const QRect next = value.toRectF().toRect();
            const QRect previous = m_hoverPaint;
            m_hoverPaint = next;
            updateHoverRegion(previous, next);
        });
        connect(m_hoverAnimation, &QAbstractAnimation::finished, this, [this] {
            const QRect previous = m_hoverPaint;
            m_hoverPaint = m_hoverAnimation->endValue().toRectF().toRect();
            updateHoverRegion(previous, m_hoverPaint);
        });
    }
    if (running) m_hoverAnimation->stop();
    // The first target appears immediately; only changes between real elements glide.
    if (!animate || target.isNull() || old.isNull()) {
        m_hoverPaint = target;
        updateHoverRegion(old, m_hoverPaint);
        if (old.isNull() || target.isNull()) update();   // Paint the dim mask the first time, not only the hole.
        updateTip();
        return;
    }
    // Every accepted result retargets from the current visible rect, so a
    // burst of detector replies never jumps the hole back to an older target.
    m_hoverAnimation->setStartValue(QRectF(old));
    m_hoverAnimation->setEndValue(QRectF(target));
    m_hoverAnimation->start();
    updateTip();
}

void CaptureOverlay::queueHoverPaint()
{
    if (m_hoverPaint.isNull()) {
        applyHoverPaint(false);
        return;
    }
    // A fast detector can answer several times per display frame. Restarting
    // the 88 ms tween for every reply resets it before it ever ticks, so keep
    // the newest target and begin at most one transition per frame.
    if (!m_hoverPaintTimer.isActive()) m_hoverPaintTimer.start();
}

void CaptureOverlay::setHoverRect(const QRect &rect, bool animate)
{
    m_hoverPaintTimer.stop();
    m_hover = rect;
    applyHoverPaint(animate);
}

void CaptureOverlay::clearHover()
{
    if (m_hover.isNull() && m_hoverPaint.isNull()) return;
    m_hoverPaintTimer.stop();
    if (m_hoverAnimation) m_hoverAnimation->stop();
    const QRect old = m_hoverPaint.isNull() ? m_hover : m_hoverPaint;
    m_hover = {};
    m_hoverPaint = {};
    updateHoverRegion(old, QRect());
}

void CaptureOverlay::handleDetectedRect(const QRect &native, quint64 id)
{
    if (!id || !isVisible()) return;
    const auto contentFor = [this](const QRect &rawNative) {
        for (const DetectedWindow &win : m_windowRects)
            if (win.hwnd && m_nativeRects.value(quintptr(win.hwnd)) == rawNative)
                return contentRectFor(win, m_detectionPoint);
        return QRect();
    };
    const auto keepsElement = [this](const QRect &candidate) {
        // Slow providers can return an ancestor after a smaller element was
        // already resolved. Keep the element while the cursor stays inside it;
        // widening is user-driven (wheel) or follows the cursor leaving.
        return !m_expanding && !m_hover.isNull() && m_hover.contains(m_detectionPoint)
            && candidate != m_hover && candidate.contains(m_hover)
            && rectArea(candidate) > rectArea(m_hover);
    };
    if (id != m_queryId) return;      // A newer query owns the hover; old points must not paint.
    m_queryPending = false;
    m_detectionWatchdog.stop();
    const QRect content = contentFor(native);
    QRect detected = content.isEmpty()
        ? localScreenRect(logicalFromNative(m_frames, native)) : content;
    if (native.isEmpty() && m_detectionOptions.windows) {
        detected = windowAt(m_detectionPoint);
        if (detected.isEmpty()) {
            const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_detectionPoint));
            if (screen) detected = localScreenRect(screen->availableGeometry());
            if (!detected.contains(m_detectionPoint)) detected = QRect(m_detectionPoint, QSize(1, 1));
        }
    }
    if (m_expanding && detected == m_hover) {
        const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_detectionPoint));
        const QRect available = screen ? localScreenRect(screen->availableGeometry()) : QRect();
        if (available != m_hover && available.contains(m_hover)) detected = available;
    }
    if (!detected.isEmpty() && !detected.contains(m_detectionPoint)) return;
    if (keepsElement(detected)) return;
    if (m_drag == H_New || m_waitingClick) {
        m_pressWindow = detected;
        if (m_waitingClick) {
            m_waitingClick = false;
            m_sel = detected;
            if (m_sel.width() >= 4 && m_sel.height() >= 4) {
                m_mode = Adjusting;
                ensureAutoWatermark();
                showToolBars();
                placeToolBars();
                if (m_pinAfterDetection) { m_pinAfterDetection = false; pinCapture(); return; }
            } else resetSelection();
            clearHover();
        }
    } else if (m_drag == H_None) {
        if (m_expanding) {
            setHoverRect(detected, true);
        } else {
            m_hover = detected;
            queueHoverPaint();
        }
        if (m_expanding && (m_hoverHistory.isEmpty() || m_hoverHistory.last() != detected))
            m_hoverHistory.append(detected);
        m_refineTimer.stop();
        m_refineScheduled = false;
        if (!m_expanding && !m_refineTried && m_detectionOptions.elements) {
            for (const DetectedWindow &win : m_windowRects)
                if (win.native.contains(m_detectionPoint)) {
                    if (rectArea(detected) * 4 >= rectArea(win.native) * 3) {
                        m_refinePoint = m_detectionPoint;
                        m_refineScheduled = true;
                        m_refineTimer.start(180);
                    }
                    break;
                }
        }
        return;
    }
    updateTip();
    updateInspector();
    update();
}

void CaptureOverlay::updateInspector()
{
    const QPoint local = mapFromGlobal(QCursor::pos());
    if (!isVisible() || m_embedded || editorActive() || m_live || m_itemDrag != H_None
        || m_drag == H_Move
        || (m_mode == Adjusting && m_drag == H_None
            && (m_atool != ATool::None || m_selAnn >= 0 || !m_sel.contains(local)))
        || overToolChrome(local)) {
        m_inspector->hide();
        return;
    }
    POINT cursor;
    if (GetCursorPos(&cursor)) {
        const QPoint pixel(cursor.x, cursor.y);
        for (const ScreenFrame &frame : m_frames) {
            const QRect bounds = localScreenRect(frame.logicalRect);
            const QRect hover = !m_hoverPaint.isNull() ? m_hoverPaint : m_hover;
            const QRect hole = !hover.isNull() ? hover : m_sel;
            const qreal sx = qreal(frame.pixelRect.width()) / bounds.width();
            const qreal sy = qreal(frame.pixelRect.height()) / bounds.height();
            const QRectF selectionPixels((hole.x() - bounds.x()) * sx, (hole.y() - bounds.y()) * sy,
                                         hole.width() * sx, hole.height() * sy);
            if (!m_inspector->setSample(frame, pixel, hole.isEmpty() ? QRectF() : selectionPixels))
                continue;
            m_inspector->move(inspectorPosition(local, m_inspector->size(), bounds));
            m_inspector->show();
            m_inspector->raise();
            m_sizeBar->raise();
            return;
        }
    }
    m_inspector->hide();
}

QPoint CaptureOverlay::inspectorPosition(const QPoint &cursor, const QSize &size, const QRect &bounds)
{
    int x = cursor.x() + 8, y = cursor.y() + 20;
    if (x + size.width() > bounds.right() + 1) x = cursor.x() - size.width() - 8;
    if (y + size.height() > bounds.bottom() + 1) y = cursor.y() - size.height() - 20;
    return QPoint(qBound(bounds.left(), x, qMax(bounds.left(), bounds.right() + 1 - size.width())),
                  qBound(bounds.top(), y, qMax(bounds.top(), bounds.bottom() + 1 - size.height())));
}

void CaptureOverlay::resetSelection()
{
    stopItemDrag();
    if (m_colorDialog) m_colorDialog->close();
    m_lockAspect = false;
    if (m_sizeLockButton) {
        const QSignalBlocker block(m_sizeLockButton);
        m_sizeLockButton->setChecked(false);
    }
    m_mode = Selecting;                         // m_hover is kept: the next mouse move refreshes it.
    m_sel = QRect();
    m_drag = H_None;
    for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
        if (bar) bar->hide();
    m_toolOptions->hide();
    m_sizeBar->hide();
    setCursor(Qt::CrossCursor);
    updateInspector();
    update();
}

QRect CaptureOverlay::windowAt(const QPoint &p) const
{
    if (!m_detectionOptions.windows) return {};
    for (const DetectedWindow &w : m_windowRects) {   // Top-down z-order: first hit wins.
        if (!w.native.contains(p))
            continue;
        QRect hit = m_detectionOptions.elements ? ::childAt(w, p) : w.native;
        if (hit == w.native) {
            const QRect content = contentRectFor(w, p);
            if (!content.isEmpty()) hit = content;
        }
        return hit;
    }
    return QRect();
}

QRect CaptureOverlay::contentRectFor(const DetectedWindow &win, const QPoint &point) const
{
    if (!win.hwnd || win.native.isEmpty()) return win.native;
    const QRect raw = m_nativeRects.value(quintptr(win.hwnd));
    if (raw.isEmpty()) return win.native;
    const quintptr key = quintptr(win.hwnd);
    const auto cached = m_contentCache.constFind(key);
    if (cached != m_contentCache.constEnd() && cached->first == raw)
        return cached->second.contains(point) ? cached->second : win.native;
    const QRect trimmedNative = trimLayeredContent(win.hwnd, raw);
    QRect trimmed = trimmedNative == raw ? win.native
        : localScreenRect(logicalFromNative(m_frames, trimmedNative));
    if (trimmed.isEmpty() || !trimmed.contains(point)) trimmed = win.native;
    if (m_contentCache.size() > 256) m_contentCache.clear();
    m_contentCache.insert(key, qMakePair(raw, trimmed));
    return trimmed;
}

QRect CaptureOverlay::trimLayeredContent(HWND hwnd, const QRect &native) const
{
    if (!hwnd || native.isEmpty() || !isVisible() || IsIconic(hwnd)) return native;
    if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)) return native;
    const int maxInset = qMin(96, qMin(native.width() / 3, native.height() / 3));
    if (maxInset < 1) return native;
    HWND overlay = reinterpret_cast<HWND>(winId());
    const LONG_PTR overlayEx = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
    // Probe the OS hit-test surface with the overlay click-through. The style is
    // restored synchronously before the event loop can deliver another click.
    SetWindowLongPtrW(overlay, GWL_EXSTYLE, overlayEx | WS_EX_TRANSPARENT);
    const auto hit = [hwnd, overlay](int x, int y) {
        POINT point{x, y};
        HWND h = WindowFromPoint(point);
        if (!h || h == overlay || GetAncestor(h, GA_ROOT) == overlay) return false;
        return h == hwnd || IsChild(hwnd, h);
    };
    const auto firstHit = [&](bool horizontal, int fixed, int start, int step) {
        for (int i = 0; i <= maxInset; ++i) {
            const int v = start + i * step;
            if (hit(horizontal ? v : fixed, horizontal ? fixed : v)) return v;
        }
        return -1;
    };
    int left = native.left(), top = native.top();
    int right = native.right(), bottom = native.bottom();
    const int cx = native.left() + native.width() / 2;
    const int cy = native.top() + native.height() / 2;
    const int ySamples[] = {cy, native.top() + native.height() / 4,
                            native.top() + native.height() * 3 / 4};
    const int xSamples[] = {cx, native.left() + native.width() / 4,
                            native.left() + native.width() * 3 / 4};
    int found = -1;
    for (int y : ySamples) {
        const int v = firstHit(true, y, native.left(), 1);
        if (v >= 0) found = found < 0 ? v : qMin(found, v);
    }
    if (found >= 0) left = found;
    found = -1;
    for (int y : ySamples) {
        const int v = firstHit(true, y, native.right(), -1);
        if (v >= 0) found = found < 0 ? v : qMax(found, v);
    }
    if (found >= 0) right = found;
    found = -1;
    for (int x : xSamples) {
        const int v = firstHit(false, x, native.top(), 1);
        if (v >= 0) found = found < 0 ? v : qMin(found, v);
    }
    if (found >= 0) top = found;
    found = -1;
    for (int x : xSamples) {
        const int v = firstHit(false, x, native.bottom(), -1);
        if (v >= 0) found = found < 0 ? v : qMax(found, v);
    }
    if (found >= 0) bottom = found;
    SetWindowLongPtrW(overlay, GWL_EXSTYLE, overlayEx);
    QRect trimmed(left, top, right - left + 1, bottom - top + 1);
    if (trimmed.width() < 8 || trimmed.height() < 8 || !native.contains(trimmed))
        return native;
    if (trimmed == native) return native;   // Fully hit-testable: no shadow rim to refine.
    // Hit-testing already removed the invisible rim. The remaining self-drawn
    // shadow is a smooth gradient ending at the image's first strong pixel
    // step; use the frozen frame to find that step.
    for (const ScreenFrame &frame : m_frames) {
        if (frame.image.isNull() || !frame.pixelRect.contains(trimmed.center())) continue;
        const QRect refined = pixelContentRect(frame.image, frame.pixelRect, trimmed);
        if (refined.width() >= 8 && refined.height() >= 8 && trimmed.contains(refined))
            trimmed = refined;
        break;
    }
    return trimmed;
}

QVector<QRect> CaptureOverlay::handleRects(const QRect &r)
{
    const int s = 8;
    const QPoint c = r.center();
    return {
        QRect(r.topLeft() - QPoint(s/2, s/2), QSize(s, s)),
        QRect(r.topRight() - QPoint(s/2 - 1, s/2), QSize(s, s)),
        QRect(r.bottomLeft() - QPoint(s/2, s/2 - 1), QSize(s, s)),
        QRect(r.bottomRight() - QPoint(s/2 - 1, s/2 - 1), QSize(s, s)),
        QRect(r.left() - s/2, c.y() - s/2, s, s),
        QRect(r.right() - s/2 + 1, c.y() - s/2, s, s),
        QRect(c.x() - s/2, r.top() - s/2, s, s),
        QRect(c.x() - s/2, r.bottom() - s/2 + 1, s, s),
    };
}

bool CaptureOverlay::onSelectionBorder(const QPoint &p) const
{
    if (m_sel.isEmpty()) return false;
    // The frame, a one-pixel inner ring and everything outside it are selection
    // controls; only the interior belongs to marks.
    if (!m_sel.adjusted(2, 2, -2, -2).contains(p)) return true;
    return m_roundEnabled && m_roundRadius > 0 && !m_embedded && !m_pinEdit
        && !selectionPath(m_sel).contains(QPointF(p));
}

CaptureOverlay::Handle CaptureOverlay::hitHandle(const QPoint &p) const
{
    if (m_mode != Adjusting || m_sel.isEmpty())
        return H_None;
    const QVector<QRect> hs = handleRects(m_sel);
    for (int i = 0; i < hs.size(); ++i)
        if (hs[i].adjusted(-2, -2, 2, 2).contains(p))
            return Handle(H_TL + i);
    if (onSelectionBorder(p)) {
        if (p.x() < m_sel.left()) return p.y() < m_sel.top() ? H_TL : p.y() > m_sel.bottom() ? H_BL : H_L;
        if (p.x() > m_sel.right()) return p.y() < m_sel.top() ? H_TR : p.y() > m_sel.bottom() ? H_BR : H_R;
        if (p.y() < m_sel.top()) return H_T;
        if (p.y() > m_sel.bottom()) return H_B;
        if (p.x() <= m_sel.left() + 1) return H_L;
        if (p.x() >= m_sel.right() - 1) return H_R;
        if (p.y() <= m_sel.top() + 1) return H_T;
        if (p.y() >= m_sel.bottom() - 1) return H_B;
        // A point inside the bounding rect but outside a rounded corner belongs to
        // that corner's resize control.
        return p.x() < m_sel.center().x()
            ? (p.y() < m_sel.center().y() ? H_TL : H_BL)
            : (p.y() < m_sel.center().y() ? H_TR : H_BR);
    }
    return m_sel.contains(p) ? H_Move : H_None;
}

Qt::CursorShape CaptureOverlay::cursorForHandle(Handle h)
{
    switch (h) {
    case H_TL: case H_BR: return Qt::SizeFDiagCursor;
    case H_TR: case H_BL: return Qt::SizeBDiagCursor;
    case H_L: case H_R:   return Qt::SizeHorCursor;
    case H_T: case H_B:   return Qt::SizeVerCursor;
    case H_Move:          return Qt::SizeAllCursor;
    default:              return Qt::CrossCursor;
    }
}

QRect CaptureOverlay::resizeBy(Handle h, const QPoint &pos)
{
    QRect r = m_pressSel;
    switch (h) {
    case H_TL:  r.setTopLeft(pos); break;
    case H_TR:  r.setTopRight(pos); break;
    case H_BL:  r.setBottomLeft(pos); break;
    case H_BR:  r.setBottomRight(pos); break;
    case H_L:   r.setLeft(pos.x()); break;
    case H_R:   r.setRight(pos.x()); break;
    case H_T:   r.setTop(pos.y()); break;
    case H_B:   r.setBottom(pos.y()); break;
    case H_Move: {
        const QPoint d = pos - m_pressPos;
        r.translate(d);
        r.moveTo(qBound(0, r.left(), width() - r.width()),
                 qBound(0, r.top(), height() - r.height()));
        return r;
    }
    default: break;
    }
    const qreal aspect = (m_lockAspect && m_pressSel.width() > 0 && m_pressSel.height() > 0)
        ? qreal(m_pressSel.width()) / m_pressSel.height() : 0;
    if (aspect <= 0)
        return QRect(QPoint(qMin(r.left(),r.right()),qMin(r.top(),r.bottom())),
                     QPoint(qMax(r.left(),r.right()),qMax(r.top(),r.bottom())));
    int w = r.width(), height = r.height();
    const QPoint center = m_pressSel.center();
    switch (h) {
    case H_TL: case H_TR: case H_BL: case H_BR: {
        // One fixed opposite corner; both edges grow together at the current ratio.
        w = qMax(1, w);
        height = qMax(1, height);
        if (w / aspect >= height) height = qMax(1, qRound(w / aspect));
        else w = qMax(1, qRound(height * aspect));
        const bool leftEdge = h == H_TL || h == H_BL;
        const bool topEdge = h == H_TL || h == H_TR;
        const int x = leftEdge ? m_pressSel.right() - w + 1 : m_pressSel.left();
        const int y = topEdge ? m_pressSel.bottom() - height + 1 : m_pressSel.top();
        return QRect(x, y, w, height);
    }
    case H_L: case H_R:
        w = qMax(1, w);
        height = qMax(1, qRound(w / aspect));
        return QRect(h == H_R ? m_pressSel.left() : m_pressSel.right() - w + 1,
                     center.y() - (height - 1) / 2, w, height);
    case H_T: case H_B:
        height = qMax(1, height);
        w = qMax(1, qRound(height * aspect));
        return QRect(center.x() - (w - 1) / 2,
                     h == H_B ? m_pressSel.top() : m_pressSel.bottom() - height + 1, w, height);
    default:
        break;
    }
    return QRect(QPoint(qMin(r.left(),r.right()),qMin(r.top(),r.bottom())),
                 QPoint(qMax(r.left(),r.right()),qMax(r.top(),r.bottom())));
}

void CaptureOverlay::updateTip()
{
    if (m_pinEdit) { m_sizeBar->hide(); return; }   // Size strip is capture chrome.
    const QRect r = !m_hover.isNull() ? m_hover : m_sel;
    if (r.isNull()) { m_sizeBar->hide(); return; }
    const QSize shown = sizeInUnit(r, m_sizeUnit);
    m_sizeTip->setText(QStringLiteral("%1 × %2").arg(shown.width()).arg(shown.height()));
    if (m_sizeUnitButton)
        m_sizeUnitButton->setText(m_sizeUnit == SizeUnit::Dp ? QStringLiteral("dip") : QStringLiteral("px"));
    if (m_sizeLockButton) {
        const QSignalBlocker block(m_sizeLockButton);
        m_sizeLockButton->setChecked(m_lockAspect);
    }
    m_sizeBar->adjustSize();
    const int gap = 6;
    const QSize size = m_sizeBar->size();
    const bool fullScreen = (r == rect());
    const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(r.center()));
    const QRect bounds = fullScreen ? rect()
        : localScreenRect((screen ? screen : QGuiApplication::primaryScreen())
            ->availableGeometry()).intersected(rect());
    const auto clear = [this, &bounds, gap](const QRect &candidate) {
        if (!bounds.contains(candidate)) return false;
        if (!m_layoutCardRect.isNull() && candidate.intersects(m_layoutCardRect)) return false;
        for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
            if (bar && bar->isVisible()
                && candidate.intersects(bar->geometry().adjusted(-gap, -gap, gap, gap))) return false;
        if (m_toolOptions && m_toolOptions->isVisible()
            && candidate.intersects(m_toolOptions->geometry().adjusted(-gap, -gap, gap, gap))) return false;
        if (m_morePanel && m_morePanel->isVisible()
            && candidate.intersects(m_morePanel->geometry().adjusted(-gap, -gap, gap, gap))) return false;
        return true;
    };
    // The size strip belongs to the region's top-left corner: above the region
    // with both left edges aligned, then to its left when the top edge reaches
    // the screen, then to the right, then inside as a last resort.
    QVector<QRect> candidates;
    const int cornerX = qBound(bounds.left(), r.left(),
                               qMax(bounds.left(), bounds.right() + 1 - size.width()));
    const int cornerY = qBound(bounds.top(), r.top(),
                               qMax(bounds.top(), bounds.bottom() + 1 - size.height()));
    const QRect above(QPoint(cornerX, r.top() - gap - size.height()), size);
    const QRect left(QPoint(r.left() - gap - size.width(), cornerY), size);
    const QRect right(QPoint(r.right() + 1 + gap, cornerY), size);
    const QRect below(QPoint(cornerX, r.bottom() + 1 + gap), size);
    const QRect inside(QPoint(r.left() + gap, r.top() + gap), size);
    if (fullScreen)
        candidates << inside << below;
    else
        candidates << above << left << right << below << inside;
    QVector<int> xs{candidates.first().x(), bounds.left(), bounds.right() + 1 - size.width(),
                    r.left() - gap - size.width(), r.right() + 1 + gap};
    QVector<int> ys{candidates.first().y(), bounds.top(), bounds.bottom() + 1 - size.height(),
                    r.top() - gap - size.height(), r.bottom() + 1 + gap};
    for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight}) {
        if (!bar || !bar->isVisible()) continue;
        xs << bar->x() - gap - size.width() << bar->geometry().right() + 1 + gap;
        ys << bar->y() - gap - size.height() << bar->geometry().bottom() + 1 + gap;
    }
    QRect chosen;
    for (bool outsideSelection : {true, false}) {
        const auto fits = [&](const QRect &candidate) {
            return clear(candidate) && (!outsideSelection
                || !candidate.intersects(r.adjusted(-4, -4, 4, 4)));
        };
        for (const QRect &candidate : candidates)
            if (fits(candidate)) { chosen = candidate; break; }
        if (!chosen.isNull()) break;
        int distance = -1;
        for (int x : xs) for (int y : ys) {
            const QRect candidate(QPoint(x, y), size);
            if (!fits(candidate)) continue;
            const int cost = (candidate.topLeft() - candidates.first().topLeft()).manhattanLength();
            if (distance < 0 || cost < distance) { chosen = candidate; distance = cost; }
        }
        if (!chosen.isNull()) break;
    }
    if (chosen.isNull()) {
        m_sizeBar->hide();
        return;
    }
    m_sizeBar->setGeometry(chosen);
    m_sizeBar->show();
    m_sizeBar->raise();
    if (m_customSizePopup && m_customSizePopup->isVisible() && !m_sel.isNull() && !m_updatingCustom) {
        QWidget *focus = QApplication::focusWidget();
        if (!focus || !m_customSizePopup->isAncestorOf(focus))
            updateCustomSizeInputs();
    }
}

QWidget *CaptureOverlay::barForSide(const QString &side) const
{
    if (side == QStringLiteral("top")) return m_toolsTop;
    if (side == QStringLiteral("left")) return m_toolsLeft;
    if (side == QStringLiteral("right")) return m_toolsRight;
    return m_tools;
}

QWidget *CaptureOverlay::widgetForTool(const QString &id) const
{
    if (id == QStringLiteral("more")) return m_moreButton;
    for (QWidget *button : m_toolButtons)
        if (button->property("lenssnipToolId").toString() == id) return button;
    return nullptr;
}

QString CaptureOverlay::toolSide(const QString &id) const
{
    const QString side = m_toolSide.value(id, QStringLiteral("bottom"));
    return (side == QStringLiteral("top") || side == QStringLiteral("right")
            || side == QStringLiteral("left") || side == QStringLiteral("hidden"))
        ? side : QStringLiteral("bottom");
}

QStringList CaptureOverlay::toolsForSide(const QString &side) const
{
    QStringList result;
    for (const QString &id : m_toolOrder)
        if (toolSide(id) == side) result << id;
    return result;
}

QString CaptureOverlay::activeToolSide() const
{
    if (m_autoMosaic && m_atool == ATool::Mosaic) {
        QString side = toolSide(QStringLiteral("autoMosaic"));
        if (side == QStringLiteral("hidden")) side = toolSide(QStringLiteral("more"));
        return side;
    }
    QString id;
    switch (m_atool) {
    case ATool::Rect: case ATool::Ellipse: id = QStringLiteral("shape"); break;
    case ATool::Arrow: id = QStringLiteral("arrow"); break;
    case ATool::Pen: id = QStringLiteral("pen"); break;
    case ATool::Marker: id = QStringLiteral("marker"); break;
    case ATool::Mosaic: id = QStringLiteral("mosaic"); break;
    case ATool::Text: id = QStringLiteral("text"); break;
    case ATool::Number: id = QStringLiteral("number"); break;
    case ATool::Eraser: id = QStringLiteral("eraser"); break;
    default: break;
    }
    if (id.isEmpty()) return QString();
    QString side = toolSide(id);
    if (side == QStringLiteral("hidden")) side = toolSide(QStringLiteral("more"));
    return side;
}

bool CaptureOverlay::isToolBarWidget(const QObject *object) const
{
    for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
        if (object == bar) return true;
    return false;
}

bool CaptureOverlay::overToolChrome(const QPoint &point) const
{
    for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
        if (bar && bar->isVisible() && bar->geometry().contains(point)) return true;
    return (m_toolOptions && m_toolOptions->isVisible()
            && m_toolOptions->geometry().contains(point))
        || (m_morePanel && m_morePanel->isVisible()
            && m_morePanel->geometry().contains(point));
}

void CaptureOverlay::raiseToolBars()
{
    for (QWidget *bar : {m_toolsLeft, m_toolsRight, m_toolsTop, m_tools})
        if (bar) bar->raise();
    if (m_toolOptions) m_toolOptions->raise();
    if (m_morePanel && m_morePanel->isVisible()) m_morePanel->raise();
}

void CaptureOverlay::buildToolLayout()
{
    if (!m_tools) return;
    relayoutToolButtons();
}

void CaptureOverlay::relayoutToolButtons()
{
    const QStringList sides{QStringLiteral("top"), QStringLiteral("right"),
                            QStringLiteral("bottom"), QStringLiteral("left")};
    for (const QString &side : sides) {
        QWidget *bar = barForSide(side);
        if (!bar) continue;
        if (QLayout *old = bar->layout()) {
            while (QLayoutItem *item = old->takeAt(0)) delete item;
            delete old;
        }
        if (side == QStringLiteral("left") || side == QStringLiteral("right")) {
            auto *lay = new QVBoxLayout(bar);
            lay->setContentsMargins(7, 10, 7, 10);
            lay->setSpacing(1);
        } else {
            auto *lay = new QHBoxLayout(bar);
            lay->setContentsMargins(12, 7, 12, 7);
            lay->setSpacing(4);
        }
    }
    for (QWidget *button : m_toolButtons) button->hide();
    if (m_moreButton) m_moreButton->hide();
    for (const QString &id : m_toolOrder) {
        QWidget *button = widgetForTool(id);
        if (!button) continue;
        const QString side = toolSide(id);
        if (side == QStringLiteral("hidden")) continue;
        QWidget *bar = barForSide(side);
        auto *lay = bar ? qobject_cast<QBoxLayout *>(bar->layout()) : nullptr;
        if (!lay) continue;
        button->setParent(bar);
        if (id == QStringLiteral("more")) {
            const bool vertical = side == QStringLiteral("left") || side == QStringLiteral("right");
            button->setFixedSize(vertical ? QSize(Bar::button, 20) : QSize(20, Bar::button));
            static_cast<QToolButton *>(button)->setIcon(barIcon(Icon::More, 20));
        } else {
            button->setFixedSize(Bar::button, Bar::button);
        }
        lay->addWidget(button);
        button->show();
    }
    for (const QString &side : sides) {
        QWidget *bar = barForSide(side);
        if (!bar) continue;
        bar->adjustSize();
        const bool hasItems = bar->layout() && bar->layout()->count() > 0;
        bar->setVisible(hasItems);
    }
    if (m_morePanel && m_morePanel->isVisible()) rebuildMorePanel();
}

void CaptureOverlay::loadToolLayout()
{
    if (m_embedded || m_pinEdit) return;
    LensSnipSettings settings;
    QStringList order;
    QHash<QString, QString> sides;
    const QStringList saved = settings.value(QStringLiteral("Capture/toolLayoutV2")).toStringList();
    if (!saved.isEmpty()) {
        for (const QString &entry : saved) {
            const QString id = entry.section(QLatin1Char('='), 0, 0);
            QString side = entry.section(QLatin1Char('='), 1, 1);
            if (id.isEmpty() || order.contains(id) || !widgetForTool(id)) continue;
            if (side != QStringLiteral("top") && side != QStringLiteral("right")
                && side != QStringLiteral("left") && side != QStringLiteral("hidden"))
                side = QStringLiteral("bottom");
            order << id;
            sides.insert(id, side);
        }
    } else {
        const QStringList old = settings.value(QStringLiteral("Capture/toolLayout")).toStringList();
        if (!old.isEmpty()) {
            for (const QString &entry : old) {
                const QString id = entry.section(QLatin1Char('='), 0, 0);
                bool ok = false;
                const int visibility = entry.section(QLatin1Char('='), 1, 1).toInt(&ok);
                if (id.isEmpty() || !ok || order.contains(id) || !widgetForTool(id)) continue;
                order << id;
                sides.insert(id, visibility == 0 ? QStringLiteral("bottom") : QStringLiteral("hidden"));
            }
        } else {
            order = settings.value(QStringLiteral("Capture/toolOrder")).toStringList();
            for (const QString &id : order)
                if (widgetForTool(id)) sides.insert(id, QStringLiteral("bottom"));
        }
    }
    // A tool added after the user saved a layout must appear next to its group,
    // not at the far end where the overflow panel would hide it.
    QStringList all = m_defaultToolOrder;
    if (m_moreButton) all << QStringLiteral("more");
    if (order.isEmpty()) order = all;
    // The reference keeps watermark in the secondary tool area; users who use it
    // often can drag it to a primary side in the toolbar editor.
    if (!sides.contains(QStringLiteral("watermark")))
        sides.insert(QStringLiteral("watermark"), QStringLiteral("hidden"));
    for (const QString &id : all)
        if (!order.contains(id)) {
            order << id;
            sides.insert(id, QStringLiteral("bottom"));
        }
    m_toolOrder.clear();
    m_toolSide.clear();
    for (const QString &id : order) {
        if (!all.contains(id) || m_toolOrder.contains(id)) continue;
        QString side = sides.value(id, QStringLiteral("bottom"));
        if (id == QStringLiteral("more") && side == QStringLiteral("hidden"))
            side = QStringLiteral("bottom");
        m_toolOrder << id;
        m_toolSide.insert(id, side);
    }
}

void CaptureOverlay::saveToolLayout()
{
    if (m_embedded || m_pinEdit) return;
    QStringList layout;
    for (const QString &id : m_toolOrder)
        layout << id + QLatin1Char('=') + toolSide(id);
    LensSnipSettings settings;
    settings.setValue(QStringLiteral("Capture/toolLayoutV2"), layout);
    settings.remove(QStringLiteral("Capture/toolLayout"));
    settings.remove(QStringLiteral("Capture/toolDock"));
    settings.remove(QStringLiteral("Capture/toolOrder"));
}

QVector<ToolLayoutItem> CaptureOverlay::currentToolLayout() const
{
    QVector<ToolLayoutItem> result;
    for (const QString &id : m_toolOrder) {
        if (id == QStringLiteral("more")) {
            result.append(ToolLayoutItem{id, tr("下拉"), Icon::More,
                                         toolSide(id), true});
            continue;
        }
        if (!widgetForTool(id)) continue;
        result.append(ToolLayoutItem{id, toolDisplayName(id), toolIcon(id),
                                     toolSide(id), false});
    }
    return result;
}

void CaptureOverlay::applyToolLayout(const QVector<ToolLayoutItem> &items)
{
    QStringList order;
    QHash<QString, QString> sides;
    for (const ToolLayoutItem &item : items) {
        if (!widgetForTool(item.id) || order.contains(item.id)) continue;
        QString side = item.side;
        if (side != QStringLiteral("top") && side != QStringLiteral("right")
            && side != QStringLiteral("left") && side != QStringLiteral("hidden"))
            side = QStringLiteral("bottom");
        if (item.id == QStringLiteral("more") && side == QStringLiteral("hidden"))
            side = QStringLiteral("bottom");
        order << item.id;
        sides.insert(item.id, side);
    }
    QStringList all = m_defaultToolOrder;
    if (m_moreButton) all << QStringLiteral("more");
    for (const QString &id : all)
        if (!order.contains(id)) {
            order << id;
            sides.insert(id, QStringLiteral("bottom"));
        }
    m_toolOrder = order;
    m_toolSide = sides;
    relayoutToolButtons();
    if (!m_embedded && !m_sel.isNull()) placeToolBars();
}

void CaptureOverlay::rebuildMorePanel()
{
    if (!m_morePanel) return;
    auto *lay = qobject_cast<QHBoxLayout *>(m_morePanel->layout());
    if (!lay) return;
    while (QLayoutItem *item = lay->takeAt(0)) {
        if (QWidget *widget = item->widget()) { widget->hide(); widget->deleteLater(); }
        delete item;
    }
    for (const QString &id : toolsForSide(QStringLiteral("hidden"))) {
        auto *button = qobject_cast<QToolButton *>(widgetForTool(id));
        if (!button) continue;
        auto *proxy = barButton(toolIcon(id), toolDisplayName(id));
        proxy->setObjectName(QStringLiteral("toolMoreItem"));
        proxy->setCheckable(button->isCheckable());
        proxy->setChecked(button->isChecked());
        proxy->setEnabled(button->isEnabled());
        proxy->setFocusPolicy(Qt::NoFocus);
        connect(proxy, &QToolButton::clicked, this, [this, button] { triggerToolButton(button); });
        lay->addWidget(proxy);
    }
    auto *adjust = new QToolButton(m_morePanel);
    adjust->setObjectName(QStringLiteral("toolMoreAdjust"));
    adjust->setIcon(barIcon(Icon::Sliders, 20));
    adjust->setIconSize(QSize(20, 20));
    adjust->setFixedSize(Bar::button, Bar::button);
    adjust->setCursor(Qt::PointingHandCursor);
    adjust->setFocusPolicy(Qt::NoFocus);
    adjust->setToolTip(tr("调整工具栏"));
    adjust->setAccessibleName(tr("调整工具栏"));
    connect(adjust, &QToolButton::clicked, this, &CaptureOverlay::showToolLayoutDialog);
    lay->addWidget(adjust);
    m_morePanel->adjustSize();
    placeMorePanel();
}

void CaptureOverlay::toggleMorePanel(bool open)
{
    if (!m_morePanel || !m_moreButton) return;
    // The kebab glyph never flips: open state is carried by the checked background.
    if (!open) {
        m_morePanel->hide();
        return;
    }
    rebuildMorePanel();
    m_morePanel->show();
    m_morePanel->raise();
}

void CaptureOverlay::placeMorePanel()
{
    if (!m_morePanel || !m_moreButton) return;
    m_morePanel->adjustSize();
    const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_sel.center()));
    const QRect avail = localScreenRect((screen ? screen : QGuiApplication::primaryScreen())->availableGeometry());
    QWidget *barWidget = m_moreButton->parentWidget();
    const QRect bar = barWidget
        ? QRect(barWidget->mapTo(this, QPoint(0, 0)), barWidget->size())
        : QRect(m_moreButton->mapTo(this, QPoint(0, 0)), m_moreButton->size());
    const bool vertical = toolSide(QStringLiteral("more")) == QStringLiteral("left")
        || toolSide(QStringLiteral("more")) == QStringLiteral("right");
    const QSize size = m_morePanel->size();
    QVector<QRect> candidates;
    if (vertical) {
        candidates << QRect(QPoint(bar.left() - Bar::gap - size.width(), bar.top()), size)
                   << QRect(QPoint(bar.right() + 1 + Bar::gap, bar.top()), size)
                   << QRect(QPoint(bar.left() - Bar::gap - size.width(),
                                   bar.bottom() + 1 - size.height()), size)
                   << QRect(QPoint(bar.left(), bar.bottom() + 1 + Bar::gap), size);
    } else {
        candidates << QRect(QPoint(bar.right() + 1 - size.width(), bar.bottom() + 1 + Bar::gap), size)
                   << QRect(QPoint(bar.right() + 1 - size.width(),
                                   bar.top() - Bar::gap - size.height()), size)
                   << QRect(QPoint(bar.left(), bar.bottom() + 1 + Bar::gap), size)
                   << QRect(QPoint(bar.left(), bar.top() - Bar::gap - size.height()), size)
                   << QRect(QPoint(bar.left() - Bar::gap - size.width(),
                                   bar.top() + (bar.height() - size.height()) / 2), size)
                   << QRect(QPoint(bar.right() + 1 + Bar::gap,
                                   bar.top() + (bar.height() - size.height()) / 2), size);
    }
    if (!m_sel.isNull()) {
        const int belowBar = bar.bottom() + 1 + Bar::gap;
        const int besideSelectionY = qMax(avail.top(), qMin(belowBar, avail.bottom() + 1 - size.height()));
        candidates << QRect(QPoint(m_sel.left() - Bar::gap - size.width(), besideSelectionY), size)
                   << QRect(QPoint(m_sel.right() + 1 + Bar::gap, besideSelectionY), size)
                   << QRect(QPoint(m_sel.left() - Bar::gap - size.width(), bar.top()), size)
                   << QRect(QPoint(m_sel.right() + 1 + Bar::gap, bar.top()), size);
    }
    const auto clear = [this, &avail](const QRect &candidate) {
        if (!avail.contains(candidate)) return false;
        if (!m_layoutCardRect.isNull() && candidate.intersects(m_layoutCardRect)) return false;
        if (!m_sel.isNull() && candidate.intersects(m_sel)) return false;
        for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
            if (bar && bar->isVisible() && candidate.intersects(bar->geometry())) return false;
        if (m_sizeBar && m_sizeBar->isVisible()
            && candidate.intersects(m_sizeBar->geometry())) return false;
        if (m_toolOptions && m_toolOptions->isVisible()
            && candidate.intersects(m_toolOptions->geometry())) return false;
        return true;
    };
    QRect placed;
    for (const QRect &candidate : candidates) {
        if (clear(candidate)) {
            placed = candidate;
            break;
        }
    }
    if (placed.isNull()) {
        placed = candidates.value(0);
        placed.moveLeft(qBound(avail.left(), placed.left(),
                               qMax(avail.left(), avail.right() + 1 - placed.width())));
        placed.moveTop(qBound(avail.top(), placed.top(),
                              qMax(avail.top(), avail.bottom() + 1 - placed.height())));
    }
    m_morePanel->move(placed.topLeft());
}

void CaptureOverlay::showToolLayoutDialog()
{
    if (m_embedded || m_pinEdit) return;
    if (m_moreButton) {
        m_moreButton->setChecked(false);
        if (m_morePanel) m_morePanel->hide();
    }
    const QVector<ToolLayoutItem> previous = currentToolLayout();
    QVector<ToolLayoutItem> entries = previous;
    QVector<ToolLayoutItem> defaults;
    for (const QString &id : m_defaultToolOrder)
        defaults.append(ToolLayoutItem{id, toolDisplayName(id), toolIcon(id),
                                       QStringLiteral("bottom"), false});
    if (m_moreButton)
        defaults.append(ToolLayoutItem{QStringLiteral("more"), tr("下拉"), Icon::ChevronDown,
                                       QStringLiteral("bottom"), true});

    ToolLayoutDialog dialog(entries, defaults, this);
    const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_sel.center()));
    dialog.fitToScreen((screen ? screen : QGuiApplication::primaryScreen())->availableGeometry());
    m_layoutCardWidget = &dialog;
    updateLayoutCardRect();
    connect(&dialog, &ToolLayoutDialog::layoutChanged, this,
            [this](const QVector<ToolLayoutItem> &items) { applyToolLayout(items); });
    placeToolBars();
    const int result = dialog.exec();
    const QVector<ToolLayoutItem> finalItems = dialog.items();
    m_layoutCardWidget = nullptr;
    m_layoutCardRect = QRect();
    if (result != QDialog::Accepted) {
        applyToolLayout(previous);
        placeToolBars();
        showToolBars();
        return;
    }
    applyToolLayout(finalItems);
    saveToolLayout();
    placeToolBars();
    showToolBars();
}

void CaptureOverlay::updateLayoutCardRect()
{
    if (!m_layoutCardWidget) {
        m_layoutCardRect = QRect();
        return;
    }
    const QRect global = m_layoutCardWidget->geometry();
    m_layoutCardRect = QRect(mapFromGlobal(global.topLeft()), global.size());
}

void CaptureOverlay::triggerToolButton(QToolButton *button)
{
    if (!button) return;
    if (button == m_refreshButton) refreshScreenshot();
    else button->click();
}

QString CaptureOverlay::toolDisplayName(const QString &id) const
{
    if (id == QStringLiteral("shape")) return tr("形状");
    if (id == QStringLiteral("arrow")) return tr("箭头");
    if (id == QStringLiteral("pen")) return tr("画笔");
    if (id == QStringLiteral("marker")) return tr("荧光笔");
    if (id == QStringLiteral("mosaic")) return tr("马赛克");
    if (id == QStringLiteral("text")) return tr("文字");
    if (id == QStringLiteral("number")) return tr("序号");
    if (id == QStringLiteral("eraser")) return tr("橡皮");
    if (id == QStringLiteral("watermark")) return tr("水印");
    if (id == QStringLiteral("undo")) return tr("撤销");
    if (id == QStringLiteral("redo")) return tr("重做");
    if (id == QStringLiteral("scroll")) return tr("长截图");
    if (id == QStringLiteral("ocr")) return tr("文字识别");
    if (id == QStringLiteral("translate")) return tr("翻译");
    if (id == QStringLiteral("barcode")) return tr("二维码");
    if (id == QStringLiteral("pin")) return tr("贴图");
    if (id == QStringLiteral("refresh")) return tr("刷新");
    if (id == QStringLiteral("save")) return tr("保存");
    if (id == QStringLiteral("copy")) return tr("复制");
    if (id == QStringLiteral("done")) return tr("完成");
    if (id == QStringLiteral("more")) return tr("下拉");
    return id;
}

Icon CaptureOverlay::toolIcon(const QString &id) const
{
    if (id == QStringLiteral("shape")) return Icon::Rect;
    if (id == QStringLiteral("arrow")) return Icon::Arrow;
    if (id == QStringLiteral("pen")) return Icon::Pen;
    if (id == QStringLiteral("marker")) return Icon::Marker;
    if (id == QStringLiteral("mosaic")) return Icon::Mosaic;
    if (id == QStringLiteral("text")) return Icon::Text;
    if (id == QStringLiteral("number")) return Icon::Number;
    if (id == QStringLiteral("eraser")) return Icon::Eraser;
    if (id == QStringLiteral("watermark")) return Icon::Watermark;
    if (id == QStringLiteral("undo")) return Icon::Undo;
    if (id == QStringLiteral("redo")) return Icon::Redo;
    if (id == QStringLiteral("scroll")) return Icon::LongShot;
    if (id == QStringLiteral("ocr")) return Icon::Ocr;
    if (id == QStringLiteral("translate")) return Icon::Translate;
    if (id == QStringLiteral("barcode")) return Icon::Barcode;
    if (id == QStringLiteral("pin")) return Icon::Pin;
    if (id == QStringLiteral("refresh")) return Icon::Refresh;
    if (id == QStringLiteral("save")) return Icon::Save;
    if (id == QStringLiteral("copy")) return Icon::Copy;
    if (id == QStringLiteral("done")) return Icon::Check;
    if (id == QStringLiteral("more")) return Icon::More;
    return Icon::Rect;
}

void CaptureOverlay::placeToolBars()
{
    if (m_embedded) return;   // The pin toolbar owns placement; the strip is top-level.
    const QRect old = [this] {
        QRect result;
        for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
            if (bar && bar->isVisible()) result = result.isNull()
                ? bar->geometry() : result.united(bar->geometry());
        if (m_toolOptions && m_toolOptions->isVisible())
            result = result.isNull() ? m_toolOptions->geometry()
                                     : result.united(m_toolOptions->geometry());
        return result;
    }();
    const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_sel.center()));
    const QRect avail = localScreenRect((screen ? screen : QGuiApplication::primaryScreen())->availableGeometry());
    const int gap = Bar::gap;
    const int selRight = m_sel.right() + 1, selBottom = m_sel.bottom() + 1;

    if (m_pinEdit) {
        m_tools->show();
        const QSize main = m_tools->sizeHint();
        const int width = qMax(main.width(), optionsTool() == ATool::None ? 0 : m_toolOptions->sizeHint().width());
        const int x = qBound(avail.left(), m_sel.center().x() - width / 2,
                             qMax(avail.left(), avail.right() + 1 - width));
        int y = selBottom + gap;
        if (y + main.height() > avail.bottom() + 1) y = m_sel.top() - gap - main.height();
        m_tools->setGeometry(x + width - main.width(), y, main.width(), main.height());
        positionToolOptions();
        const QRect glass = m_toolOptions->isVisible()
            ? m_tools->geometry().united(m_toolOptions->geometry()) : m_tools->geometry();
        update(glass.united(old).adjusted(-4, -4, 4, 4));
        return;
    }

    QVector<QRect> placedBars;
    for (const QString &side : QStringList{QStringLiteral("top"), QStringLiteral("right"),
                                           QStringLiteral("bottom"), QStringLiteral("left")}) {
        QWidget *bar = barForSide(side);
        if (!bar || !bar->layout() || bar->layout()->count() == 0)
            continue;
        const QSize size = bar->sizeHint();
        QPoint point;
        if (side == QStringLiteral("top")) point = QPoint(selRight - size.width(), m_sel.top() - gap - size.height());
        else if (side == QStringLiteral("bottom")) point = QPoint(selRight - size.width(), selBottom + gap);
        else if (side == QStringLiteral("left")) point = QPoint(m_sel.left() - gap - size.width(),
                                                                m_sel.top());
        else point = QPoint(selRight + gap, m_sel.top());
        const QRect preferred(QPoint(
            qBound(avail.left(), point.x(), qMax(avail.left(), avail.right() + 1 - size.width())),
            qBound(avail.top(), point.y(), qMax(avail.top(), avail.bottom() + 1 - size.height()))), size);
        QVector<QRect> candidates{preferred};
        if (!m_sel.isNull() && preferred.intersects(m_sel)) {
            if (side == QStringLiteral("top")) {
                candidates << QRect(QPoint(m_sel.left() - gap - size.width(), avail.top()), size)
                             << QRect(QPoint(selRight + gap, avail.top()), size)
                             << QRect(QPoint(selRight - size.width(), selBottom + gap), size);
            } else if (side == QStringLiteral("bottom")) {
                candidates << QRect(QPoint(m_sel.left() - gap - size.width(),
                                             avail.bottom() + 1 - size.height()), size)
                             << QRect(QPoint(selRight + gap,
                                             avail.bottom() + 1 - size.height()), size)
                             << QRect(QPoint(selRight - size.width(), m_sel.top() - gap - size.height()), size);
            } else if (side == QStringLiteral("left")) {
                candidates << QRect(QPoint(avail.left(), m_sel.top() - gap - size.height()), size)
                             << QRect(QPoint(avail.left(), selBottom + gap), size)
                             << QRect(QPoint(selRight + gap,
                                             m_sel.center().y() - size.height() / 2), size);
            } else {
                candidates << QRect(QPoint(avail.right() + 1 - size.width(),
                                             m_sel.top() - gap - size.height()), size)
                             << QRect(QPoint(avail.right() + 1 - size.width(), selBottom + gap), size)
                             << QRect(QPoint(m_sel.left() - gap - size.width(),
                                             m_sel.center().y() - size.height() / 2), size);
            }
        }
        QVector<int> xs{preferred.x(), avail.left(), avail.right() + 1 - size.width(),
                        m_sel.left() - gap - size.width(), selRight + gap};
        QVector<int> ys{preferred.y(), avail.top(), avail.bottom() + 1 - size.height(),
                        m_sel.top() - gap - size.height(), selBottom + gap};
        QVector<QRect> obstacles = placedBars;
        if (!m_layoutCardRect.isNull()) obstacles << m_layoutCardRect;
        const bool vertical = side == QStringLiteral("left") || side == QStringLiteral("right");
        for (const QRect &obstacle : obstacles) {
            const QRect before(QPoint(obstacle.left() - gap - size.width(), preferred.y()), size);
            const QRect after(QPoint(obstacle.right() + 1 + gap, preferred.y()), size);
            const QRect above(QPoint(preferred.x(), obstacle.top() - gap - size.height()), size);
            const QRect below(QPoint(preferred.x(), obstacle.bottom() + 1 + gap), size);
            if (vertical) candidates << below << above << before << after;
            else candidates << before << after << above << below;
            xs << before.x() << after.x();
            ys << above.y() << below.y();
        }
        const auto fits = [&](const QRect &candidate, bool outsideSelection) {
            if (candidate.isNull() || !avail.contains(candidate)) return false;
            if (outsideSelection && candidate.intersects(m_sel.adjusted(-4, -4, 4, 4))) return false;
            for (const QRect &obstacle : obstacles)
                if (candidate.adjusted(-gap, -gap, gap, gap).intersects(obstacle)) return false;
            return true;
        };
        QRect chosen;
        // Only this layout pass owns occupied bounds; hidden or stale widgets cannot affect placement.
        for (bool outsideSelection : {true, false}) {
            for (const QRect &candidate : candidates) {
                const QRect adjusted = avoidLayoutCard(candidate, avail, side);
                if (fits(adjusted, outsideSelection)) { chosen = adjusted; break; }
            }
            if (!chosen.isNull()) break;
            int distance = -1;
            for (int x : xs) for (int y : ys) {
                const QRect candidate(QPoint(x, y), size);
                if (!fits(candidate, outsideSelection)) continue;
                const int cost = (candidate.topLeft() - preferred.topLeft()).manhattanLength();
                if (distance < 0 || cost < distance) { chosen = candidate; distance = cost; }
            }
            if (!chosen.isNull()) break;
        }
        if (chosen.isNull() && !m_layoutCardRect.isNull()) {
            bar->hide();   // The modal editor already previews tools when no external space remains.
            continue;
        }
        // shortcut: a bar larger than the screen keeps clamped bounds until overflow rules are defined.
        if (chosen.isNull()) chosen = preferred;
        placedBars << chosen;
        bar->setGeometry(chosen);
        bar->show();
        bar->raise();
    }
    positionToolOptions();
    updateTip();
    const QRect glass = [this] {
        QRect result;
        for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
            if (bar && bar->isVisible()) result = result.isNull()
                ? bar->geometry() : result.united(bar->geometry());
        if (m_toolOptions && m_toolOptions->isVisible())
            result = result.isNull() ? m_toolOptions->geometry()
                                     : result.united(m_toolOptions->geometry());
        return result;
    }();
    update(glass.united(old).adjusted(-4, -4, 4, 4));
    if (m_morePanel && m_morePanel->isVisible()) placeMorePanel();
}

QRect CaptureOverlay::avoidLayoutCard(const QRect &bar, const QRect &avail, const QString &side) const
{
    if (m_layoutCardRect.isNull() || !bar.intersects(m_layoutCardRect)) return bar;
    QRect candidate = bar;
    if (side == QStringLiteral("top"))
        candidate.moveTop(m_layoutCardRect.top() - Bar::gap - bar.height());
    else if (side == QStringLiteral("bottom"))
        candidate.moveTop(m_layoutCardRect.bottom() + 1 + Bar::gap);
    else if (side == QStringLiteral("left"))
        candidate.moveLeft(m_layoutCardRect.left() - Bar::gap - bar.width());
    else
        candidate.moveLeft(m_layoutCardRect.right() + 1 + Bar::gap);
    if (avail.contains(candidate) && !candidate.intersects(m_layoutCardRect))
        return candidate;
    return QRect();
}

void CaptureOverlay::positionToolOptions()
{
    if (m_embedded) return;                       // The pin positions the detached strip.
    if (!m_toolOptions->isVisible() && optionsTool() == ATool::None) return;
    QString side = activeToolSide();
    if (side.isEmpty() || side == QStringLiteral("hidden")) side = toolSide(QStringLiteral("more"));
    QWidget *barWidget = barForSide(side);
    if (!barWidget || !barWidget->isVisible()) barWidget = m_tools;
    const QRect bar = barWidget ? barWidget->geometry() : QRect();
    const QScreen *screen = QGuiApplication::screenAt(mapToGlobal(m_sel.center()));
    const QRect avail = localScreenRect((screen ? screen : QGuiApplication::primaryScreen())->availableGeometry());
    const QSize hint = m_toolOptions->sizeHint();
    const int gap = Bar::gap;
    const bool vertical = side == QStringLiteral("left") || side == QStringLiteral("right");
    QVector<QRect> candidates;
    if (vertical) {
        const bool barOnRight = bar.center().x() > m_sel.center().x();
        const int outsideX = barOnRight ? bar.left() - gap - hint.width() : bar.right() + 1 + gap;
        const int insideX = barOnRight ? bar.right() + 1 + gap : bar.left() - gap - hint.width();
        const int y = bar.top() + (bar.height() - hint.height()) / 2;
        candidates << QRect(QPoint(outsideX, y), hint)
                   << QRect(QPoint(insideX, y), hint)
                   << QRect(QPoint(bar.left(), bar.bottom() + 1 + gap), hint)
                   << QRect(QPoint(bar.left(), bar.top() - gap - hint.height()), hint);
    } else {
        const int x = qBound(avail.left(), bar.left(),
                             qMax(avail.left(), avail.right() + 1 - hint.width()));
        const int outsideY = bar.bottom() < m_sel.top()
            ? bar.top() - gap - hint.height() : bar.bottom() + 1 + gap;
        const int insideY = bar.bottom() < m_sel.top()
            ? bar.bottom() + 1 + gap : bar.top() - gap - hint.height();
        candidates << QRect(QPoint(x, outsideY), hint)
                   << QRect(QPoint(x, insideY), hint)
                   << QRect(QPoint(x, m_sel.top() - gap - hint.height()), hint)
                   << QRect(QPoint(x, m_sel.bottom() + 1 + gap), hint);
    }
    QRect chosen;
    for (const QRect &candidate : candidates) {
        if (avail.contains(candidate)
            && (m_layoutCardRect.isNull() || !candidate.intersects(m_layoutCardRect))) {
            chosen = candidate;
            break;
        }
    }
    if (chosen.isNull() && !m_layoutCardRect.isNull()) {
        m_toolOptions->hide();
        return;
    }
    if (chosen.isNull()) {
        chosen = candidates.value(0);
        chosen.moveLeft(qBound(avail.left(), chosen.left(),
                               qMax(avail.left(), avail.right() + 1 - chosen.width())));
        chosen.moveTop(qBound(avail.top(), chosen.top(),
                              qMax(avail.top(), avail.bottom() + 1 - chosen.height())));
    }
    m_toolOptions->setGeometry(chosen);
    if (!m_layoutCardRect.isNull() && optionsTool() != ATool::None) m_toolOptions->show();
}

void CaptureOverlay::hideToolBars()
{
    if (optionsTool() != ATool::None) return;     // Active tool options remain available throughout a drag.
    if (m_toolOptions->isVisible()) {
        update(m_toolOptions->geometry().adjusted(-4, -4, 4, 4));
        m_toolOptions->hide();
    }
    for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight}) {
        if (bar && bar->isVisible()) {
            update(bar->geometry().adjusted(-4, -4, 4, 4));
            bar->hide();
        }
    }
    if (m_morePanel) m_morePanel->hide();
    if (m_moreButton && m_moreButton->isChecked()) {
        const QSignalBlocker block(m_moreButton);
        m_moreButton->setChecked(false);
    }
}

void CaptureOverlay::showToolBars()
{
    if (!m_embedded) {
        for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
            if (bar && bar->layout() && bar->layout()->count() > 0) bar->show();
    }
    if (optionsTool() != ATool::None) {
        positionToolOptions();
        m_toolOptions->show();
    }
}

void CaptureOverlay::presetSelection(const QRect &localSel, ATool tool)
{
    // Same state as a user-committed click selection: adjusting mode, toolbars
    // visible, optionally a drawing tool armed. Re-annotation of an existing
    // image (pin edit) starts here instead of a fresh region drag.
    m_sel = localSel.normalized();
    if (m_sel.width() < 4 || m_sel.height() < 4) { m_sel = {}; return; }
    m_mode = Adjusting;
    ensureAutoWatermark();
    clearHover();
    m_drag = H_None;
    if (!m_pinEdit) setCursor(Qt::SizeAllCursor);   // Region moving is a capture-only gesture.
    placeToolBars();
    showToolBars();
    updateTip();
    updateInspector();
    if (tool != ATool::None) selectTool(tool);
    update();
}

QPainterPath CaptureOverlay::selectionPath(const QRect &r) const
{
    QPainterPath path;
    if (r.isEmpty()) return path;
    if (m_roundEnabled && m_roundRadius > 0 && !m_embedded && !m_pinEdit)
        path.addRoundedRect(QRectF(r), m_roundRadius, m_roundRadius);
    else
        path.addRect(QRectF(r));
    return path;
}

void CaptureOverlay::applyRoundSettings(bool persist)
{
    if (m_embedded) return;
    const bool rounded = m_roundEnabled;
    if (m_sizeCornerButton) {
        const QSignalBlocker lock(m_sizeCornerButton);
        m_sizeCornerButton->setChecked(rounded);
        m_sizeCornerButton->setIcon(barIcon(rounded ? Icon::CornerRounded : Icon::CornerSquare));
    }
    if (m_sizeRadiusSpin) {
        const QSignalBlocker lock(m_sizeRadiusSpin);
        m_sizeRadiusSpin->setValue(m_roundRadius);
        if (auto *box = m_sizeRadiusSpin->parentWidget()) box->setVisible(rounded);
    }
    if (m_sizeRadiusDivider) m_sizeRadiusDivider->setVisible(rounded);
    if (m_sizeBar && m_sizeBar->isVisible()) m_sizeBar->adjustSize();
    if (persist) {
        LensSnipSettings settings;
        settings.setValue(QStringLiteral("Capture/roundEnabled"), m_roundEnabled);
        settings.setValue(QStringLiteral("Capture/roundRadius"), m_roundRadius);
    }
    update();
}

QRect CaptureOverlay::nativeFromGlobal(const QRect &global) const
{
    if (global.isEmpty()) return {};
    for (const ScreenFrame &frame : m_frames) {
        if (frame.logicalRect.isEmpty() || !frame.logicalRect.contains(global.center()))
            continue;
        const qreal sx = qreal(frame.pixelRect.width()) / frame.logicalRect.width();
        const qreal sy = qreal(frame.pixelRect.height()) / frame.logicalRect.height();
        if (sx <= 0 || sy <= 0) break;
        return QRect(qRound(frame.pixelRect.left() + (global.left() - frame.logicalRect.left()) * sx),
                     qRound(frame.pixelRect.top() + (global.top() - frame.logicalRect.top()) * sy),
                     qRound(global.width() * sx), qRound(global.height() * sy));
    }
    return global;
}

QRect CaptureOverlay::globalFromUnit(int x, int y, int width, int height, SizeUnit unit) const
{
    if (unit == SizeUnit::Dp) return QRect(x, y, width, height);
    const QPoint native(x, y);
    for (const ScreenFrame &frame : m_frames) {
        if (frame.pixelRect.isEmpty() || !frame.pixelRect.contains(native))
            continue;
        const qreal sx = qreal(frame.logicalRect.width()) / frame.pixelRect.width();
        const qreal sy = qreal(frame.logicalRect.height()) / frame.pixelRect.height();
        return QRect(qRound(frame.logicalRect.left() + (x - frame.pixelRect.left()) * sx),
                     qRound(frame.logicalRect.top() + (y - frame.pixelRect.top()) * sy),
                     qMax(1, qRound(width * sx)), qMax(1, qRound(height * sy)));
    }
    return QRect(x, y, width, height);
}

QRect CaptureOverlay::unitFromLocal(const QRect &local, SizeUnit unit) const
{
    if (local.isEmpty()) return {};
    const QRect global = globalScreenRect(local);
    return unit == SizeUnit::Dp ? global : nativeFromGlobal(global);
}

QSize CaptureOverlay::sizeInUnit(const QRect &local, SizeUnit unit) const
{
    return unitFromLocal(local, unit).size();
}

void CaptureOverlay::setSizeUnit(SizeUnit unit, bool persist)
{
    m_sizeUnit = unit;
    if (persist) {
        LensSnipSettings settings;
        settings.setValue(QStringLiteral("Capture/sizeUnit"),
                          unit == SizeUnit::Dp ? QStringLiteral("dip") : QStringLiteral("px"));
    }
    updateTip();
}

void CaptureOverlay::updateCustomSizeInputs()
{
    if (!m_customX || !m_customY || !m_customWidth || !m_customHeight || m_sel.isNull()) return;
    const QRect shown = unitFromLocal(m_sel, m_sizeUnit);
    m_updatingCustom = true;
    m_customX->setValue(shown.x());
    m_customY->setValue(shown.y());
    m_customWidth->setValue(qMax(1, shown.width()));
    m_customHeight->setValue(qMax(1, shown.height()));
    m_updatingCustom = false;
}

static const QSize kCommonSizePresets[] = {
    {1920, 1080}, {2560, 1440}, {3840, 2160}, {1280, 720}, {1024, 768},
    {1920, 1200}, {1080, 1080}, {800, 600}, {640, 480}
};

static void applyDarkWindowChrome(QWidget *window)
{
    if (!window) return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof(darkTitle));
    const COLORREF captionColor = RGB(24, 25, 29), titleText = RGB(244, 245, 248),
                   borderColor = RGB(43, 45, 51);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &titleText, sizeof(titleText));
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &borderColor, sizeof(borderColor));
    QPalette dark = window->palette();
    dark.setColor(QPalette::Window, Bar::surface);
    dark.setColor(QPalette::Base, Bar::content);
    dark.setColor(QPalette::WindowText, Bar::ink);
    dark.setColor(QPalette::Text, Bar::ink);
    dark.setColor(QPalette::ButtonText, Bar::ink);
    dark.setColor(QPalette::Mid, Bar::divider);
    dark.setColor(QPalette::Dark, Bar::divider);
    window->setPalette(dark);
}

void CaptureOverlay::showCustomSizePopup()
{
    if (m_embedded || m_pinEdit || m_sel.isNull()) return;
    if (m_customSizePopup) {
        loadSizePresets();
        refreshPresetCombo();
        updateCustomSizeInputs();
        m_customSizePopup->show();
        m_customSizePopup->raise();
        m_customSizePopup->activateWindow();
        return;
    }
    auto *popup = new QDialog(this, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                                        | Qt::WindowStaysOnTopHint);
    m_customSizePopup = popup;
    popup->setObjectName(QStringLiteral("customSizePanel"));
    popup->setWindowTitle(tr("自定义截图"));
    popup->setModal(false);
    popup->setAttribute(Qt::WA_DeleteOnClose, false);
    popup->setStyleSheet(QStringLiteral(
        "QLabel{color:#9c9da1;background:transparent;font-size:13px;}"
        "QLabel#customSection{color:#9c9da1;font-weight:400;}"
        "QLabel#customHint{color:#9c9da1;font-size:12px;}"
        "QFrame#customDivider{background:#2b2d33;border:none;}"
        "QFrame#customField{background:#111216;border:1px solid #777980;border-radius:8px;}"
        "QFrame#customField:hover{background:#222326;}"
        "QFrame#customField[focus=\"true\"]{border-color:#828fff;background:#222326;}"
        "QLabel#customFieldLabel{color:#9c9da1;background:transparent;font-size:13px;}"
        "QSpinBox{color:#f4f5f8;background:transparent;border:none;padding:0;min-height:30px;"
        "font-size:14px;selection-background-color:#5e6ad2;}"
        "QSpinBox:hover,QSpinBox:focus{background:transparent;border:none;}"
        "QComboBox{color:#f4f5f8;background:#111216;border:1px solid #777980;border-radius:8px;"
        "padding:0 8px;min-height:30px;font-size:13px;}"
        "QComboBox:hover{background:#222326;} QComboBox:focus{border-color:#828fff;}"
        "QComboBox::drop-down{border:none;width:22px;background:transparent;}"
        "QComboBox::down-arrow{image:url(:/icons/chevron_down_light.png);width:10px;height:10px;}"
        "QComboBox QAbstractItemView{color:#f4f5f8;background:#18191d;border:none;"
        "border-radius:0px;padding:0px;outline:0;selection-background-color:#5e6ad2;"
        "selection-color:#ffffff;}"
        "QComboBox QAbstractItemView::item{min-height:26px;padding-left:10px;padding-right:8px;}"
        "QScrollBar:vertical{background:transparent;width:8px;margin:0;}"
        "QScrollBar::handle:vertical{background:#35373e;border-radius:4px;min-height:20px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QCheckBox{color:#f4f5f8;background:transparent;font-size:13px;spacing:7px;}"
        "QCheckBox::indicator{width:16px;height:16px;border:1px solid #777980;border-radius:4px;"
        "background:#111216;}"
        "QCheckBox::indicator:hover{border-color:#73757f;}"
        "QCheckBox::indicator:checked{background:#5e6ad2;border-color:#5e6ad2;"
        "image:url(:/icons/check_light.png);}"
        "QPushButton{color:#f4f5f8;background:#18191d;border:1px solid #2b2d33;border-radius:6px;"
        "padding:0 8px;min-height:30px;font-size:13px;text-align:center;}"
        "QPushButton#customPresetSave{text-align:center;}"
        "QPushButton:hover{background:#222326;} QPushButton:pressed{background:#2b2d33;}"));

    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto makeField = [this, popup](const QString &name, int minimum, int maximum,
                                   QPointer<QSpinBox> &out) {
        auto *field = new QFrame(popup);
        field->setObjectName(QStringLiteral("customField"));
        field->setFixedSize(100, 34);
        auto *row = new QHBoxLayout(field);
        row->setContentsMargins(2, 0, 2, 0);
        row->setSpacing(0);
        auto *spin = new QSpinBox(field);
        spin->setObjectName(name);
        spin->setRange(minimum, maximum);
        spin->setAlignment(Qt::AlignCenter);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setFrame(false);
        spin->setFixedHeight(30);
        spin->installEventFilter(this);
        for (QWidget *child : spin->findChildren<QWidget *>()) child->installEventFilter(this);
        row->addWidget(spin, 1, Qt::AlignVCenter);
        auto *steps = new QWidget(field);
        auto *stepLayout = new QVBoxLayout(steps);
        stepLayout->setContentsMargins(0, 0, 0, 0);
        stepLayout->setSpacing(0);
        const auto makeStep = [steps, spin](const QString &objectName, Icon icon, auto step) {
            auto *button = new QToolButton(steps);
            button->setObjectName(objectName);
            button->setIcon(makeIcon(icon, 9, true));
            button->setIconSize(QSize(9, 9));
            button->setFixedSize(16, 15);
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setCursor(Qt::PointingHandCursor);
            button->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:none;padding:0;}"
                "QToolButton:hover{background:#222326;border-radius:2px;}"));
            QObject::connect(button, &QToolButton::clicked, spin, step);
            return button;
        };
        stepLayout->addWidget(makeStep(QStringLiteral("customStepUp"), Icon::ChevronUp, &QSpinBox::stepUp));
        stepLayout->addWidget(makeStep(QStringLiteral("customStepDown"), Icon::ChevronDown, &QSpinBox::stepDown));
        row->addWidget(steps, 0, Qt::AlignVCenter);
        out = spin;
        return field;
    };
    auto makeLetter = [popup](const QString &name, const QString &text) {
        auto *label = new QLabel(text, popup);
        label->setObjectName(name);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedWidth(12);
        label->setStyleSheet(QStringLiteral(
            "QLabel{color:#9c9da1;background:transparent;font-size:12px;}"));
        return label;
    };
    QFrame *xField = makeField(QStringLiteral("customX"), -999999, 999999, m_customX);
    QFrame *yField = makeField(QStringLiteral("customY"), -999999, 999999, m_customY);
    QFrame *wField = makeField(QStringLiteral("customWidth"), 4, 99999, m_customWidth);
    QFrame *hField = makeField(QStringLiteral("customHeight"), 4, 99999, m_customHeight);
    QLabel *xLetter = makeLetter(QStringLiteral("customXLetter"), QStringLiteral("X"));
    QLabel *yLetter = makeLetter(QStringLiteral("customYLetter"), QStringLiteral("Y"));
    QLabel *wLetter = makeLetter(QStringLiteral("customWidthLetter"), QStringLiteral("W"));
    QLabel *hLetter = makeLetter(QStringLiteral("customHeightLetter"), QStringLiteral("H"));
    m_customX->setAccessibleName(tr("区域横向位置"));
    m_customY->setAccessibleName(tr("区域纵向位置"));
    m_customWidth->setAccessibleName(tr("区域宽度"));
    m_customHeight->setAccessibleName(tr("区域高度"));
    auto *positionRow = new QHBoxLayout;
    positionRow->setSpacing(8);
    auto *positionLabel = new QLabel(tr("位置"), popup);
    positionLabel->setObjectName(QStringLiteral("customSection"));
    positionRow->addWidget(positionLabel);
    positionRow->addWidget(xLetter);
    positionRow->addWidget(xField);
    positionRow->addWidget(yLetter);
    positionRow->addWidget(yField);

    auto *sizeRow = new QHBoxLayout;
    sizeRow->setSpacing(8);
    auto *sizeLabel = new QLabel(tr("大小"), popup);
    sizeLabel->setObjectName(QStringLiteral("customSection"));
    sizeRow->addWidget(sizeLabel);
    sizeRow->addWidget(wLetter);
    sizeRow->addWidget(wField);
    sizeRow->addWidget(hLetter);
    sizeRow->addWidget(hField);

    auto *presetRow = new QHBoxLayout;
    presetRow->setSpacing(8);
    auto *presetLabel = new QLabel(tr("预设"), popup);
    presetLabel->setObjectName(QStringLiteral("customSection"));
    const int sectionWidth = qMax(30, qMax(positionLabel->sizeHint().width(),
                                            qMax(sizeLabel->sizeHint().width(), presetLabel->sizeHint().width())));
    positionLabel->setFixedWidth(sectionWidth);
    sizeLabel->setFixedWidth(sectionWidth);
    presetLabel->setFixedWidth(sectionWidth);
    m_presetCombo = new CenteredCombo(popup);
    m_presetCombo->setObjectName(QStringLiteral("customPreset"));
    m_presetCombo->setAccessibleName(tr("常用尺寸和已保存区域"));
    m_presetCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_presetCombo->setFixedSize(160, 32);
    m_presetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_presetCombo->setMinimumContentsLength(8);
    m_presetCombo->setMaxVisibleItems(16);
    auto *savePreset = new QPushButton(tr("保存当前"), popup);
    savePreset->setObjectName(QStringLiteral("customPresetSave"));
    savePreset->setToolTip(tr("保存当前区域为预设"));
    savePreset->setFixedSize(80, 32);
    presetRow->addWidget(presetLabel);
    presetRow->addWidget(m_presetCombo, 1);
    presetRow->addWidget(savePreset);

    auto *divider = new QFrame(popup);
    divider->setObjectName(QStringLiteral("customDivider"));
    divider->setFixedHeight(1);
    layout->addWidget(divider);
    layout->addLayout(positionRow);
    layout->addLayout(sizeRow);
    layout->addLayout(presetRow);

    connect(m_customX, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        if (!m_updatingCustom) customFieldsChanged();
    });
    connect(m_customY, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        if (!m_updatingCustom) customFieldsChanged();
    });
    connect(m_customWidth, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_updatingCustom) return;
        if (m_lockAspect && m_aspect > 0 && m_customHeight) {
            const QSignalBlocker block(m_customHeight);
            m_customHeight->setValue(qMax(4, qRound(value / m_aspect)));
        }
        customFieldsChanged();
    });
    connect(m_customHeight, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_updatingCustom) return;
        if (m_lockAspect && m_aspect > 0 && m_customWidth) {
            const QSignalBlocker block(m_customWidth);
            m_customWidth->setValue(qMax(4, qRound(value * m_aspect)));
        }
        customFieldsChanged();
    });
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!m_updatingCustom && index >= 0) applyCustomPreset(index);
    });
    connect(savePreset, &QPushButton::clicked, this, &CaptureOverlay::saveCurrentPreset);
    connect(popup, &QDialog::finished, this, [this] {
        if (isVisible()) setFocus();
        updateTip();
    });
    loadSizePresets();
    refreshPresetCombo();
    updateCustomSizeInputs();
    styleDialog(popup);
    if (auto *surface = popup->findChild<QWidget *>(QStringLiteral("dialogSurface"))) {
        surface->setStyleSheet(QStringLiteral(
            "QWidget#dialogSurface{background:#18191d;border:1px solid #2b2d33;border-radius:8px;}"
            "QWidget#dialogContent{background:transparent;border:none;}"));
        if (auto *column = surface->layout()) {
            column->setContentsMargins(16, 4, 16, 16);
            column->setSpacing(4);
        }
    }
    popup->show();
    popup->adjustSize();
    const QPoint below = m_sizeBar->mapToGlobal(QPoint(0, m_sizeBar->height()));
    const QScreen *screen = QGuiApplication::screenAt(below);
    const QRect available = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    const int y = below.y() + popup->height() <= available.bottom() + 1 ? below.y()
        : qMax(available.top(), m_sizeBar->mapToGlobal(QPoint()).y() - popup->height());
    popup->move(qBound(available.left(), below.x(), available.right() + 1 - popup->width()), y);
}

void CaptureOverlay::customFieldsChanged()
{
    if (m_updatingCustom || m_sel.isNull() || !m_customX || !m_customY
        || !m_customWidth || !m_customHeight)
        return;
    const QRect global = globalFromUnit(m_customX->value(), m_customY->value(),
                                        m_customWidth->value(), m_customHeight->value(), m_sizeUnit);
    QRect local = localScreenRect(global).intersected(rect());
    if (local.width() < 4 || local.height() < 4) return;
    cancelDetection();
    m_sel = local.normalized();
    if (m_lockAspect && m_sel.height() > 0) m_aspect = qreal(m_sel.width()) / m_sel.height();
    if (m_presetCombo) {
        const QSignalBlocker block(m_presetCombo);
        m_presetCombo->setCurrentIndex(-1);
    }
    m_mode = Adjusting;
    clearHover();
    m_drag = H_None;
    placeToolBars();
    showToolBars();
    updateTip();
    updateInspector();
    update();
}

void CaptureOverlay::applyCustomPreset(int index)
{
    if (index < 0 || index >= (m_presetCombo ? m_presetCombo->count() : 0) || m_sel.isNull())
        return;
    const QVariant data = m_presetCombo->itemData(index);
    QRect global;
    if (data.canConvert<QRect>()) {
        global = data.toRect();
    } else if (data.canConvert<QSize>()) {
        // Common sizes are native pixels; keep the current top-left and convert
        // through the same screen scale used by the px unit.
        const QSize size = data.toSize();
        const QRect native = nativeFromGlobal(globalScreenRect(m_sel));
        global = globalFromUnit(native.x(), native.y(), size.width(), size.height(), SizeUnit::Px);
    } else {
        return;
    }
    QRect local = localScreenRect(global).intersected(rect());
    if (local.width() < 4 || local.height() < 4) return;
    cancelDetection();
    m_sel = local;
    if (m_lockAspect && m_sel.height() > 0) m_aspect = qreal(m_sel.width()) / m_sel.height();
    m_mode = Adjusting;
    clearHover();
    m_drag = H_None;
    updateCustomSizeInputs();
    placeToolBars();
    showToolBars();
    updateTip();
    updateInspector();
    update();
}

void CaptureOverlay::loadSizePresets()
{
    m_sizePresets.clear();
    LensSnipSettings settings;
    for (const QString &entry : settings.value(QStringLiteral("Capture/presets")).toStringList()) {
        const QStringList parts = entry.split(QLatin1Char('|'));
        if (parts.size() != 5) continue;
        const QRect global(parts.at(1).toInt(), parts.at(2).toInt(),
                           parts.at(3).toInt(), parts.at(4).toInt());
        if (global.width() < 4 || global.height() < 4) continue;
        m_sizePresets.append({parts.at(0), global});
        if (m_sizePresets.size() >= 50) break;
    }
}

void CaptureOverlay::saveSizePresets() const
{
    QStringList entries;
    for (const SizePreset &preset : m_sizePresets)
        entries.append(QStringLiteral("%1|%2|%3|%4|%5").arg(preset.name)
                           .arg(preset.global.x()).arg(preset.global.y())
                           .arg(preset.global.width()).arg(preset.global.height()));
    LensSnipSettings()
        .setValue(QStringLiteral("Capture/presets"), entries);
}

void CaptureOverlay::refreshPresetCombo(const QString &selected)
{
    if (!m_presetCombo) return;
    m_updatingCustom = true;
    m_presetCombo->clear();
    for (const QSize &size : kCommonSizePresets) {
        m_presetCombo->addItem(QStringLiteral("%1 × %2 px").arg(size.width()).arg(size.height()),
                               size);
    }
    if (!m_sizePresets.isEmpty()) {
        m_presetCombo->addItem(tr("已保存"));
        if (auto *model = qobject_cast<QStandardItemModel *>(m_presetCombo->model())) {
            if (auto *item = model->item(m_presetCombo->count() - 1)) {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
                item->setForeground(Bar::muted);
            }
        }
        for (const SizePreset &preset : m_sizePresets) {
            m_presetCombo->addItem(preset.name, preset.global);
            if (auto *model = qobject_cast<QStandardItemModel *>(m_presetCombo->model())) {
                if (auto *item = model->item(m_presetCombo->count() - 1))
                    item->setData(true, Qt::UserRole + 1);
            }
        }
    }
    int at = -1;
    if (!selected.isEmpty()) {
        for (int i = 0; i < m_presetCombo->count(); ++i) {
            if (m_presetCombo->itemText(i) == selected
                && m_presetCombo->itemData(i, Qt::UserRole + 1).toBool()) {
                at = i;
                break;
            }
        }
    }
    m_presetCombo->setCurrentIndex(at);
    m_updatingCustom = false;
}

void CaptureOverlay::saveCurrentPreset()
{
    if (m_sel.isNull()) return;
    const QString defaultName = tr("预设 %1").arg(m_sizePresets.size() + 1);
    QInputDialog dialog(m_customSizePopup);
    dialog.setWindowTitle(tr("保存预设"));
    dialog.setLabelText(tr("预设名称"));
    dialog.setTextValue(defaultName);
    dialog.setOkButtonText(tr("保存"));
    dialog.setCancelButtonText(tr("取消"));
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog{background:#18191d;}"
        "QLabel{color:#f4f5f8;font-size:14px;}"
        "QLineEdit{color:#f4f5f8;background:#111216;border:1px solid #777980;border-radius:8px;"
        "padding:0 8px;min-height:30px;font-size:14px;selection-background-color:#5e6ad2;}"
        "QLineEdit:focus{border-color:#828fff;}"
        "QPushButton{color:#f4f5f8;background:#222326;border:none;border-radius:8px;"
        "padding:0 12px;min-height:30px;font-size:14px;text-align:center;}"
        "QPushButton:hover{background:#2b2d33;}"));
    applyDarkWindowChrome(&dialog);
    if (dialog.exec() == QDialog::Accepted) saveSizePreset(dialog.textValue());
}

void CaptureOverlay::saveSizePreset(const QString &name)
{
    QString clean = name.trimmed();
    clean.replace(QLatin1Char('|'), QLatin1Char(' '));
    if (clean.isEmpty() || m_sel.isNull()) return;
    const QRect global = globalScreenRect(m_sel);
    bool replaced = false;
    for (SizePreset &preset : m_sizePresets)
        if (preset.name == clean) {
            preset.global = global;
            replaced = true;
            break;
        }
    if (!replaced) m_sizePresets.append({clean, global});
    saveSizePresets();
    refreshPresetCombo(clean);
    if (m_customSizePopup) {
        m_customSizePopup->raise();
        m_customSizePopup->activateWindow();
    }
}

void CaptureOverlay::updateRefreshIcon()
{
    if (!m_refreshButton) return;
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(qCeil(Bar::icon * dpr), qCeil(Bar::icon * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.translate(Bar::icon / 2.0, Bar::icon / 2.0);
    p.rotate(m_refreshSpinAngle);
    p.drawPixmap(QPointF(-Bar::icon / 2.0, -Bar::icon / 2.0),
                 barIcon(Icon::Refresh).pixmap(QSize(Bar::icon, Bar::icon)));
    p.end();
    m_refreshButton->setIcon(QIcon(pm));
}

void CaptureOverlay::beginDesktopPreview()
{
    if (m_embedded || m_pinEdit || m_refreshing) return;
    m_desktopPreview = true;
    if (m_refreshButton) {
        const QSignalBlocker lock(m_refreshButton);
        m_refreshButton->setChecked(true);
    }
    cancelDetection();
    if (m_detector) m_detector->stop();
    if (m_sizePopup) m_sizePopup->close();
    clearHover();
    m_sizeBar->hide();
    m_inspector->hide();
    m_toolOptions->hide();
    if (m_refreshAnimation) m_refreshAnimation->start();
    update();
}

void CaptureOverlay::refreshScreenshot()
{
    if (m_embedded || m_pinEdit || m_refreshing) return;
    m_refreshing = true;
    if (m_refreshAnimation) m_refreshAnimation->stop();
    m_refreshSpinAngle = 0;
    if (m_refreshButton) {
        m_refreshButton->setIcon(barIcon(Icon::Refresh));
        m_refreshButton->setChecked(false);
    }
    cancelDetection();
    cancelAutoMosaicDetection();
    if (m_detector) m_detector->stop();
    if (m_sizePopup) m_sizePopup->close();
    if (m_colorDialog) m_colorDialog->close();
    m_toolOptions->hide();
    m_inspector->hide();
    m_sizeBar->hide();
    const bool customVisible = m_customSizePopup && m_customSizePopup->isVisible();
    if (customVisible) m_customSizePopup->hide();

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    // Excluding the capture window keeps the frame visible while the desktop
    // underneath is grabbed, so the selection never flashes away.
    const bool excluded = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    if (excluded) {
        m_desktopPreview = true;
        for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
            if (bar && bar->layout() && bar->layout()->count() > 0) bar->show();
        update();
    } else {
        m_desktopPreview = false;
        hide();
    }
    QTimer::singleShot(150, this, [this, hwnd, excluded, customVisible] {
        if (!m_refreshing) {
            SetWindowDisplayAffinity(hwnd, WDA_NONE);
            if (customVisible && m_customSizePopup) m_customSizePopup->show();
            return;
        }
        const QVector<ScreenFrame> frames = m_frameSource ? m_frameSource() : freezeScreens();
        SetWindowDisplayAffinity(hwnd, WDA_NONE);
        if (frames.isEmpty()) {
            m_refreshing = false;
            m_desktopPreview = false;
            show(); raise(); activateWindow(); setFocus();
            if (!m_sel.isNull()) {
                placeToolBars();
                showToolBars();
                updateTip();
                updateInspector();
                // Keep snapping alive: resetting the selection later must not
                // leave a stopped detector behind.
                if (m_liveDetection) startDetector();
            } else if (m_liveDetection) {
                startDetector();
                requestDetection(mapFromGlobal(QCursor::pos()));
            }
            if (customVisible && m_customSizePopup) m_customSizePopup->show();
            showOutputError(tr("刷新失败，请重试"));
            update();
            return;
        }
        applyRefreshFrames(frames);
        m_refreshing = false;
        m_desktopPreview = false;
        if (!excluded) show();
        raise(); activateWindow(); setFocus();
        if (!m_sel.isNull()) {
            placeToolBars();
            showToolBars();
            updateTip();
            updateInspector();
            if (m_liveDetection) startDetector();   // Same as the failure branch above.
        } else if (m_liveDetection) {
            startDetector();
            requestDetection(mapFromGlobal(QCursor::pos()));
        }
        if (customVisible && m_customSizePopup) m_customSizePopup->show();
        update();
    });
}

void CaptureOverlay::applyRefreshFrames(const QVector<ScreenFrame> &frames)
{
    if (editorActive()) commitTextEdit();
    if (m_live) { m_live = false; commitAnnotation(); }
    m_frames = frames;
    buildShot(frames);
    m_mosaicFrame = QImage();
    // Refresh replaces the frozen pixels only: keep the committed selection,
    // annotations, history and armed tool exactly as the user left them.
    m_pressSel = QRect();
    clearHover();
    m_hoverHistory.clear();
    m_windowRects.clear();
    m_mode = m_sel.isNull() ? Selecting : Adjusting;
    m_drag = H_None;
    m_itemDrag = H_None;
    m_dragStarted = false;
    m_queryId = 0;
    m_queryPending = m_waitingClick = false;
    m_pinAfterDetection = false;
    m_detectionPoint = QPoint();
    syncToolButtons();
    syncUndoButtons();
    if (m_colorDialog) m_colorDialog->close();
    update();
}

QPixmap CaptureOverlay::cropped() const
{
    if (m_sel.isEmpty() || m_shot.isNull()) return {};
    qreal scale = m_shot.devicePixelRatio();
    const QRect r = m_sel.normalized();
    QPixmap pix;
    // A selection inside one screen keeps that screen's native pixels when the
    // desktop mixes DPI. The combined shot uses the largest DPR, which would
    // upscale lower-DPI screens.
    qreal firstScale = 0;
    bool mixedDpi = false;
    for (const ScreenFrame &frame : m_frames) {
        if (frame.logicalRect.isEmpty() || frame.image.size() != frame.pixelRect.size()) continue;
        const qreal frameScale = qreal(frame.pixelRect.width()) / frame.logicalRect.width();
        if (firstScale <= 0) firstScale = frameScale;
        else if (qAbs(frameScale - firstScale) > 0.01) mixedDpi = true;
    }
    const QRect global = globalScreenRect(r);
    if (mixedDpi) {
        for (const ScreenFrame &frame : m_frames) {
            if (!global.isNull() && frame.logicalRect.contains(global)
                && frame.image.size() == frame.pixelRect.size()
                && !frame.logicalRect.isEmpty()) {
                const qreal sx = qreal(frame.pixelRect.width()) / frame.logicalRect.width();
                const qreal sy = qreal(frame.pixelRect.height()) / frame.logicalRect.height();
                const QRect native(qRound(frame.pixelRect.left() + (global.left() - frame.logicalRect.left()) * sx),
                                   qRound(frame.pixelRect.top() + (global.top() - frame.logicalRect.top()) * sy),
                                   qRound(global.width() * sx), qRound(global.height() * sy));
                if (native.width() > 0 && native.height() > 0) {
                    // ScreenFrame.image starts at the screen's native origin; pixelRect
                    // is the global desktop origin, so remove it before copying.
                    pix = QPixmap::fromImage(frame.image.copy(native.translated(-frame.pixelRect.topLeft())));
                    if (!pix.isNull()) {
                        pix.setDevicePixelRatio(1);
                        scale = qreal(pix.width()) / (std::max)(1, r.width());
                        break;
                    }
                }
            }
        }
    }
    if (pix.isNull()) {
        pix = m_shot.copy(QRect(qRound(r.x() * scale), qRound(r.y() * scale),
                                qRound(r.width() * scale), qRound(r.height() * scale)));
        pix.setDevicePixelRatio(1);
    }
    const bool rounded = m_roundEnabled && m_roundRadius > 0 && !m_embedded && !m_pinEdit;
    if (rounded) {
        // Render through a rounded clip so save/copy/pin keep transparent corners.
        QImage out(pix.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        const qreal radius = m_roundRadius * scale;
        clip.addRoundedRect(QRectF(0, 0, out.width(), out.height()), radius, radius);
        p.setClipPath(clip);
        p.drawPixmap(0, 0, pix);
        if (!m_anns.isEmpty() || editorActive())
            paintAnnotations(p, -QPointF(r.topLeft()) * scale, scale, QRectF(r));
        p.end();
        pix = QPixmap::fromImage(out);
    } else if (!m_anns.isEmpty() || editorActive()) {   // Unfinished text is also included in previews.
        QPainter p(&pix);
        paintAnnotations(p, -QPointF(r.topLeft()) * scale, scale, QRectF(r));
    }
    return pix;
}

void CaptureOverlay::armTool(ATool tool)          // Embedded pins arm tools from their own toolbar.
{
    if (tool == ATool::None || tool == m_atool) {
        stopItemDrag();
        m_selAnn = -1;   // Leaving the tool leaves no selection handles on the pin.
        setToolNone();
    } else selectTool(tool);
    if (optionsTool() == ATool::None) {
        m_toolOptions->hide();
    } else {
        m_toolOptions->adjustSize();
        m_toolOptions->show();
        m_toolOptions->raise();
    }
    emit stripRebuilt();
    setFocus(Qt::OtherFocusReason);   // Esc ladder and shortcuts reach the canvas immediately.
}

QPixmap CaptureOverlay::composited() const
{
    QPixmap pix = m_shot.copy(m_shot.rect());
    pix.setDevicePixelRatio(1);
    if (!m_anns.isEmpty() || editorActive()) {
        QPainter p(&pix);
        paintAnnotations(p, QPointF(0, 0), 1.0, QRectF(rect()), false);
    }
    return pix;
}

void CaptureOverlay::setImage(const QImage &image)
{
    if (image.isNull()) return;
    m_shot = QPixmap::fromImage(image);
    m_mosaicFrame = QImage();   // Mosaic samples the refreshed pixels.
    m_autoDetectedRect = {};    // Refreshed pixels invalidate the cached candidates.
    if (m_embedded) m_sel = rect();   // Marks clip to the whole pin at its current size.
    update();
}

void CaptureOverlay::clearAll()
{
    cancelAutoMosaicDetection();
    m_autoDetectedRect = {};
    m_anns.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_selAnn = -1;
    syncUndoButtons();
    update();
}

void CaptureOverlay::ocrCapture() { openTextCard(false); }

void CaptureOverlay::translateCapture() { openTextCard(true); }

void CaptureOverlay::barcodeCapture() { openTextCard(false,true); }

void CaptureOverlay::reportCaptured(const QPixmap &pix, const QRect &globalRegion)
{
    if (m_captured || pix.isNull() || globalRegion.isEmpty()) return;
    m_captured = true;
    QPixmap report = pix;
    if (globalRegion.width() > 0)
        report.setDevicePixelRatio(qreal(pix.width()) / globalRegion.width());
    emit captured(report, globalRegion);
}

void CaptureOverlay::openTextCard(bool translate, bool barcode)
{
    stopItemDrag();
    commitTextEdit();
    const QImage image = cropped().toImage();
    if (image.isNull()) {
        showOutputError(barcode ? tr("识别失败，请先选择有效截图区域")
                                : translate ? tr("翻译失败，请先选择有效截图区域")
                                            : tr("识别失败，请先选择有效截图区域"));
        return;
    }
    const QRect captureRect=globalScreenRect(m_sel.normalized());
    cancelDetection();
    if (m_colorDialog) m_colorDialog->close();
    for (QWidget *bar : {m_tools, m_toolsTop, m_toolsLeft, m_toolsRight})
        if (bar) bar->hide();
    m_toolOptions->hide(); m_inspector->hide();
    hide();
    auto *dialog = barcode ? showBarcode(image,this,captureRect)
                           : translate ? showTranslate(image,this,captureRect) : showOcr(image,this,captureRect);
    if (!dialog) {
        show(); showToolBars(); update();
        return;
    }
    reportCaptured(QPixmap::fromImage(image), captureRect);
    m_ocrDialog=dialog;
    connect(dialog,&QDialog::finished,this,[this,dialog] {
        if (dialog->returnedToSource()) {
            // Back restores the live selection and its toolbars instead of ending the capture.
            m_ocrDialog=nullptr;
            show(); raise(); activateWindow();
            showToolBars(); update();
            return;
        }
        QTimer::singleShot(0,this,&QWidget::close);
    });
}

void CaptureOverlay::undoAnnotation()
{
    stopItemDrag();
    commitTextEdit();
    if (m_undoStack.isEmpty()) return;
    m_redoStack.append(annotationSnapshot());
    m_anns = m_undoStack.takeLast();
    ++m_smartEpoch;                              // Restored indexes must not receive stale erases.
    if (!autoMosaicAnnotation()) m_autoDetectedRect = {};   // Undoing the scan allows a fresh one.
    m_selAnn = -1;
    normalizeNumbers();
    syncUndoButtons();
    updateToolOptions();
    update();
}

void CaptureOverlay::redoAnnotation()
{
    stopItemDrag();
    commitTextEdit();
    if (m_redoStack.isEmpty()) return;
    m_undoStack.append(annotationSnapshot());
    m_anns = m_redoStack.takeLast();
    ++m_smartEpoch;                              // Restored indexes must not receive stale erases.
    m_selAnn = -1;
    normalizeNumbers();
    syncUndoButtons();
    updateToolOptions();
    update();
}

void CaptureOverlay::saveCapture()
{
    commitTextEdit();
    if (m_sel.isEmpty()) return;
    ensureAutoWatermark();
    const QImage image = cropped().toImage();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    ExportDialog dialog(image, writableImageFormats(), dir,
                        QStringLiteral("LensSnip_{{yyyy-MM-dd HH:mm:ss}}"), this);
    if (dialog.exec() != QDialog::Accepted) {
        activateWindow();                       // Cancelled card: reclaim focus and keep the selection.
        setFocus();
        return;
    }
    const ExportRequest request = dialog.request();
    saveCaptureTo(request.path, {request.format, request.quality, request.scalePercent});
}

bool CaptureOverlay::saveCaptureTo(const QString &path, const ImageSaveOptions &options)
{
    ensureAutoWatermark();
    const QPixmap pix = cropped();
    if (!saveImageAtomic(pix.toImage(), path, options)) {
        showOutputError(tr("保存失败，请检查路径或磁盘空间后重试"));
        return false;
    }
    reportCaptured(pix, globalScreenRect(m_sel.normalized()));
    close();
    return true;
}

void CaptureOverlay::copyCapture()
{
    commitTextEdit();
    ensureAutoWatermark();
    const QPixmap pix = cropped();
    if (!copyImageToClipboard(pix.toImage())) {
        showOutputError(tr("复制失败，剪贴板可能被占用，请重试"));
        return;
    }
    reportCaptured(pix, globalScreenRect(m_sel.normalized()));
    close();
}

void CaptureOverlay::showOutputError(const QString &message)
{
    m_sizeTip->setText(message);
    tipPosition(m_sel.center());
    m_sizeBar->show();
    activateWindow();
    setFocus();
}

void CaptureOverlay::pinCapture()
{
    commitTextEdit();
    ensureAutoWatermark();
    const QPixmap pix = cropped();
    if (pix.isNull()) {
        showOutputError(tr("贴图失败，请确认选区有效并检查内存后重试"));
        return;
    }
    const QRect global = globalScreenRect(m_sel.normalized());
    reportCaptured(pix, global);
    close();
    emit grabbed(pix, global);
}

void CaptureOverlay::longCapture()
{
    commitTextEdit();
    const QRect global = globalScreenRect(m_sel);
    close();
    emit longRequested(global);
}
