#include "shared/export_dialog.h"

#include "shared/settings.h"
#include "shared/time_tokens.h"
#include "shared/widgets.h"

#include <QBuffer>
#include <QButtonGroup>
#include <QCursor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

class FieldFrame : public QFrame
{
public:
    explicit FieldFrame(QWidget *parent = nullptr) : QFrame(parent) { setFrameShape(QFrame::NoFrame); }

    void setRing(bool ring)
    {
        if (m_ring == ring) return;
        m_ring = ring;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(DialogTheme::border, 1));
        painter.setBrush(DialogTheme::field);
        painter.drawRoundedRect(box, 8, 8);
        if (m_ring) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Bar::focusRing, 2));
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        }
    }

private:
    bool m_ring = false;
};

// Qt has no :focus-visible; keep the segment ring for keyboard focus only.
class SegmentButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void focusInEvent(QFocusEvent *event) override
    {
        QToolButton::focusInEvent(event);
        const auto reason = event->reason();
        setProperty("kbdFocus", reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason
                                || reason == Qt::ShortcutFocusReason);
        style()->unpolish(this);
        style()->polish(this);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QToolButton::focusOutEvent(event);
        setProperty("kbdFocus", false);
        style()->unpolish(this);
        style()->polish(this);
    }
};

class FieldFocusFilter : public QObject
{
public:
    FieldFocusFilter(FieldFrame *frame, QObject *parent = nullptr)
        : QObject(parent), m_frame(frame) {}

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::FocusIn) {
            const auto reason = static_cast<QFocusEvent *>(event)->reason();
            m_frame->setRing(reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason
                             || reason == Qt::ShortcutFocusReason);
        } else if (event->type() == QEvent::FocusOut) {
            m_frame->setRing(false);
        }
        return false;
    }

private:
    FieldFrame *m_frame;
};

class ExportSlider : public QSlider
{
public:
    ExportSlider(int wheelStep, QWidget *parent = nullptr)
        : QSlider(Qt::Horizontal, parent), m_step(wheelStep)
    {
        setProperty("exportSlider", true);
        setFixedHeight(24);
        setPageStep(10);
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        if (!isEnabled() || !underMouse()) {
            event->ignore();
            return;
        }
        const int delta = event->angleDelta().y();
        if (delta == 0) {
            event->ignore();
            return;
        }
        setValue(qBound(minimum(), maximum(), value() + (delta > 0 ? m_step : -m_step)));
        event->accept();
    }

private:
    int m_step = 1;
};

QString formatLabel(const QString &format)
{
    if (format == QLatin1String("webp")) return QStringLiteral("WebP");
    return format.toUpper();
}

QString readableSize(qint64 bytes)
{
    if (bytes >= 1024 * 1024)
        return QString::number(bytes / 1024.0 / 1024.0, 'f', 1) + QStringLiteral(" MB");
    return QString::number(qMax<qint64>(1, qRound64(bytes / 1024.0))) + QStringLiteral(" KB");
}

} // namespace

ExportDialog::ExportDialog(const QImage &image, const QStringList &formats,
                           const QString &defaultDirectory, const QString &defaultName,
                           QWidget *parent)
    : QDialog(parent), m_image(image), m_defaultName(defaultName)
{
    setWindowTitle(tr("保存"));
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    for (const QString &format : formats) {
        QString normalized = format.trimmed().toLower();
        if (normalized == QLatin1String("jpeg")) normalized = QStringLiteral("jpg");
        if (normalized != QLatin1String("png") && normalized != QLatin1String("jpg")
            && normalized != QLatin1String("webp") && normalized != QLatin1String("bmp")) continue;
        if (!m_formats.contains(normalized)) m_formats.append(normalized);
    }
    if (m_formats.isEmpty()) m_formats.append(QStringLiteral("png"));
    if (!m_formats.contains(QStringLiteral("png"))) m_formats.prepend(QStringLiteral("png"));

    LensSnipSettings settings;
    const QString last = settings.value(QStringLiteral("Output/LastFormat"),
                                        m_formats.first()).toString();
    m_format = m_formats.contains(last) ? last : m_formats.first();
    for (const QString &format : m_formats)
        m_qualities[format] = qBound(0, settings.value(QStringLiteral("Output/Quality_") + format, 90).toInt(), 100);

    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
    const bool shortScreen = available.height() < 420;
    m_shadow = shortScreen ? 8 : 24;
    m_cardWidth = qMin(440, qMax(360, available.width() - 2 * m_shadow - 32));
    m_padding = (available.width() < 440 + 2 * m_shadow + 32) ? 16 : 20;

    QString directory = settings.value(QStringLiteral("Output/LastDir"), defaultDirectory).toString();
    if (directory.isEmpty()) directory = defaultDirectory;
    if (directory.isEmpty()) directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);

    buildUi();
    m_path->setText(QDir::toNativeSeparators(directory));
    m_path->setCursorPosition(0);
    m_name->setText(m_defaultName.isEmpty() ? QStringLiteral("LensSnip_{{yyyy-MM-dd HH:mm:ss}}")
                                            : m_defaultName);
    setFormat(m_format);
    refreshQualityRow();
    refreshEstimate();
    updateNamePreview();

    setFixedWidth(m_cardWidth + 2 * m_shadow);
    adjustSize();
    QPoint position = available.center() - QPoint(width() / 2, height() / 2);
    position.setX(qBound(available.left(), position.x(),
                         qMax(available.left(), available.right() + 1 - width())));
    position.setY(qBound(available.top(), position.y(),
                         qMax(available.top(), available.bottom() + 1 - height())));
    move(position);

    if (QAbstractButton *current = m_formatGroup->checkedButton())
        current->setFocus(Qt::OtherFocusReason);
}

void ExportDialog::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(m_shadow, m_shadow, m_shadow, m_shadow);
    outer->setSpacing(0);

    auto *surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("exportSurface"));
    surface->setAttribute(Qt::WA_StyledBackground, true);
    outer->addWidget(surface);

    auto *shadow = new QGraphicsDropShadowEffect(surface);
    shadow->setBlurRadius(48);
    shadow->setOffset(0, 16);
    shadow->setColor(DialogTheme::shadow);
    surface->setGraphicsEffect(shadow);

    QString qss = QStringLiteral(
        "QWidget#exportSurface{background:%1;border:1px solid %2;border-radius:12px;}"
        "QLabel{color:%3;}"
        "QLabel#exportTitle{font-size:16px;font-weight:600;}"
        "QLabel#exportLabel{color:%4;font-size:14px;font-weight:500;}"
        "QLabel[exportValue=\"true\"]{color:%3;font-size:14px;font-weight:500;}"
        "QLabel#exportEstimate{color:%5;font-size:12px;}"
        "QLabel#exportSuffix{color:%4;background:rgba(255,255,255,20);border-radius:4px;"
        "padding:2px 6px;font-size:13px;}"
        "QFrame#exportSegments{background:%6;border:1px solid %7;border-radius:9px;}"
        "QToolButton#exportSegment{background:transparent;color:%5;border:1px solid transparent;"
        "border-radius:6px;font-size:13px;font-weight:500;}"
        "QToolButton#exportSegment:hover{background:%8;color:%4;}"
        "QToolButton#exportSegment:checked{background:#2c2d30;color:%3;"
        "border:1px solid rgba(255,255,255,26);}"
        "QToolButton#exportSegment[kbdFocus=\"true\"]:focus{border:2px solid %9;}"
        "QToolButton#exportSegment[kbdFocus=\"false\"]:focus{border:1px solid transparent;}"
        "QToolButton#exportSegment[kbdFocus=\"false\"]:checked:focus{"
        "border:1px solid rgba(255,255,255,26);}"
        "QToolButton#exportClose{background:transparent;border:none;border-radius:6px;}"
        "QToolButton#exportClose:hover{background:%8;}"
        "QToolButton#exportClose:focus{border:2px solid %9;}"
        "QToolButton#exportBrowse{background:transparent;border:none;border-radius:6px;}"
        "QToolButton#exportBrowse:hover{background:%8;}"
        "QToolButton#exportBrowse:focus{border:2px solid %9;}"
        "QLineEdit{background:transparent;border:none;padding:0;"
        "selection-background-color:%10;selection-color:#ffffff;}"
        "QLineEdit#exportName{color:%3;font-size:14px;}"
        "QLineEdit#exportPath{color:%4;font-size:13px;}")
        .arg(DialogTheme::surface.name(), DialogTheme::strongBorder.name(QColor::HexArgb),
             DialogTheme::ink.name(), DialogTheme::secondary.name(), DialogTheme::muted.name(),
             DialogTheme::field.name(), DialogTheme::border.name(QColor::HexArgb),
             DialogTheme::hover.name(), Bar::focusRing.name(), Bar::accent.name());
    qss += QStringLiteral(
        "QPushButton#exportCancel{color:%1;background:transparent;border:none;border-radius:8px;"
        "padding:0 12px;font-size:14px;font-weight:500;}"
        "QPushButton#exportCancel:hover{background:%2;color:%3;}"
        "QPushButton#exportCancel:pressed{background:%4;}"
        "QPushButton#exportCancel:focus{border:2px solid %5;padding:0 10px;}"
        "QPushButton#exportSave{color:%6;background:%7;border:none;border-radius:8px;"
        "padding:0 12px;font-size:14px;font-weight:500;}"
        "QPushButton#exportSave:hover{background:#ffffff;}"
        "QPushButton#exportSave:pressed{background:%8;}"
        "QPushButton#exportSave:focus{border:2px solid %5;padding:0 10px;}"
        "QSlider[exportSlider=\"true\"]::groove:horizontal{height:4px;background:%9;border:none;"
        "border-radius:2px;}"
        "QSlider[exportSlider=\"true\"]::sub-page:horizontal{background:%3;border-radius:2px;}"
        "QSlider[exportSlider=\"true\"]::handle:horizontal{width:14px;height:14px;"
        "margin:-5px 0;border-radius:7px;background:#ffffff;}"
        "QSlider[exportSlider=\"true\"]::sub-page:horizontal:disabled{background:%3;}"
        "QSlider[exportSlider=\"true\"]::handle:horizontal:disabled{background:%10;}"
        "QSlider[exportSlider=\"true\"]::groove:horizontal:disabled{background:%9;}")
        .arg(DialogTheme::secondary.name(), DialogTheme::hover.name(), DialogTheme::ink.name(),
             DialogTheme::pressed.name(), Bar::focusRing.name(), Bar::primaryInk.name(),
             Bar::primaryFill.name(), DialogTheme::primaryPressed.name(),
             QStringLiteral("#6b6e77"), DialogTheme::muted.name());
    setStyleSheet(qss);

    auto *layout = new QVBoxLayout(surface);
    layout->setContentsMargins(m_padding + 1, m_padding + 1, m_padding + 1, m_padding + 1);
    layout->setSpacing(0);

    auto *titleRow = new QWidget(surface);
    titleRow->setObjectName(QStringLiteral("exportTitleRow"));
    titleRow->setFixedHeight(28);
    auto *titleLine = new QHBoxLayout(titleRow);
    titleLine->setContentsMargins(0, 0, 0, 0);
    titleLine->setSpacing(8);
    auto *title = new QLabel(tr("保存"), titleRow);
    title->setObjectName(QStringLiteral("exportTitle"));
    titleLine->addWidget(title);
    titleLine->addStretch();
    auto *close = new QToolButton(titleRow);
    close->setObjectName(QStringLiteral("exportClose"));
    close->setIcon(subtleIcon(Icon::CardClose, 20));
    close->setIconSize(QSize(20, 20));
    close->setFixedSize(28, 28);
    close->setCursor(Qt::PointingHandCursor);
    close->setAccessibleName(tr("关闭 (Esc)"));
    close->setToolTip(tr("关闭 (Esc)"));
    connect(close, &QToolButton::clicked, this, &QDialog::reject);
    titleLine->addWidget(close);
    layout->addWidget(titleRow);

    const auto addRow = [this, surface, layout](const QString &text, QWidget *control,
                                                int topSpacing, const QString &rowName) {
        if (topSpacing > 0) layout->addSpacing(topSpacing);
        auto *row = new QWidget(surface);
        row->setObjectName(rowName);
        row->setFixedHeight(32);
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(12);
        auto *label = new QLabel(text, row);
        label->setObjectName(QStringLiteral("exportLabel"));
        label->setFixedWidth(48);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        line->addWidget(label);
        line->addWidget(control, 1);
        layout->addWidget(row);
        return label;
    };

    auto *segments = new QFrame(surface);
    segments->setObjectName(QStringLiteral("exportSegments"));
    segments->setFixedHeight(32);
    auto *segmentLine = new QHBoxLayout(segments);
    segmentLine->setContentsMargins(2, 2, 2, 2);
    segmentLine->setSpacing(2);
    m_formatGroup = new QButtonGroup(this);
    m_formatGroup->setExclusive(true);
    for (int i = 0; i < m_formats.size(); ++i) {
        auto *button = new SegmentButton(segments);
        button->setObjectName(QStringLiteral("exportSegment"));
        button->setProperty("kbdFocus", false);
        button->setText(formatLabel(m_formats[i]));
        button->setCheckable(true);
        button->setFixedHeight(26);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_formatGroup->addButton(button, i);
        segmentLine->addWidget(button);
    }
    connect(m_formatGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (id >= 0 && id < m_formats.size()) setFormat(m_formats[id]);
    });
    addRow(tr("格式"), segments, 16, QStringLiteral("formatRow"));

    auto *qualityControl = new QWidget(surface);
    qualityControl->setObjectName(QStringLiteral("qualityControl"));
    qualityControl->setFixedHeight(32);
    auto *qualityLine = new QHBoxLayout(qualityControl);
    qualityLine->setContentsMargins(0, 0, 0, 0);
    qualityLine->setSpacing(12);
    m_quality = new ExportSlider(1, qualityControl);
    m_quality->setObjectName(QStringLiteral("qualitySlider"));
    m_quality->setRange(0, 100);
    qualityLine->addWidget(m_quality, 1);
    m_qualityValue = new QLabel(qualityControl);
    m_qualityValue->setObjectName(QStringLiteral("qualityValue"));
    m_qualityValue->setProperty("exportValue", true);
    m_qualityValue->setFixedWidth(40);
    m_qualityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    qualityLine->addWidget(m_qualityValue);
    auto *qualityLabel = addRow(tr("质量"), qualityControl, 16, QStringLiteral("qualityRow"));
    qualityLabel->setBuddy(m_quality);
    connect(m_quality, &QSlider::valueChanged, this, [this](int value) {
        if (!m_lossy) return;
        m_qualities[m_format] = value;
        m_qualityValue->setText(QString::number(value));
        scheduleEstimate();
    });

    auto *scaleControl = new QWidget(surface);
    scaleControl->setObjectName(QStringLiteral("scaleControl"));
    scaleControl->setFixedHeight(32);
    auto *scaleLine = new QHBoxLayout(scaleControl);
    scaleLine->setContentsMargins(0, 0, 0, 0);
    scaleLine->setSpacing(12);
    m_scale = new ExportSlider(5, scaleControl);
    m_scale->setObjectName(QStringLiteral("scaleSlider"));
    m_scale->setRange(10, 100);
    m_scale->setValue(qBound(10, LensSnipSettings().value(QStringLiteral("Output/Scale"), 100).toInt(), 100));
    scaleLine->addWidget(m_scale, 1);
    m_scaleValue = new QLabel(scaleControl);
    m_scaleValue->setObjectName(QStringLiteral("scaleValue"));
    m_scaleValue->setProperty("exportValue", true);
    m_scaleValue->setFixedWidth(40);
    m_scaleValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    scaleLine->addWidget(m_scaleValue);
    auto *scaleLabel = addRow(tr("缩放"), scaleControl, 16, QStringLiteral("scaleRow"));
    scaleLabel->setBuddy(m_scale);
    connect(m_scale, &QSlider::valueChanged, this, [this](int value) {
        m_scaleValue->setText(QString::number(value) + QStringLiteral("%"));
        scheduleEstimate();
    });

    auto *nameField = new FieldFrame(surface);
    nameField->setObjectName(QStringLiteral("nameField"));
    nameField->setFixedHeight(32);
    auto *nameLine = new QHBoxLayout(nameField);
    nameLine->setContentsMargins(10, 0, 4, 0);
    nameLine->setSpacing(6);
    m_name = new QLineEdit(nameField);
    m_name->setObjectName(QStringLiteral("exportName"));
    m_name->setFrame(false);
    nameLine->addWidget(m_name, 1);
    m_suffixLabel = new QLabel(nameField);
    m_suffixLabel->setObjectName(QStringLiteral("exportSuffix"));
    m_suffixLabel->setAlignment(Qt::AlignCenter);
    m_suffixLabel->setFixedHeight(22);
    nameLine->addWidget(m_suffixLabel);
    m_name->installEventFilter(new FieldFocusFilter(nameField, m_name));
    auto *nameLabel = addRow(tr("文件名"), nameField, 24, QStringLiteral("nameRow"));
    nameLabel->setBuddy(m_name);
    connect(m_name, &QLineEdit::textChanged, this, [this] { updateNamePreview(); });

    auto *pathField = new FieldFrame(surface);
    pathField->setObjectName(QStringLiteral("pathField"));
    pathField->setFixedHeight(32);
    auto *pathLine = new QHBoxLayout(pathField);
    pathLine->setContentsMargins(10, 0, 4, 0);
    pathLine->setSpacing(6);
    m_path = new QLineEdit(pathField);
    m_path->setObjectName(QStringLiteral("exportPath"));
    m_path->setFrame(false);
    pathLine->addWidget(m_path, 1);
    auto *browse = new QToolButton(pathField);
    browse->setObjectName(QStringLiteral("exportBrowse"));
    browse->setIcon(subtleIcon(Icon::Folder, 20));
    browse->setIconSize(QSize(20, 20));
    browse->setFixedSize(28, 28);
    browse->setCursor(Qt::PointingHandCursor);
    browse->setToolTip(tr("选择文件夹"));
    browse->setAccessibleName(tr("选择文件夹"));
    pathLine->addWidget(browse);
    m_path->installEventFilter(new FieldFocusFilter(pathField, m_path));
    auto *pathLabel = addRow(tr("保存到"), pathField, 16, QStringLiteral("pathRow"));
    pathLabel->setBuddy(m_path);
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString directory = QFileDialog::getExistingDirectory(
            this, tr("选择文件夹"), m_path->text().trimmed(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!directory.isEmpty()) m_path->setText(QDir::toNativeSeparators(directory));
    });

    layout->addSpacing(24);
    auto *footer = new QWidget(surface);
    footer->setObjectName(QStringLiteral("exportFooter"));
    footer->setFixedHeight(32);
    auto *footerLine = new QHBoxLayout(footer);
    footerLine->setContentsMargins(0, 0, 0, 0);
    footerLine->setSpacing(12);
    m_estimate = new QLabel(footer);
    m_estimate->setObjectName(QStringLiteral("exportEstimate"));
    footerLine->addWidget(m_estimate);
    footerLine->addStretch();
    auto *cancel = new QPushButton(tr("取消"), footer);
    cancel->setObjectName(QStringLiteral("exportCancel"));
    cancel->setFixedHeight(32);
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setAccessibleName(tr("取消"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    footerLine->addWidget(cancel);
    m_save = new QPushButton(tr("保存"), footer);
    m_save->setObjectName(QStringLiteral("exportSave"));
    m_save->setFixedHeight(32);
    m_save->setCursor(Qt::PointingHandCursor);
    m_save->setDefault(true);
    m_save->setAutoDefault(true);
    connect(m_save, &QPushButton::clicked, this, &ExportDialog::submit);
    footerLine->addWidget(m_save);
    layout->addWidget(footer);

    m_estimateTimer = new QTimer(this);
    m_estimateTimer->setSingleShot(true);
    connect(m_estimateTimer, &QTimer::timeout, this, &ExportDialog::refreshEstimate);

    m_scaleValue->setText(QString::number(m_scale->value()) + QStringLiteral("%"));
}

void ExportDialog::setFormat(const QString &format)
{
    if (format.isEmpty() || !m_formats.contains(format)) return;
    if (m_lossy) m_qualities[m_format] = m_quality->value();
    m_format = format;
    m_lossy = format == QLatin1String("jpg") || format == QLatin1String("webp");
    m_suffixLabel->setText(suffixFor(format));
    for (int i = 0; i < m_formats.size(); ++i)
        if (auto *button = m_formatGroup->button(i)) button->setChecked(m_formats[i] == format);
    updateNamePreview();
    refreshQualityRow();
    scheduleEstimate();
}

void ExportDialog::refreshQualityRow()
{
    if (!m_quality || !m_qualityValue) return;
    if (m_lossy) {
        const QSignalBlocker blocker(m_quality);
        m_quality->setEnabled(true);
        m_quality->setValue(m_qualities.value(m_format, 90));
        m_qualityValue->setText(QString::number(m_quality->value()));
        m_qualityValue->setStyleSheet(QStringLiteral("color:%1;").arg(DialogTheme::ink.name()));
    } else {
        const QSignalBlocker blocker(m_quality);
        m_quality->setEnabled(false);
        m_quality->setValue(100);
        m_qualityValue->setText(QStringLiteral("100"));
        m_qualityValue->setStyleSheet(QStringLiteral("color:%1;").arg(DialogTheme::muted.name()));
    }
}

QSize ExportDialog::targetSize() const
{
    if (m_image.isNull() || !m_scale) return m_image.size();
    const int scale = qBound(10, m_scale->value(), 100);
    if (scale >= 100) return m_image.size();
    const int width = qMax(1, qRound(m_image.width() * scale / 100.0));
    const int height = qMax(1, qRound(m_image.height() * double(width) / m_image.width()));
    return QSize(width, height);
}

void ExportDialog::scheduleEstimate()
{
    if (m_estimateTimer) m_estimateTimer->start(150);
}

void ExportDialog::refreshEstimate()
{
    if (!m_estimate) return;
    const QSize size = targetSize();
    QImage scaled = m_image;
    if (size.isValid() && size != m_image.size())
        scaled = m_image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, m_format.toLatin1());
    if (m_lossy) writer.setQuality(m_quality->value());
    const bool ok = !scaled.isNull() && writer.write(scaled);
    buffer.close();
    m_estimate->setText(tr("≈ %1 · %2 × %3")
                            .arg(ok ? readableSize(data.size()) : QStringLiteral("--"))
                            .arg(size.width()).arg(size.height()));
}

void ExportDialog::updateNamePreview()
{
    if (!m_name) return;
    const QString name = resolvedName(m_name->text(), QDateTime::currentDateTime());
    m_name->setToolTip(tr("将保存为 %1").arg(name + suffixFor(m_format)));
}

QString ExportDialog::suffixFor(const QString &format) const
{
    return QStringLiteral(".") + format;
}

QString ExportDialog::composedPath() const
{
    QString directory = m_path->text().trimmed();
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return QDir(directory).filePath(resolvedName(m_name->text(), QDateTime::currentDateTime())
                                    + suffixFor(m_format));
}

int ExportDialog::qualityForSave() const
{
    return m_lossy && m_quality ? m_quality->value() : -1;
}

ExportRequest ExportDialog::request() const
{
    if (m_confirmed) return m_request;
    ExportRequest result;
    result.path = composedPath();
    result.format = m_format;
    result.quality = qualityForSave();
    result.scalePercent = m_scale ? m_scale->value() : 100;
    return result;
}

void ExportDialog::submit()
{
    const QString directory = m_path->text().trimmed();
    if (directory.isEmpty()) {
        QMessageBox::warning(this, tr("保存"), tr("请选择保存文件夹。"));
        return;
    }
    if (!QDir(directory).exists()) {
        QMessageBox::warning(this, tr("保存"), tr("文件夹不存在，请重新选择。"));
        return;
    }
    const QString path = composedPath();
    if (QFileInfo::exists(path)) {
        const auto answer = QMessageBox::question(
            this, tr("保存"), tr("“%1”已存在，是否覆盖？").arg(QFileInfo(path).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    LensSnipSettings settings;
    settings.setValue(QStringLiteral("Output/LastFormat"), m_format);
    if (m_lossy) m_qualities[m_format] = m_quality->value();
    settings.setValue(QStringLiteral("Output/Quality_jpg"), m_qualities.value(QStringLiteral("jpg"), 90));
    settings.setValue(QStringLiteral("Output/Quality_webp"), m_qualities.value(QStringLiteral("webp"), 90));
    settings.setValue(QStringLiteral("Output/Scale"), m_scale->value());
    settings.setValue(QStringLiteral("Output/LastDir"), directory);

    m_request.path = path;
    m_request.format = m_format;
    m_request.quality = qualityForSave();
    m_request.scalePercent = m_scale->value();
    m_confirmed = true;
    accept();
}

QString ExportDialog::resolvedName(const QString &pattern, const QDateTime &stamp)
{
    QString name = pattern.trimmed();
    if (name.isEmpty()) name = QStringLiteral("LensSnip_{{yyyy-MM-dd HH:mm:ss}}");
    name = resolveTimeTokens(name, stamp);
    static const QRegularExpression illegal(QStringLiteral("[\\\\/:*?\"<>|]"));
    name.replace(illegal, QStringLiteral("-"));
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    name.replace(whitespace, QStringLiteral("_"));
    static const QRegularExpression imageSuffix(
        QStringLiteral("\\.(png|jpe?g|webp|bmp)$"), QRegularExpression::CaseInsensitiveOption);
    name.remove(imageSuffix);
    while (!name.isEmpty() && (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))))
        name.chop(1);
    return name.isEmpty() ? QStringLiteral("LensSnip") : name;
}

