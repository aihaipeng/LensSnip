#include <tuple>
#include "text/cards/ocr_dialog.h"
#include "text/engines/translate_gateway.h"
#include "shared/settings.h"
#include <QWindow>
#include <QGuiApplication>
#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QColor>
#include <QFont>
#include <QResizeEvent>
#include <QWidget>
#include <QScreen>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QLabel>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QtMath>
#include <QToolButton>
#include <QAbstractButton>
#include <algorithm>
#include <QAbstractSlider>
#include <QVBoxLayout>
#include <QDialog>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QCursor>
#include <QMessageBox>
#include <QTimer>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QGridLayout>
#include <QTextEdit>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QShortcut>
#include <QKeySequence>
#include <QTextDocument>
#include <QTextBlock>
#include <QSet>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QComboBox>
#include <QSettings>
#include <qt_windows.h>
#include <QApplication>
#include <QPushButton>
#include <QFrame>
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAbstractSlider>
#include <QAbstractScrollArea>
#include <QSplitterHandle>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <atomic>
#include <memory>

// Child-free padding along every card edge; its outer half grabs a resize.
constexpr int ResizeBand = 16;

static QUrl barcodeUrl(const QString &text)
{
    const QUrl url(text.trimmed());
    const QString scheme = url.scheme().toLower();
    if (url.isValid() && (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
                          || scheme == QStringLiteral("ftp")))
        return url;
    return {};
}

QString OcrDialog::barcodeDownloadName(const QUrl &url)
{
    const QString pathName = QUrl::fromPercentEncoding(url.fileName().toUtf8()).trimmed();
    QString name = pathName.isEmpty() ? url.host() : pathName;
    if (name.isEmpty()) name = QStringLiteral("page");
    static const QString invalid = QStringLiteral("<>:\"/\\|?*");
    for (QChar &ch : name)
        if (ch.unicode() < 0x20 || invalid.contains(ch)) ch = QLatin1Char('_');
    name = name.trimmed();
    if (name.isEmpty()) name = QStringLiteral("page");
    if (pathName.isEmpty() || QFileInfo(name).suffix().isEmpty())
        name += QStringLiteral(".html");
    return name;
}

static OcrDialog *presentCard(OcrDialog *dialog, QWidget *source)
{
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (source) QObject::connect(source,&QObject::destroyed,dialog,&QObject::deleteLater);
    // The dialog places its compact loading card and final layout at the screen centre.
    dialog->show();
    dialog->raise(); dialog->activateWindow();
    return dialog;
}

OcrDialog *showOcr(const QImage &image, QWidget *source, const QRect &captureRect)
{
    if (image.isNull()) return nullptr;
    return presentCard(new OcrDialog(image,captureRect), source);
}

OcrDialog *showTranslate(const QImage &image, QWidget *source, const QRect &captureRect)
{
    if (image.isNull()) return nullptr;
    return presentCard(new OcrDialog(image,captureRect,true), source);
}

OcrDialog *showBarcode(const QImage &image, QWidget *source, const QRect &captureRect)
{
    if (image.isNull()) return nullptr;
    return presentCard(new OcrDialog(image,captureRect,false,true), source);
}

OcrImageView::OcrImageView(const QImage &image, QWidget *parent) : QGraphicsView(parent) {
    auto *scene = new QGraphicsScene(this);
    setScene(scene);
    QImage native = image;
    native.setDevicePixelRatio(1);
    scene->addPixmap(QPixmap::fromImage(native));
    // The capture keeps its selection outline, so it reads as the region just framed.
    QPen frame(Bar::accent, 1);
    frame.setCosmetic(true);
    scene->addRect(QRectF(QPointF(), native.size()).adjusted(-0.5,-0.5,0.5,0.5), frame);
    const qreal margin = qMax(native.width(), native.height()) * 0.04 + 8;   // Graphite breathing room.
    m_fit = QRectF(QPointF(), native.size()).adjusted(-margin,-margin,margin,margin);
    // A boundless scene lets the capture be dragged anywhere without scroll bars.
    scene->setSceneRect(m_fit.adjusted(-1e5,-1e5,1e5,1e5));
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHint(QPainter::SmoothPixmapTransform);
    setBackgroundBrush(Bar::content);   // Same near-black as the block it sits on.
    setAccessibleName(tr("截图对照：滚轮缩放，拖动查看"));
}

void OcrImageView::resizeEvent(QResizeEvent *event)  {
    QGraphicsView::resizeEvent(event);
    if (!m_zoomed) fitInView(m_fit, Qt::KeepAspectRatio);
    if (zoomed) zoomed();
}

void OcrTextEdit::wheelEvent(QWheelEvent *event) {
    if (wheelHandler && wheelHandler(event)) {
        event->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(event);
}

void OcrImageView::fit() {
    m_zoomed = false;
    fitInView(m_fit, Qt::KeepAspectRatio);
    if (zoomed) zoomed();
}

int OcrImageView::percent() const {
    // The scene holds native capture pixels; a scale of 1/DPR shows them one-to-one on screen.
    return qRound(transform().m11() * devicePixelRatioF() * 100);
}

void OcrImageView::zoomTo(int value) {
    const qreal target = qBound(2,value,1600) / 100.0 / devicePixelRatioF();
    const QPointF centre = mapToScene(viewport()->rect().center());
    setTransform(QTransform::fromScale(target,target));
    centerOn(centre);
    m_zoomed = true;
    if (zoomed) zoomed();
}

void OcrImageView::wheelEvent(QWheelEvent *event)  {
    if (!event->angleDelta().y()) return;
    const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1/1.2;
    const qreal next = transform().m11() * factor;
    if (next >= 0.02 && next <= 16) {
        // AnchorUnderMouse relies on a prior mouse move; a cursor resting since the card opened anchors at the origin.
        const QPoint pos = event->position().toPoint();
        const QPointF anchor = mapToScene(pos);
        setTransformationAnchor(NoAnchor);
        scale(factor, factor);
        centerOn(mapToScene(viewport()->rect().center()) + anchor - mapToScene(pos));
        m_zoomed = true;
        if (zoomed) zoomed();
    }
    event->accept();
}

void OcrImageView::mouseDoubleClickEvent(QMouseEvent *event)  {
    // On text a double click selects the whole recognized line; elsewhere it fits the capture.
    const int hit = event->button()==Qt::LeftButton ? glyphAt(event->position().toPoint(),false) : -1;
    if (hit >= 0) {
        int first = hit, last = hit;
        while (first > 0 && m_glyphs[first-1].line == m_glyphs[hit].line) --first;
        while (last+1 < m_glyphs.size() && m_glyphs[last+1].line == m_glyphs[hit].line) ++last;
        m_press = -1; m_selecting = false;
        selectRange(first,last);
        event->accept(); return;
    }
    fit();
    event->accept();
}

void OcrImageView::setTextLines(const QVector<OcrLine> &lines) {
    m_textData.clear(); m_glyphs.clear(); m_highlighted.clear(); m_barcodeRegions.clear();
    m_anchor = m_lead = m_press = -1; m_selecting = false;
    viewport()->setMouseTracking(true);   // Hover alone must switch the cursor over text.
    for (int l = 0; l < lines.size(); ++l) {
        const OcrLine &line = lines[l];
        if (!m_textData.isEmpty()) m_textData += QLatin1Char('\n');
        const int base = int(m_textData.size());
        m_textData += line.text;
        if (line.glyphs.size() != line.text.size()) continue;   // Text without geometry stays unselectable.
        for (int c = 0; c < line.text.size(); ++c)
            if (!line.text[c].isSpace() && !line.glyphs[c].boundingRect().isEmpty())
                m_glyphs.append(Glyph{line.glyphs[c], base+c, l});
    }
    viewport()->update();
}

int OcrImageView::glyphAt(const QPoint &viewPos, bool nearest) const {
    // A glyph under the point wins; while dragging the closest glyph box follows the cursor.
    const QPointF at = mapToScene(viewPos);
    int best = -1;
    double bestDistance = std::numeric_limits<double>::max();
    for (int i = 0; i < m_glyphs.size(); ++i) {
        if (m_glyphs[i].polygon.containsPoint(at,Qt::OddEvenFill)) return i;
        if (!nearest) continue;
        const QRectF r = m_glyphs[i].polygon.boundingRect();
        const double dx = at.x()<r.left() ? r.left()-at.x() : at.x()>r.right() ? at.x()-r.right() : 0;
        const double dy = at.y()<r.top() ? r.top()-at.y() : at.y()>r.bottom() ? at.y()-r.bottom() : 0;
        if (dx*dx+dy*dy < bestDistance) { bestDistance = dx*dx+dy*dy; best = i; }
    }
    return best;
}

void OcrImageView::selectRange(int anchor, int lead) {
    const bool changed = m_anchor != anchor || m_lead != lead;
    m_anchor = anchor; m_lead = lead;
    viewport()->update();
    if (changed && selectionChanged) selectionChanged();
}

void OcrImageView::clearSelection() {
    if (m_anchor < 0 && m_lead < 0) return;
    m_anchor = m_lead = -1;
    viewport()->update();
}

QVector<int> OcrImageView::selectedLines() const {
    QVector<int> lines;
    if (m_anchor < 0 || m_lead < 0 || m_glyphs.isEmpty()) return lines;
    const int first = qMin(m_anchor,m_lead);
    const int last = qMin(qMax(m_anchor,m_lead),int(m_glyphs.size())-1);
    int previous = -1;
    for (int i = first; i <= last; ++i) {
        if (m_glyphs[i].line == previous) continue;
        previous = m_glyphs[i].line;
        lines.append(previous);
    }
    return lines;
}

QString OcrImageView::selectedText() const {
    int from = -1, to = -1;
    return selectedTextRange(&from,&to) ? m_textData.mid(from,to-from+1) : QString();
}

bool OcrImageView::selectedTextRange(int *from, int *to) const {
    if (!from || !to || m_anchor < 0 || m_lead < 0 || m_glyphs.isEmpty()) return false;
    const int first = qMin(m_anchor,m_lead), last = qMin(qMax(m_anchor,m_lead),int(m_glyphs.size())-1);
    if (first < 0 || first > last) return false;
    *from = m_glyphs[first].textIndex;
    *to = m_glyphs[last].textIndex;
    return *from >= 0 && *to >= *from;
}

void OcrImageView::setBarcodeRegions(const QVector<QPolygonF> &regions, int selected)
{
    m_barcodeRegions = regions;
    m_barcodeSelected = selected;
    viewport()->update();
}

void OcrImageView::highlightText(const QString &selection) {
    m_highlighted.clear();
    // Compatibility-folded and blank-free on both sides, so reflowed or width-converted text still finds its glyphs.
    QString needle;
    for (const QChar ch : selection.normalized(QString::NormalizationForm_KC))
        if (!ch.isSpace() && ch != QChar(0x2029) && ch != QChar(0x2028)) needle += ch;
    if (!needle.isEmpty() && !m_glyphs.isEmpty()) {
        QHash<int,int> glyphOf;
        for (int i = 0; i < m_glyphs.size(); ++i) glyphOf.insert(m_glyphs[i].textIndex,i);
        QString hay;
        QVector<int> source;
        for (int i = 0; i < m_textData.size(); ++i) {
            if (m_textData[i].isSpace()) continue;
            for (const QChar ch : QString(m_textData[i]).normalized(QString::NormalizationForm_KC)) {
                hay += ch; source.append(i);
            }
        }
        QSet<int> marked;
        for (int at = hay.indexOf(needle); at >= 0; at = hay.indexOf(needle,at+needle.size()))
            for (int k = at; k < at+needle.size(); ++k)
                if (glyphOf.contains(source[k])) marked.insert(glyphOf.value(source[k]));
        m_highlighted = QVector<int>(marked.begin(),marked.end());
        std::sort(m_highlighted.begin(),m_highlighted.end());
    }
    viewport()->update();
}

void OcrImageView::highlightTextRange(int from, int to) {
    m_highlighted.clear();
    if (from >= 0 && to >= from)
        for (int i = 0; i < m_glyphs.size(); ++i)
            if (m_glyphs[i].textIndex >= from && m_glyphs[i].textIndex <= to) m_highlighted.append(i);
    viewport()->update();
}

void OcrImageView::copySelection() {
    const QString text = selectedText();
    if (text.isEmpty()) return;
    copyTextToClipboard(text);
}

void OcrImageView::mousePressEvent(QMouseEvent *event) {
    if (event->button()==Qt::LeftButton && !m_glyphs.isEmpty()) {
        setFocus(Qt::MouseFocusReason);
        const int hit = glyphAt(event->position().toPoint(),false);
        if (hit >= 0) {   // Text starts a selection, not a pan; the text cursor stays for the whole drag.
            m_press = hit;
            viewport()->setCursor(Qt::IBeamCursor);
            event->accept(); return;
        }
        selectRange(-1,-1);
    }
    QGraphicsView::mousePressEvent(event);
}

void OcrImageView::mouseMoveEvent(QMouseEvent *event) {
    if (m_press >= 0 && (event->buttons() & Qt::LeftButton)) {
        if (!m_selecting) { m_selecting = true; selectRange(m_press,m_press); }
        const int lead = glyphAt(event->position().toPoint(),true);
        if (lead >= 0 && lead != m_lead) selectRange(m_anchor,lead);
        event->accept(); return;
    }
    if (event->buttons()==Qt::NoButton && !m_glyphs.isEmpty())
        viewport()->setCursor(glyphAt(event->position().toPoint(),false) >= 0 ? Qt::IBeamCursor : Qt::OpenHandCursor);
    QGraphicsView::mouseMoveEvent(event);
}

void OcrImageView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button()==Qt::LeftButton && m_press >= 0) {
        // A click on text without a drag drops the selection.
        if (!m_selecting) selectRange(-1,-1);
        m_press = -1; m_selecting = false;
        viewport()->setCursor(glyphAt(event->position().toPoint(),false) >= 0 ? Qt::IBeamCursor : Qt::OpenHandCursor);
        event->accept(); return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void OcrImageView::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::Copy) && !selectedText().isEmpty()) { copySelection(); event->accept(); return; }
    QGraphicsView::keyPressEvent(event);
}

void OcrImageView::contextMenuEvent(QContextMenuEvent *event) {
    if (selectedText().isEmpty()) { QGraphicsView::contextMenuEvent(event); return; }
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("复制"));
    if (menu.exec(event->globalPos()) == copy) copySelection();
    event->accept();
}

void OcrImageView::leaveEvent(QEvent *event) {
    if (!m_glyphs.isEmpty()) viewport()->setCursor(Qt::OpenHandCursor);
    QGraphicsView::leaveEvent(event);
}

void OcrImageView::drawForeground(QPainter *painter, const QRectF &rect) {
    QGraphicsView::drawForeground(painter,rect);
    if (!m_barcodeRegions.isEmpty()) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < m_barcodeRegions.size(); ++i) {
            const QPolygonF &quad = m_barcodeRegions[i];
            if (quad.isEmpty()) continue;
            const bool active = i == m_barcodeSelected;
            QPen pen(active ? Bar::accent : Bar::dim);
            pen.setCosmetic(true);
            pen.setWidthF(active ? 2.5 : 1.5);
            painter->setPen(pen);
            QColor activeFill = Bar::accent;
            activeFill.setAlpha(55);
            if (active) painter->setBrush(activeFill);
            else painter->setBrush(QBrush(Qt::NoBrush));
            if (quad.boundingRect().height() < 2) {   // 1D symbols are reported as a line.
                painter->drawRoundedRect(quad.boundingRect().adjusted(-2,-7,2,7),3,3);
            } else if (!active) {
                // The unselected frame sits just outside the modules so it remains visible on white quiet zones.
                painter->drawRoundedRect(quad.boundingRect().adjusted(-5,-5,5,5),3,3);
            } else {
                painter->drawPolygon(quad);
            }
        }
        painter->restore();
    }
    if (m_glyphs.isEmpty()) return;
    // Consecutive glyphs of one line paint as one quad: first glyph's leading edge to the last one's trailing edge.
    const auto paintRuns = [&](const QVector<int> &indices) {
        for (int i = 0; i < indices.size();) {
            int j = i;
            while (j+1 < indices.size() && indices[j+1] == indices[j]+1
                   && m_glyphs[indices[j+1]].line == m_glyphs[indices[i]].line) ++j;
            const QPolygonF &a = m_glyphs[indices[i]].polygon, &b = m_glyphs[indices[j]].polygon;
            const QPolygonF run{a[0],b[1],b[2],a[3]};
            if (run.boundingRect().intersects(rect)) painter->drawPolygon(run);
            i = j+1;
        }
    };
    painter->save();
    painter->setPen(Qt::NoPen);
    painter->setRenderHint(QPainter::Antialiasing);
    if (!m_highlighted.isEmpty()) {
        painter->setBrush(QColor(0xff,0xc4,0x00,110));
        paintRuns(m_highlighted);
    }
    if (m_anchor >= 0 && m_lead >= 0) {
        QVector<int> range;
        for (int i = qMin(m_anchor,m_lead); i <= qMin(qMax(m_anchor,m_lead),int(m_glyphs.size())-1); ++i) range.append(i);
        QColor selectedFill = Bar::accent;
        selectedFill.setAlpha(80);
        painter->setBrush(selectedFill);
        paintRuns(range);
    }
    painter->restore();
}

PickerCombo::PickerCombo()
{
    setItemDelegate(new PopupItemDelegate(this));
}

void PickerCombo::showPopup()
{
    prepareComboPopup(this);
    QComboBox::showPopup();
}

QString PickerCombo::displayText() const
{
    const QString choice = currentIndex() == 0 && !m_detected.isEmpty() ? m_detected : currentText();
    return m_prefix.isEmpty() ? choice : QStringLiteral("%1 (%2)").arg(m_prefix,choice);
}

QSize PickerCombo::sizeHint() const
{
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const QSize text(fontMetrics().horizontalAdvance(displayText()),fontMetrics().height());
    return style()->sizeFromContents(QStyle::CT_ComboBox,&option,text,this);
}

void PickerCombo::paintEvent(QPaintEvent *)
{
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    option.currentText = displayText();
    painter.drawComplexControl(QStyle::CC_ComboBox,option);
    painter.drawControl(QStyle::CE_ComboBoxLabel,option);
    // The sheet hides the native arrow; draw the card chevron in the drop-down area instead.
    const QRect arrow = style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxArrow,this);
    const QIcon chevron = cardIcon(Icon::CardDown);
    chevron.paint(&painter,QRect(arrow.center()-QPoint(10,10),QSize(20,20)),Qt::AlignCenter,
                  underMouse() ? QIcon::Active : QIcon::Normal);
}

// Compact first-load strip: one dark rounded surface, a single accent spinner and one short label.
class OcrLoadingBar : public QWidget {
public:
    explicit OcrLoadingBar(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(72);
        m_timer = new QTimer(this);
        m_timer->setInterval(50);
        connect(m_timer,&QTimer::timeout,this,[this] {
            m_angle = (m_angle + 8) % 360;
            update();
        });
    }
    void setText(const QString &text) { m_text = text; update(); }
    void setActive(bool active) {
        if (active) m_timer->start();
        else m_timer->stop();
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(1.5,1.5,-1.5,-1.5);
        QFont label = font();
        label.setPointSizeF((std::max)(12.0,font().pointSizeF()));
        painter.setFont(label);
        const int textWidth = painter.fontMetrics().horizontalAdvance(m_text);
        const qreal contentWidth = 28 + 20 + textWidth;
        const qreal left = (std::max)(box.left() + 24, box.center().x() - contentWidth / 2);
        const QRectF ring(left,box.center().y()-14,28,28);
        painter.setPen(QPen(Bar::divider,2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(ring);
        QPen arc(Bar::accent,2.0);
        arc.setCapStyle(Qt::RoundCap);
        painter.setPen(arc);
        painter.drawArc(ring,-m_angle*16,100*16);

        painter.setPen(Bar::ink);
        const QRectF text(ring.right()+20,box.top(),box.right()-ring.right()-36,box.height());
        painter.drawText(text,Qt::AlignLeft|Qt::AlignVCenter,m_text);
    }
private:
    QTimer *m_timer = nullptr;
    QString m_text;
    int m_angle = 0;
};

OcrDialog::OcrDialog(const QImage &image, const QRect &captureRect, bool translate, bool barcode)
    : QDialog(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint),
      m_image(image), m_translate(translate), m_barcode(barcode) {
    setObjectName(QStringLiteral("ocrComparison"));
    setWindowTitle(m_barcode ? tr("LensSnip 二维码/条形码")
                             : m_translate ? tr("LensSnip 翻译") : tr("LensSnip OCR"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumSize(640,360);
    setMouseTracking(true);
    const QFont interfaceFont = font();   // Card chrome keeps the app-wide ui font; content panes set their own tiers below.
    // Dark card: zinc chrome, each content on its own near-black rounded block, actions under it.
    const QString surface = QStringLiteral("#18191d"), panel = QStringLiteral("#0c0d10"),
                  hover = QStringLiteral("#222326"), foreground = QStringLiteral("#f4f5f8"),
                  muted = QStringLiteral("#9c9da1"), outline = QStringLiteral("#2b2d33"),
                  accent = QStringLiteral("#5e6ad2"), selected = QStringLiteral("#32345b"),
                  thumb = QStringLiteral("#555555");
    // Natively drawn bits (combo arrows, message box text) follow the palette, not the sheet.
    QPalette dark = palette();
    for (const auto role : {QPalette::Window, QPalette::Base, QPalette::Button, QPalette::AlternateBase})
        dark.setColor(role, QColor(panel));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        dark.setColor(role, QColor(foreground));
    dark.setColor(QPalette::PlaceholderText, QColor(0x80,0x80,0x80));
    dark.setColor(QPalette::Highlight, QColor(selected));
    dark.setColor(QPalette::HighlightedText, QColor(foreground));
    setPalette(dark);
    setStyleSheet(QStringLiteral(
        "QDialog{background:%1;color:%4;border:none;border-radius:12px;}"
        "QLabel{color:%5;background:transparent;} QLabel#ocrTitle{color:%4;}"
        "QWidget#ocrLoading{background:transparent;}"
        "QFrame#ocrImagePanel,QFrame#ocrReadingSurface{background:%2;border:none;border-radius:12px;}"
        "QFrame#ocrTextPanel,QFrame#translateSourceBlock,QFrame#translateResultBlock{background:transparent;border:none;}"
        "QWidget#ocrFind{background:transparent;border:none;}"
        // Pickers: borderless filled fields; lists drop below.
        "QComboBox{background:%3;color:%4;border:none;border-radius:8px;padding:0 8px 0 12px;"
            "min-height:32px;combobox-popup:0;}"
        "QComboBox:hover{background:#2b2d33;} QComboBox:focus{background:%8;}"
        "QComboBox::drop-down{width:24px;border:none;background:transparent;} QComboBox::down-arrow{image:none;width:0;height:0;}"
        "QComboBox QAbstractItemView{background:%1;color:%4;border:none;outline:none;padding:0px;}"
        "QComboBox QAbstractItemView::item{min-height:24px;padding:0 12px;border-radius:8px;}"
        "QComboBox QAbstractItemView::item:selected{background:%7;color:white;}"
        "QPlainTextEdit{background:%2;color:%4;border:none;padding:8px 12px;selection-background-color:%8;}"
        "QPlainTextEdit#translateSourceText{color:%5;}"
        "QGraphicsView{background:%2;border:none;}"
        "QLineEdit{background:%2;color:%4;border:none;border-radius:8px;padding:3px 8px;selection-background-color:%8;}"
        "QLineEdit:focus{background:%3;}"
        "QPushButton{background:%3;color:%4;border:none;border-radius:8px;padding:4px 14px;min-width:56px;text-align:center;}"
        "QPushButton:hover{background:#2b2d33;} QPushButton:focus{background:%8;}"
        // Stale translation: a grey haze over the pane, one filled button in the middle.
        "QWidget#retranslateMask{background:rgba(12,13,16,220);border-radius:12px;}"
        "QPushButton#retranslateButton{min-width:150px;max-width:150px;min-height:30px;max-height:30px;padding:0;"
            "border:none;border-radius:12px;background:%1;color:%4;}"
        "QPushButton#retranslateButton:hover,QPushButton#retranslateButton:pressed{background:%7;color:white;}"
        // Icon buttons carry no fill; hover/focus use a background fill instead of a line.
        "QToolButton{background:transparent;color:%4;border:none;border-radius:6px;padding:3px;text-align:center;}"
        "QToolButton:hover{background:%3;} QToolButton:pressed{background:#2b2d33;}"
        "QToolButton:focus{background:%8;} QToolButton:disabled{color:#73757f;}"
        "QToolButton::menu-indicator{image:none;width:0;}"
        "QToolButton#ocrReplace,QToolButton#ocrReplaceAll{background:%3;padding:0 10px;border:none;}"
        "QToolButton#ocrReplace:hover,QToolButton#ocrReplaceAll:hover{background:#2b2d33;}"
        // Splitters: invisible grab gap, no hairline.
        "QSplitter::handle{background:transparent;}"
        "QSplitter::handle:hover,QSplitter::handle:pressed{background:transparent;}"
        "QMenu{background:%1;color:%4;border:none;padding:4px;}"
        "QMenu::item{padding:6px 20px;border-radius:6px;} QMenu::item:selected{background:%7;color:white;}"
        "QMenu::item:disabled{color:#73757f;} QMenu::separator{height:4px;background:transparent;margin:2px 8px;}"
        "QScrollBar:vertical{background:transparent;width:8px;margin:0;} QScrollBar:horizontal{background:transparent;height:8px;margin:0;}"
        "QScrollBar::handle{background:%9;border-radius:4px;min-width:16px;min-height:16px;}"
        "QScrollBar::handle:hover{background:#797979;}"
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;}"
        "QScrollBar::add-page,QScrollBar::sub-page{background:transparent;}"
        "QToolTip{background:%1;color:%4;border:none;padding:6px;}"
    ).arg(surface,panel,hover,foreground,muted,outline,accent,selected,thumb));
    auto *layout = new QVBoxLayout(this);
    // Child-free padding along every edge: the card itself sees the cursor there and resizes.
    layout->setContentsMargins(ResizeBand,ResizeBand,ResizeBand,ResizeBand);
    layout->setSpacing(0);
    const auto iconButton = [this](Icon icon,const QString &name,const QString &tip) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        // Rasterized on its own 20px grid, never rescaled; hover tints the glyph instead of filling the button.
        button->setIcon(cardIcon(icon));
        button->setIconSize(QSize(20,20));
        button->setAutoRaise(true);              // The style only reports hover (QIcon::Active) for auto-raise buttons.
        button->setFixedSize(28,28);
        // Clicks must not take focus (no outline); Tab still reaches and outlines the button.
        button->setFocusPolicy(Qt::TabFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        return button;
    };
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("ocrHeader"));
    m_header = header;
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0,0,0,0);
    headerLayout->setSpacing(6);
    auto *title = new QLabel(m_barcode ? tr("二维码 / 条形码") : m_translate ? tr("识别并翻译") : tr("文字识别"));
    title->setObjectName(QStringLiteral("ocrTitle"));
    QFont titleFont = interfaceFont;
    titleFont.setPointSizeF(qMax(12.0, interfaceFont.pointSizeF()));
    titleFont.setWeight(QFont::Medium);
    title->setFont(titleFont);
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    auto *back = iconButton(Icon::Back,QStringLiteral("ocrBack"),tr("返回"));
    connect(back,&QToolButton::clicked,this,[this] {
        m_returnedToSource = true;
        reject();
    });
    headerLayout->addWidget(back);
    auto *pin = iconButton(Icon::CardPin,QStringLiteral("ocrPin"),
                           m_barcode ? tr("置顶二维码卡片") : tr("置顶 OCR 卡片"));
    pin->setCheckable(true);
    connect(pin,&QToolButton::toggled,this,[this,pin](bool checked) {
        // Change only native Z-order so geometry, focus and editing state remain intact.
        if (!SetWindowPos(reinterpret_cast<HWND>(winId()),checked ? HWND_TOPMOST : HWND_NOTOPMOST,
                          0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)) {
            const QSignalBlocker blocker(pin);
            pin->setChecked(!checked);
            setStatus(tr("切换置顶失败，请重试"));
            return;
        }
        const QString tip = m_barcode
            ? (checked ? tr("取消置顶二维码卡片") : tr("置顶二维码卡片"))
            : (checked ? tr("取消置顶 OCR 卡片") : tr("置顶 OCR 卡片"));
        pin->setToolTip(tip); pin->setAccessibleName(tip);
    });
    headerLayout->addWidget(pin);
    auto *minimize = iconButton(Icon::CardMinimize,QStringLiteral("ocrMinimize"),tr("最小化"));
    connect(minimize,&QToolButton::clicked,this,&QWidget::showMinimized);
    headerLayout->addWidget(minimize);
    m_maximize = iconButton(Icon::CardMaximize,QStringLiteral("ocrMaximize"),tr("最大化"));
    connect(m_maximize,&QToolButton::clicked,this,&OcrDialog::toggleMaximize);
    headerLayout->addWidget(m_maximize);
    auto *close = iconButton(Icon::CardClose,QStringLiteral("ocrClose"),tr("关闭 (Esc)"));
    close->setStyleSheet(QStringLiteral(
        "QToolButton#ocrClose{background:transparent;border:none;border-radius:6px;}"
        "QToolButton#ocrClose:hover{background:#222326;}"
        "QToolButton#ocrClose:focus{background:transparent;border:2px solid #828fff;}"));
    connect(close,&QToolButton::clicked,this,&QDialog::reject);
    headerLayout->addWidget(close);
    header->setFixedHeight(28);
    header->setStyleSheet(QStringLiteral("QWidget#ocrHeader{border:none;}QLabel{border:none;}"));
    layout->addWidget(header);
    // One toolbar under the title: pickers on the left, card actions on the right.
    m_toolbar = new QWidget;
    m_toolbar->setObjectName(m_translate ? QStringLiteral("translateToolbar")
                                         : QStringLiteral("ocrToolbar"));
    QHBoxLayout *tools = new QHBoxLayout(m_toolbar);
    tools->setContentsMargins(0,8,0,8);
    tools->setSpacing(8);
    layout->addWidget(m_toolbar);
    // The card opens as a small centred strip while the first recognition/translation
    // runs; the finished card then expands to its fixed reading layout.
    m_loadingPage = new QWidget;
    m_loadingPage->setObjectName(QStringLiteral("ocrLoading"));
    auto *loadingLayout = new QVBoxLayout(m_loadingPage);
    loadingLayout->setContentsMargins(0,0,0,0);
    m_loadingBar = new OcrLoadingBar;
    loadingLayout->addWidget(m_loadingBar);
    layout->addWidget(m_loadingPage,1);
    m_loadingPage->hide();
    m_split = new QSplitter(Qt::Horizontal);
    m_split->setObjectName(QStringLiteral("ocrSplit"));
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(13);             // 6px gap, 1px hairline, 6px gap.
    // Each content sits on a rounded block; its actions run along the block's bottom edge.
    const auto block = [](const QString &name, int margin) {
        auto *frame = new QFrame;
        frame->setObjectName(name);
        auto *column = new QVBoxLayout(frame);
        column->setContentsMargins(margin,margin,margin,8);
        column->setSpacing(6);
        return std::pair<QFrame *,QVBoxLayout *>(frame,column);
    };
    const auto actionRow = [](const QString &name) {
        auto *bar = new QWidget;
        bar->setObjectName(name);
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(0,0,0,0);
        row->setSpacing(4);
        return std::pair<QWidget *,QHBoxLayout *>(bar,row);
    };
    QVBoxLayout *imageColumn;
    std::tie(m_imagePanel,imageColumn) = block(QStringLiteral("ocrImagePanel"),8);
    auto *imageView = new OcrImageView(image);
    imageView->setFocusPolicy(Qt::StrongFocus);
    imageView->setToolTip(tr("滚轮缩放 · 拖动查看 · 双击适应"));
    imageColumn->addWidget(imageView,1);
    QWidget *imageBar;
    QHBoxLayout *imageActions;
    std::tie(imageBar,imageActions) = actionRow(QStringLiteral("ocrImageBar"));
    auto *copyImage = iconButton(Icon::CardCopy,QStringLiteral("ocrCopyImage"),tr("复制图片"));
    connect(copyImage,&QToolButton::clicked,this,[this] { copyImageToClipboard(m_image); });
    // Ten percentage points a click.
    auto *zoomIn = iconButton(Icon::CardZoomIn,QStringLiteral("ocrZoomIn"),tr("放大图片"));
    connect(zoomIn,&QToolButton::clicked,imageView,[imageView] { imageView->zoomTo(imageView->percent()+10); });
    auto *zoomOut = iconButton(Icon::CardZoomOut,QStringLiteral("ocrZoomOut"),tr("缩小图片"));
    connect(zoomOut,&QToolButton::clicked,imageView,[imageView] { imageView->zoomTo(imageView->percent()-10); });
    auto *zoomLevel = new QLabel;
    zoomLevel->setObjectName(QStringLiteral("ocrZoomLevel"));
    zoomLevel->setAlignment(Qt::AlignCenter);
    zoomLevel->setMinimumWidth(40);
    auto *fitToggle = iconButton(Icon::CardFit,QStringLiteral("ocrFitToggle"),QString());
    fitToggle->setFixedSize(26,24);
    connect(fitToggle,&QToolButton::clicked,imageView,[imageView] {
        if (imageView->fitted()) imageView->zoomTo(100); else imageView->fit();
    });
    imageView->zoomed = [imageView,zoomLevel,fitToggle] {
        zoomLevel->setText(QStringLiteral("%1%").arg(imageView->percent()));
        const bool fitted = imageView->fitted();
        fitToggle->setIcon(cardIcon(fitted ? Icon::CardActual : Icon::CardFit));
        const QString tip = fitted ? tr("原始大小 (1:1)") : tr("适应屏幕");
        fitToggle->setToolTip(tip);
        fitToggle->setAccessibleName(tip);
    };
    imageView->zoomed();
    m_imageView = imageView;
    imageView->selectionChanged = [this] {
        if (!m_barcode) linkFromOcrImageSelection();
    };
    imageActions->addWidget(copyImage);
    imageActions->addWidget(zoomIn);
    imageActions->addWidget(zoomOut);
    imageActions->addWidget(zoomLevel);
    imageActions->addWidget(fitToggle);
    imageActions->addStretch();
    m_imageCount = new QLabel(QStringLiteral("0"));
    m_imageCount->setObjectName(QStringLiteral("ocrImageCount"));
    m_imageCount->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    m_imageCount->setMinimumWidth(24);
    m_imageCount->setStyleSheet(QStringLiteral("color:#9c9da1;font-size:12px;"));
    imageActions->addWidget(m_imageCount);
    imageActions->setSpacing(3);
    for (auto *button : {copyImage,zoomIn,zoomOut,fitToggle}) {
        button->setFixedSize(26,24);
        button->setIconSize(QSize(20,20));
    }
    imageBar->setFixedHeight(28);
    imageColumn->addWidget(imageBar);
    m_imagePanel->setMinimumSize(180,120);
    m_split->addWidget(m_imagePanel);
    m_format = iconButton(Icon::CardFormat,QStringLiteral("ocrFormat"),tr("排版"));
    auto *formatMenu = new QMenu(m_format);
    auto *formatGroup = new QActionGroup(formatMenu);
    const std::pair<QString,TextFormat> formats[] = {
        {tr("自动"),TextFormat::Auto},{tr("移除换行符"),TextFormat::JoinLines},{tr("移除空格"),TextFormat::RemoveSpaces},
        {tr("全角符号"),TextFormat::FullWidth},{tr("半角符号"),TextFormat::HalfWidth}};
    for (const auto &[label,kind] : formats) {
        QAction *action = formatMenu->addAction(label);
        action->setCheckable(true);
        formatGroup->addAction(action);
        if (kind==TextFormat::Auto) { m_autoFormat = action; action->setChecked(true); }
        connect(action,&QAction::triggered,this,[this,kind = kind] {
            if (m_text->isReadOnly()) return;
            // Auto restores the recognized text; other modes reformat the current, possibly corrected, text.
            const QString next = kind==TextFormat::Auto ? m_raw : formatText(m_text->toPlainText(),kind);
            if (next==m_text->toPlainText()) return;
            QTextCursor cursor(m_text->document());   // One undoable edit instead of setPlainText.
            cursor.select(QTextCursor::Document);
            cursor.insertText(next);
        });
    }
    m_format->setMenu(formatMenu);
    m_format->setPopupMode(QToolButton::InstantPopup);
    if (m_barcode) m_format->hide();
    QToolButton *search = nullptr;
    if (m_translate) {
        // One row for the whole card: language › target and the engine on the left, actions on the right.
        // Translate card: no format or search buttons; find lives in each pane's context menu and Ctrl+F/H.
        m_format->hide();
        const auto combo = [&](const QString &name, const QString &tip) {
            auto *box = new PickerCombo;
            box->setObjectName(name);
            box->setToolTip(tip);
            box->setAccessibleName(tip);
            box->setFocusPolicy(Qt::TabFocus);
            box->setCursor(Qt::PointingHandCursor);
            box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
            box->setMaxVisibleItems(18);
            return box;
        };
        m_source = combo(QStringLiteral("translateSource"),tr("原文语言"));
        m_target = combo(QStringLiteral("translateTarget"),tr("译文语言"));
        m_provider = combo(QStringLiteral("translateProvider"),tr("翻译引擎：自动会在失败时换下一家"));
        const std::pair<QString,TranslateGateway::Provider> providers[] = {
            {tr("自动选择"),TranslateGateway::Provider::Auto},{tr("Edge"),TranslateGateway::Provider::Edge},
            {tr("Google"),TranslateGateway::Provider::Google},{tr("有道"),TranslateGateway::Provider::Youdao},
            {tr("搜狗"),TranslateGateway::Provider::Sogou},{tr("腾讯交互翻译"),TranslateGateway::Provider::Transmart},
            {tr("火山"),TranslateGateway::Provider::Volcengine},{tr("DeepL"),TranslateGateway::Provider::Deepl}};
        for (const auto &[label,provider] : providers)
            m_provider->addItem(label,int(provider));
        // Every card starts on Auto (fallback across engines); a chosen engine lasts for this card only.
        const LensSnipSettings settings;
        populateLanguages(settings.value(QStringLiteral("Translate/source")).toString(),
                          settings.value(QStringLiteral("Translate/target"),QStringLiteral("zh")).toString());
        syncPickers();
        auto *arrow = new QLabel;
        arrow->setPixmap(cardIcon(Icon::CardRight).pixmap(QSize(20,20),devicePixelRatioF()));
        arrow->setAccessibleName(tr("翻译为"));
        connect(m_provider,&QComboBox::currentIndexChanged,this,[this] {
            populateLanguages(m_source->currentData().toString(),m_target->currentData().toString());
            syncPickers();
            saveTranslateChoice();
            translateLines();
        });
        connect(m_source,&QComboBox::activated,this,[this] { syncPickers(); saveTranslateChoice(); translateLines(); });
        connect(m_target,&QComboBox::activated,this,[this] { saveTranslateChoice(); translateLines(); });
        tools->addWidget(m_source);
        tools->addWidget(arrow);
        tools->addWidget(m_target);
        tools->addWidget(m_provider);
        m_view = iconButton(Icon::View,QStringLiteral("translateView"),tr("显示内容"));
        auto *viewMenu = new QMenu(m_view);
        auto *viewGroup = new QActionGroup(viewMenu);
        const QString views[] = {tr("图片、原文和译文"),tr("原文和译文"),tr("仅译文")};
        for (int mode = 0; mode < 3; ++mode) {
            QAction *action = viewMenu->addAction(views[mode]);
            action->setCheckable(true);
            action->setData(mode);
            viewGroup->addAction(action);
            connect(action,&QAction::triggered,this,[this,mode] {
                setDisplay(mode);
                LensSnipSettings().setValue(QStringLiteral("Translate/view"),mode);
            });
        }
        m_view->setMenu(viewMenu);
        m_view->setPopupMode(QToolButton::InstantPopup);
        m_again = iconButton(Icon::CardTranslate,QStringLiteral("translateAgain"),tr("重新翻译 (Ctrl+Enter)"));
        m_again->setAccessibleName(tr("重新翻译"));
        connect(m_again,&QToolButton::clicked,this,[this] { translateLines(); });
    }
    // Translate card: recognize the capture again, then translate it with the chosen engine.
    m_retry = iconButton(Icon::CardReload,QStringLiteral("ocrRetry"),
                         m_barcode ? tr("重新识别")
                                   : m_translate ? tr("重新识别并翻译") : tr("重新识别"));
    connect(m_retry,&QToolButton::clicked,this,&OcrDialog::start);
    const auto textAction = [](QToolButton *button, const QString &text) {
        if (!button) return;
        button->setProperty("fullText", text);
        button->setText(text);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setFixedHeight(32);
        button->setMinimumWidth(0);
        button->setMaximumWidth(QWIDGETSIZE_MAX);
        button->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Fixed);
        button->setStyleSheet(QStringLiteral(
            "QToolButton{border:none;background:transparent;padding:0 6px;color:#f4f5f8;text-align:center;}"
            "QToolButton:hover{background:#222326;border-radius:6px;}"
            "QToolButton:pressed{background:#2b2d33;border-radius:6px;}"
            "QToolButton:disabled{color:#73757f;}"));
    };
    textAction(m_format,tr("排版"));
    textAction(m_retry,m_translate?tr("识别并翻译"):tr("重新识别"));
    if (m_translate) { textAction(m_view,tr("显示内容")); textAction(m_again,tr("重新翻译")); }
    if (!m_translate && !m_barcode) {
        search = iconButton(Icon::CardSearch,QStringLiteral("ocrSearch"),tr("查找 (Ctrl+F)"));
        connect(search,&QToolButton::clicked,this,[this] { m_find->isVisible() ? hideFind() : showFind(false); });
        textAction(search,tr("查找"));
    }
    // Find / replace strip under the bar, shown by the search button, Ctrl+F or Ctrl+H.
    m_find = new QWidget;
    m_find->setObjectName(QStringLiteral("ocrFind"));
    m_find->setAttribute(Qt::WA_StyledBackground);
    auto *findLayout = new QGridLayout(m_find);
    findLayout->setContentsMargins(8,4,8,6);
    findLayout->setHorizontalSpacing(4); findLayout->setVerticalSpacing(4);
    const auto smallButton = [&](Icon icon,const QString &name,const QString &tip) {
        return iconButton(icon,name,tip);
    };
    m_replaceToggle = smallButton(Icon::CardRight,QStringLiteral("ocrReplaceToggle"),tr("切换替换"));
    connect(m_replaceToggle,&QToolButton::clicked,this,[this] { showFind(!m_replaceRow->isVisible(),m_findTarget); });
    findLayout->addWidget(m_replaceToggle,0,0);
    m_findEdit = new QLineEdit;
    m_findEdit->setObjectName(QStringLiteral("ocrFindEdit"));
    m_findEdit->setPlaceholderText(tr("查找"));
    m_findEdit->setAccessibleName(tr("查找"));
    m_findEdit->installEventFilter(this);
    connect(m_findEdit,&QLineEdit::textChanged,this,[this] { m_match = 0; highlightMatches(true); });
    findLayout->addWidget(m_findEdit,0,1);
    m_findCount = new QLabel;
    m_findCount->setObjectName(QStringLiteral("ocrFindCount"));
    m_findCount->setMinimumWidth(56);
    m_findCount->setAlignment(Qt::AlignCenter);
    findLayout->addWidget(m_findCount,0,2);
    auto *previous = smallButton(Icon::CardUp,QStringLiteral("ocrFindPrevious"),tr("上一个 (Shift+Enter)"));
    connect(previous,&QToolButton::clicked,this,[this] { findStep(-1); });
    findLayout->addWidget(previous,0,3);
    auto *next = smallButton(Icon::CardDown,QStringLiteral("ocrFindNext"),tr("下一个 (Enter)"));
    connect(next,&QToolButton::clicked,this,[this] { findStep(1); });
    findLayout->addWidget(next,0,4);
    auto *closeFind = smallButton(Icon::CardClose,QStringLiteral("ocrFindClose"),tr("关闭查找 (Esc)"));
    connect(closeFind,&QToolButton::clicked,this,&OcrDialog::hideFind);
    findLayout->addWidget(closeFind,0,5);
    m_replaceRow = new QWidget;
    auto *replaceLayout = new QHBoxLayout(m_replaceRow);
    replaceLayout->setContentsMargins(0,0,0,0); replaceLayout->setSpacing(4);
    m_replaceEdit = new QLineEdit;
    m_replaceEdit->setObjectName(QStringLiteral("ocrReplaceEdit"));
    m_replaceEdit->setPlaceholderText(tr("替换"));
    m_replaceEdit->setAccessibleName(tr("替换为"));
    m_replaceEdit->installEventFilter(this);
    replaceLayout->addWidget(m_replaceEdit,1);
    const std::tuple<QString,QString,void (OcrDialog::*)()> replaceActions[] = {
        {tr("替换"),QStringLiteral("ocrReplace"),&OcrDialog::replaceOne},
        {tr("全部替换"),QStringLiteral("ocrReplaceAll"),&OcrDialog::replaceAll}};
    for (const auto &[label,name,slot] : replaceActions) {
        auto *button = new QToolButton;
        button->setObjectName(name);
        button->setText(label);
        button->setFixedHeight(28);
        button->setFocusPolicy(Qt::TabFocus);
        connect(button,&QToolButton::clicked,this,slot);
        replaceLayout->addWidget(button);
    }
    findLayout->addWidget(m_replaceRow,1,1,1,5);
    m_find->hide();
    QFont font = interfaceFont;
    font.setPointSizeF(qMax(11.25,interfaceFont.pointSizeF()));
    QWidget *sourceBox = nullptr;
    if (m_translate) {
        // Recognized text sits above the translation, quieter, and stays editable:
        // fixing an OCR slip re-translates after a short pause, Ctrl+Enter at once.
        m_sourcePane = new OcrTextEdit;
        m_sourcePane->setObjectName(QStringLiteral("translateSourceText"));
        m_sourcePane->setAccessibleName(tr("识别原文，可修改后重新翻译"));
        m_sourcePane->setPlaceholderText(tr("正在识别…"));
        m_sourcePane->setReadOnly(true);
        m_sourcePane->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        QFont sourceFont = font;
        sourceFont.setPointSizeF(font.pointSizeF());
        m_sourcePane->setFont(sourceFont);
        // Selecting the recognized original mirrors the same characters on the capture.
        connect(m_sourcePane,&QPlainTextEdit::selectionChanged,this,[this] {
            m_imageView->clearSelection();
            highlightImageForPane(m_sourcePane);
        });
        // Editing the original never calls the engine by itself: the stale translation is hazed
        // over with one button, and only a click (or Ctrl+Enter) translates again.
        connect(m_sourcePane,&QPlainTextEdit::textChanged,this,&OcrDialog::updateMask);
        auto *now = new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_Return),m_sourcePane,nullptr,nullptr,Qt::WidgetShortcut);
        connect(now,&QShortcut::activated,this,[this] { translateLines(); });
        sourceBox = m_sourcePane;
    }
    m_text = new OcrTextEdit;
    m_text->setObjectName(QStringLiteral("ocrText"));
    m_text->setAccessibleName(m_barcode ? tr("二维码/条形码内容")
                                        : m_translate ? tr("译文") : tr("OCR 识别文本"));
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_findTarget = m_text;
    // Selecting recognized text marks the same characters on the capture.
    if (!m_translate && !m_barcode) {
        connect(m_text,&QPlainTextEdit::selectionChanged,this,&OcrDialog::linkFromOcrTextSelection);
        connect(m_text,&QPlainTextEdit::textChanged,this,&OcrDialog::clearLinkHighlights);
    }
    if (m_translate) {
        font.setPointSizeF(qMax(12.0,interfaceFont.pointSizeF()));
        font.setWeight(QFont::Medium);
    }
    m_text->setFont(font);
    m_resultBaseFont = font;
    const auto applyTextScale = [](QPlainTextEdit *pane, const QFont &base,
                                   qreal scale, QLabel *label) {
        QFont scaled = base;
        scaled.setPointSizeF(base.pointSizeF() * scale);
        pane->setFont(scaled);
        label->setText(QStringLiteral("%1%").arg(qRound(scale * 100)));
    };
    const auto makeTextFooter = [this, applyTextScale, interfaceFont, iconButton](
            const QString &barName, const QString &prefix, QPlainTextEdit *pane,
            const QFont &base, qreal *scale, QLabel **percentOut, QLabel **countOut,
            const QString &copyTip) {
        auto *bar = new QWidget;
        bar->setObjectName(barName);
        bar->setFixedHeight(28);
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(0,0,0,0);
        row->setSpacing(3);
        auto *copy = iconButton(Icon::CardCopy, prefix + QStringLiteral("Copy"), copyTip);
        copy->setFixedSize(26,24);
        connect(copy,&QToolButton::clicked,this,[pane] {
            copyTextToClipboard(pane->toPlainText());
        });
        copy->setEnabled(!pane->toPlainText().isEmpty());
        connect(pane->document(),&QTextDocument::contentsChanged,copy,[copy,pane] {
            copy->setEnabled(!pane->toPlainText().isEmpty());
        });
        auto *plus = iconButton(Icon::CardZoomIn, prefix + QStringLiteral("ZoomIn"), tr("字号 +10%"));
        plus->setFixedSize(26,24);
        auto *minus = iconButton(Icon::CardZoomOut, prefix + QStringLiteral("ZoomOut"), tr("字号 -10%"));
        minus->setFixedSize(26,24);
        auto *percent = new QLabel;
        percent->setObjectName(prefix + QStringLiteral("ZoomLevel"));
        percent->setAlignment(Qt::AlignCenter);
        percent->setMinimumWidth(40);
        percent->setStyleSheet(QStringLiteral("color:#9c9da1;"));
        percent->setFont(interfaceFont);
        auto *count = new QLabel(QStringLiteral("0"));
        count->setObjectName(prefix + QStringLiteral("Count"));
        count->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
        count->setMinimumWidth(24);
        count->setStyleSheet(QStringLiteral("color:#9c9da1;"));
        count->setFont(interfaceFont);
        const auto changeScale = [pane,base,scale,percent,applyTextScale](int steps) {
            *scale = qBound(0.5, *scale + 0.1 * steps, 2.0);
            applyTextScale(pane,base,*scale,percent);
        };
        connect(plus,&QToolButton::clicked,this,[changeScale] { changeScale(1); });
        connect(minus,&QToolButton::clicked,this,[changeScale] { changeScale(-1); });
        static_cast<OcrTextEdit *>(pane)->wheelHandler = [changeScale](QWheelEvent *event) {
            if (!(event->modifiers() & Qt::ControlModifier)) return false;
            const int delta = event->angleDelta().y();
            if (delta == 0) return false;
            changeScale(delta > 0 ? 1 : -1);
            return true;
        };
        const auto refreshCount = [count,pane] {
            count->setText(QString::number(pane->toPlainText().toUcs4().size()));
        };
        connect(pane->document(),&QTextDocument::contentsChanged,count,refreshCount);
        refreshCount();
        applyTextScale(pane,base,*scale,percent);
        row->addWidget(copy);
        row->addWidget(plus);
        row->addWidget(minus);
        row->addWidget(percent);
        row->addStretch();
        row->addWidget(count);
        *percentOut = percent;
        *countOut = count;
        return bar;
    };
    QWidget *resultFooter = makeTextFooter(
        m_translate ? QStringLiteral("translateResultBar") : QStringLiteral("ocrTextBar"),
        QStringLiteral("ocrText"), m_text, m_resultBaseFont, &m_resultScale,
        &m_resultZoomLevel, &m_resultCount,
        m_barcode ? tr("复制内容") : m_translate ? tr("复制译文") : tr("复制识别文本"));
    QWidget *sourceFooter = nullptr;
    if (m_sourcePane) {
        m_sourceBaseFont = m_sourcePane->font();
        sourceFooter = makeTextFooter(QStringLiteral("translateSourceBar"),
            QStringLiteral("translateSource"), m_sourcePane, m_sourceBaseFont, &m_sourceScale,
            &m_sourceZoomLevel, &m_sourceCount, tr("复制原文"));
    }
    const auto updateImageCount = [this] {
        if (!m_imageCount) return;
        const QPlainTextEdit *pane = m_translate ? m_sourcePane : m_text;
        m_imageCount->setText(QString::number(pane ? pane->toPlainText().toUcs4().size() : 0));
    };
    connect(m_text->document(),&QTextDocument::contentsChanged,this,updateImageCount);
    if (m_sourcePane)
        connect(m_sourcePane->document(),&QTextDocument::contentsChanged,this,updateImageCount);
    updateImageCount();
    m_busy = new QLabel(tr("正在识别…"));
    m_busy->setObjectName(QStringLiteral("ocrBusy"));
    QFont busyFont = font; busyFont.setPointSizeF(font.pointSizeF()*4.0/3.0);   // Lands on the 20 px heading tier.
    m_busy->setFont(m_translate ? font : busyFont);   // The translate card says what it is doing in words.
    m_busy->setAlignment(Qt::AlignCenter);
    m_busy->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_busy->hide();
    auto *busyLayout = new QVBoxLayout(m_text->viewport());
    busyLayout->setContentsMargins(18,18,18,18); busyLayout->setSpacing(14);
    busyLayout->addStretch();
    m_emptyIcon=new QLabel(m_text->viewport());m_emptyIcon->setFixedSize(48,48);
    m_emptyIcon->setAlignment(Qt::AlignCenter);m_emptyIcon->setPixmap(cardIcon(m_barcode?Icon::Barcode:Icon::Ocr,20).pixmap(20,20));
    m_emptyIcon->setStyleSheet(QStringLiteral("background:#18191d;border:none;border-radius:12px;"));m_emptyIcon->hide();
    busyLayout->addWidget(m_emptyIcon,0,Qt::AlignCenter);
    busyLayout->addWidget(m_busy,0,Qt::AlignCenter);
    m_emptyRetry = new QToolButton(m_text->viewport());
    m_emptyRetry->setObjectName(QStringLiteral("emptyRetry"));
    m_emptyRetry->setIcon(cardIcon(Icon::CardReload)); m_emptyRetry->setText(tr("重新识别"));
    m_emptyRetry->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_emptyRetry->setFixedHeight(32);
    m_emptyRetry->setStyleSheet(QStringLiteral("QToolButton{background:#222326;border:none;border-radius:8px;padding:0 12px;color:#f4f5f8;}"
                                               "QToolButton:hover{background:#2b2d33;}"
                                               "QToolButton:pressed{background:#32345b;}"
                                               "QToolButton:disabled{color:#73757f;}"));
    m_emptyRetry->setFocusPolicy(Qt::TabFocus); m_emptyRetry->hide(); connect(m_emptyRetry,&QToolButton::clicked,this,[this]{if(m_translate&&m_again)m_again->click();else m_retry->click();});
    busyLayout->addWidget(m_emptyRetry,0,Qt::AlignCenter);busyLayout->addStretch();
    connect(m_text,&QPlainTextEdit::textChanged,this,[this]{ if (!m_text->toPlainText().isEmpty() && !m_text->isReadOnly()) {m_busy->hide();m_emptyIcon->hide();m_emptyRetry->hide();} });
    m_status = new QLabel(tr("正在识别…"));
    m_status->setObjectName(QStringLiteral("ocrStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setFocusPolicy(Qt::ClickFocus);
    QFont captionFont = interfaceFont;
    captionFont.setPointSizeF(qMax(9.0,interfaceFont.pointSizeF()/1.125/1.125));
    m_status->setFont(captionFont);
    m_status->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    m_status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);   // Long advice never widens the card.
    tools->addStretch();
    tools->addWidget(m_status,1);
    // Card actions live on the right of the top toolbar; copy/count live in each module footer.
    if (m_barcode) {
        m_open = iconButton(Icon::CardLink,QStringLiteral("barcodeOpen"),tr("打开链接"));
        m_open->hide();
        connect(m_open,&QToolButton::clicked,this,[this] {
            const QUrl url = barcodeUrl(m_text->toPlainText());
            if (!url.isEmpty()) QDesktopServices::openUrl(url);
        });
        textAction(m_open,tr("打开链接"));
        m_download = iconButton(Icon::Save,QStringLiteral("barcodeDownload"),tr("下载网页"));
        m_download->hide();
        connect(m_download,&QToolButton::clicked,this,&OcrDialog::downloadBarcodePage);
        textAction(m_download,tr("下载网页"));
        tools->addWidget(m_open);
        tools->addWidget(m_download);
        tools->addWidget(m_retry);
    } else if (m_translate) {
        tools->addWidget(m_view);
        tools->addWidget(m_again);
        tools->addWidget(m_retry);
    } else {
        tools->addWidget(m_format);
        tools->addWidget(search);
        tools->addWidget(m_retry);
    }
    if (m_barcode) {
        m_barcodeLabel = new QLabel;
        m_barcodeLabel->setObjectName(QStringLiteral("barcodeKind"));
        m_barcodeLabel->setStyleSheet(QStringLiteral("QLabel{background:#222326;color:#f4f5f8;border:none;border-radius:6px;padding:4px 10px;font-size:12px;}"));
        m_barcodeLabel->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
        m_barcodeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_barcodePicker = new PickerCombo;
        m_barcodePicker->setObjectName(QStringLiteral("barcodePicker"));
        m_barcodePicker->setAccessibleName(tr("选择二维码/条形码"));
        m_barcodePicker->setFocusPolicy(Qt::TabFocus);
        m_barcodePicker->setCursor(Qt::PointingHandCursor);
        m_barcodePicker->setMaxVisibleItems(18);
        m_barcodePicker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_barcodePicker->setMinimumContentsLength(20);
        m_barcodePicker->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        m_barcodePicker->hide();
        connect(m_barcodePicker,qOverload<int>(&QComboBox::currentIndexChanged),this,&OcrDialog::showBarcodeItem);
    }
    QFrame *textBlock;
    QVBoxLayout *textColumn;
    std::tie(textBlock,textColumn) = block(m_translate ? QStringLiteral("translateResultBlock") : QStringLiteral("ocrTextPanel"),4);
    textColumn->setContentsMargins(0,0,0,0);
    textColumn->setSpacing(16);
    QFrame *readingBlock;
    QVBoxLayout *readingColumn;
    std::tie(readingBlock,readingColumn) = block(QStringLiteral("ocrReadingSurface"),4);
    if (m_barcode) {
        readingColumn->addWidget(m_barcodeLabel);
        readingColumn->addWidget(m_barcodePicker);
    }
    readingColumn->addWidget(m_find);
    readingColumn->addWidget(m_text,1);
    readingColumn->addWidget(resultFooter);
    textColumn->addWidget(readingBlock,1);
    if (sourceBox) {
        // Original and translation each sit on their own block with their own bottom row.
        QVBoxLayout *sourceColumn;
        QFrame *sourceBlock;
        std::tie(sourceBlock,sourceColumn) = block(QStringLiteral("translateSourceBlock"),4);
        m_sourceBox = sourceBlock;
        sourceColumn->setContentsMargins(0,0,0,0);
        sourceColumn->setSpacing(16);
        QFrame *sourceReadingBlock;
        QVBoxLayout *sourceReadingColumn;
        std::tie(sourceReadingBlock,sourceReadingColumn) = block(QStringLiteral("ocrReadingSurface"),4);
        m_sourceLayout = sourceReadingColumn;
        m_resultLayout = readingColumn;
        sourceReadingColumn->addWidget(sourceBox,1);
        sourceReadingColumn->addWidget(sourceFooter);
        sourceColumn->addWidget(sourceReadingBlock,1);
        m_sourceBox->setMinimumSize(120,80);
        // Haze over the translation pane once the original differs from what it translated.
        m_mask = new QWidget(m_text);
        m_mask->setObjectName(QStringLiteral("retranslateMask"));
        m_mask->setAttribute(Qt::WA_StyledBackground);
        auto *maskLayout = new QVBoxLayout(m_mask);
        auto *maskButton = new QPushButton(tr("重新翻译"));
        maskButton->setObjectName(QStringLiteral("retranslateButton"));
        maskButton->setFocusPolicy(Qt::NoFocus);
        maskButton->setCursor(Qt::PointingHandCursor);
        maskButton->setToolTip(tr("原文已修改，点击重新翻译 (Ctrl+Enter)"));
        connect(maskButton,&QPushButton::clicked,this,[this] { translateLines(); });
        maskLayout->addStretch();maskLayout->addWidget(maskButton,0,Qt::AlignCenter);maskLayout->addStretch();
        auto *editedHint=new QLabel(tr("原文已更改 · Ctrl+Enter 重新翻译"),m_mask);editedHint->setAlignment(Qt::AlignCenter);editedHint->setWordWrap(true);maskLayout->addWidget(editedHint);
        m_mask->hide();
        m_text->installEventFilter(this);   // Keeps the haze covering the pane as it resizes.
        // Find / replace from either pane: its context menu, Ctrl+F or Ctrl+H, scoped to that pane.
        for (QPlainTextEdit *pane : {m_sourcePane,m_text}) {
            pane->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(pane,&QWidget::customContextMenuRequested,this,[this,pane](const QPoint &at) {
                QMenu *menu = pane->createStandardContextMenu(at);
                menu->setAttribute(Qt::WA_DeleteOnClose);
                menu->addSeparator();
                QAction *find = menu->addAction(tr("查找"),this,[this,pane] { showFind(false,pane); });
                find->setShortcut(QKeySequence::Find);
                QAction *replace = menu->addAction(tr("替换"),this,[this,pane] { showFind(true,pane); });
                replace->setShortcut(QKeySequence(Qt::CTRL|Qt::Key_H));
                replace->setEnabled(!pane->isReadOnly());
                menu->popup(pane->viewport()->mapToGlobal(at));
            });
        }
    }
    // The image stays on the left in the translate card; the original and translation
    // share a nested splitter that turns from columns to rows as the card narrows.
    if (m_translate) {
        m_textSplit = new QSplitter(Qt::Horizontal);
        m_textSplit->setObjectName(QStringLiteral("translateTextSplit"));
        m_textSplit->setChildrenCollapsible(false);
        m_textSplit->setHandleWidth(13);
        m_textSplit->addWidget(m_sourceBox);
        m_textSplit->addWidget(textBlock);
        m_split->addWidget(m_textSplit);
    } else {
        m_split->addWidget(textBlock);
    }
    m_content = new QWidget;
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(0,m_translate ? 0 : 8,0,0);
    contentLayout->addWidget(m_split,1);
    contentLayout->setSpacing(16);
    layout->addWidget(m_content,1);
    // Tab follows the visual order: window actions, toolbar, image block, then text block(s).
    QList<QWidget *> order{back,pin,minimize,m_maximize,close};
    if (m_translate) order << m_source << m_target << m_provider;
    order << imageView << copyImage << zoomIn << zoomOut << fitToggle;
    const auto appendFooter = [&order](QWidget *footer) {
        if (!footer) return;
        for (const QString &name : {QStringLiteral("Copy"), QStringLiteral("ZoomIn"),
                                    QStringLiteral("ZoomOut")}) {
            const QString prefix = footer->objectName() == QStringLiteral("translateSourceBar")
                ? QStringLiteral("translateSource") : QStringLiteral("ocrText");
            if (auto *button = footer->findChild<QToolButton *>(prefix + name))
                order << button;
        }
    };
    if (m_translate) {
        order << m_sourcePane;
        appendFooter(sourceFooter);
        order << m_text;
        appendFooter(resultFooter);
        order << m_view << m_again << m_retry;
    } else if (m_barcode) {
        order << m_barcodePicker << m_text;
        appendFooter(resultFooter);
        order << m_open << m_download << m_retry;
    } else {
        order << m_text;
        appendFooter(resultFooter);
        order << m_format << search << m_retry;
    }
    for (int i = 1; i < order.size(); ++i) setTabOrder(order[i-1],order[i]);
    // Ctrl+F / Ctrl+H search the focused pane (the source pane in the translate card), else the main text.
    const auto findHere = [this](bool replace) {
        // Focus inside the strip keeps searching the pane it already serves.
        QPlainTextEdit *target = m_find->isAncestorOf(QApplication::focusWidget()) ? m_findTarget
            : m_sourcePane && m_sourcePane->hasFocus() ? m_sourcePane : m_text;
        showFind(replace,target);
    };
    connect(new QShortcut(QKeySequence::Find,this),&QShortcut::activated,this,[findHere] { findHere(false); });
    connect(new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_H),this),&QShortcut::activated,this,[findHere] { findHere(true); });
    for (QPlainTextEdit *pane : {m_text,m_sourcePane})
        if (pane) connect(pane,&QPlainTextEdit::textChanged,this,[this,pane] {
            if (m_find->isVisible() && pane==m_findTarget) highlightMatches();
        });
    const QRect anchor = captureRect.isEmpty() ? QRect(QCursor::pos(),QSize(1,1)) : captureRect;
    const QScreen *screen = QGuiApplication::screenAt(anchor.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    m_screenRect = screen->geometry();
    m_finalSize = preferredSize(screen->availableGeometry(),m_translate);
    setMinimumSize(loadingSize());
    setGeometry(screenCentered(m_screenRect,loadingSize()));
    if (m_translate)
        setDisplay(LensSnipSettings().value(QStringLiteral("Translate/view"),0).toInt());
    showLoading(true, m_barcode ? tr("正在识别二维码…") : tr("正在识别…"));
    connect(&m_barcodeWatcher, &QFutureWatcher<BarcodeResult>::finished, this, [this] {
        if (!m_barcode || m_closed) return;
        finishBarcode(m_barcodeWatcher.result());
    });
    connect(&m_watcher, &QFutureWatcher<OcrResult>::finished, this, [this] {
        const OcrResult result = m_watcher.result();
        m_busy->hide();
        // Translate mode re-enables retry when the gateway answers, unless recognition stops here.
        if (!m_translate) m_retry->setEnabled(true);
        else if (!result.error.isEmpty() || result.lines.isEmpty() || m_cancel->load()) setActionsEnabled(true);
        if (m_cancel->load()) { showLoading(false); return; }
        if (result.error.isEmpty()) m_imageView->setTextLines(result.lines);
        if (m_translate && (!result.error.isEmpty() || result.lines.isEmpty())) {
            // Recognition stopped: the user can still type the original and translate it.
            showLoading(false);
            m_sourcePane->setReadOnly(false);
            m_sourcePane->setPlaceholderText(tr("未识别到文字，可在此输入原文"));
            m_text->setReadOnly(false);
            m_text->setPlaceholderText(tr("在上方输入原文后点击重新翻译"));
            setStatus(result.error.isEmpty() ? tr("未识别到文字") : result.error);
            applyLayout(true);
            return;
        }
        if (!result.error.isEmpty()) {
            // A failed retry brings the previous text back instead of leaving the pane empty.
            showLoading(false);
            m_text->setPlainText(m_previous);
            m_text->document()->setModified(m_previousModified);
            setStatus(result.error);
            m_text->setReadOnly(m_text->toPlainText().isEmpty());
            m_text->setPlaceholderText(QString());
            if(m_previous.isEmpty())showEmptyState(tr("文字识别暂不可用"),result.error,true);
            applyLayout(true);
            return;
        }
        if (m_translate) {
            // The pane stays busy: recognition lines now go to the gateway.
            translateResult(result);
            return;
        }
        showLoading(false);
        m_text->setReadOnly(false);
        m_ocrLineTexts.clear();
        for (const OcrLine &line : result.lines) m_ocrLineTexts.append(line.text);
        m_raw = result.text();
        m_text->setPlainText(m_raw);
        m_autoFormat->setChecked(true);
        m_text->setPlaceholderText(QString());
        if (m_raw.isEmpty()) showEmptyState(tr("未识别到文字"),tr("请调整截图范围后重新识别。"));
        m_status->clear();
        applyLayout(true);
        m_text->setFocus();
    });
    connect(this, &QDialog::finished, this, [this] {
        m_closed=true;
        if (m_cancel) m_cancel->store(true);
        if (m_barcodeCancel) m_barcodeCancel->store(true);
    });
    // A click outside a text pane or the capture drops its selection and linked marks.
    for (QWidget *widget : findChildren<QWidget *>()) widget->installEventFilter(this);
    QTimer::singleShot(0, this, &OcrDialog::start);
}

OcrDialog::~OcrDialog()  {
    m_closed = true;
    if (m_cancel) m_cancel->store(true);
    if (m_barcodeCancel) m_barcodeCancel->store(true);
}

QSize OcrDialog::preferredSize(const QRect &available, bool translate) {
    // Width chooses the reading layout; height only has two steps so the card never
    // turns into a tall strip. Text beyond the pane scrolls.
    const int height = available.height() >= 1200 ? 760 : 650;
    // Pick a width that actually lands in the intended breakpoint after the 32px margin
    // clamp; otherwise a near-breakpoint screen gets a narrow layout with huge text.
    int width;
    if (translate) {
        if (available.width() >= 1282) width = available.width() >= 2200 ? 1700 : 1500;
        else if (available.width() >= 932) width = 1100;
        else width = 680;
    } else {
        width = available.width() >= 1082 ? 1100 : 660;
    }
    const int availableHeight = qMax(320, available.height() - 32);
    const int minHeight = qMin(560, availableHeight);
    return QSize(qMin(width,qMax(640,available.width()-32)),
                 qMax(minHeight, qMin(height, availableHeight)));
}

void OcrDialog::showEmptyState(const QString &title,const QString &detail,bool error)
{
    m_busy->setTextFormat(Qt::RichText);
    m_busy->setText(QStringLiteral("<div style=\"font-size:16px;font-weight:600;color:#f4f5f8;\">%1</div>"
                                   "<div style=\"margin-top:8px;font-size:13px;color:#9c9da1;\">%2</div>")
                        .arg(title.toHtmlEscaped(),detail.toHtmlEscaped()));
    QFont font=this->font();font.setPointSizeF(qMax(10.5,font.pointSizeF()));m_busy->setFont(font);
    m_busy->setWordWrap(true);m_busy->show();
    if(error) {
        QPixmap warning=makeIcon(Icon::Warning,20,true).pixmap(20,20);
        QPainter p(&warning);p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(warning.rect(),QColor("#e9be74"));p.end();
        m_emptyIcon->setPixmap(warning);
    } else {
        const Icon icon=m_barcode?Icon::Barcode:m_translate?Icon::CardTranslate:Icon::Ocr;
        m_emptyIcon->setPixmap(cardIcon(icon,20).pixmap(20,20));
    }
    m_emptyIcon->show();m_emptyRetry->show();
    m_emptyRetry->setText(m_translate?tr("重新翻译"):tr("重新识别"));
    m_emptyRetry->setEnabled(m_retry->isEnabled());
    m_emptyRetry->setFocus();
}

void OcrDialog::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setToolTip(text);
}

void OcrDialog::showLoading(bool loading, const QString &text) {
    if (!m_loadingPage || !m_loadingBar || !m_content) return;
    const bool wasLoading = !m_loadingPage->isHidden();
    if (loading) {
        m_busy->hide();m_emptyIcon->hide();m_emptyRetry->hide();
        m_loadingBar->setText(text);
        m_loadingBar->setActive(true);
        if (m_toolbar) m_toolbar->hide();
        m_content->hide();
        m_loadingPage->show();
        setMinimumSize(loadingSize());
        setGeometry(screenCentered(m_screenRect,loadingSize()));
    } else {
        m_loadingBar->setActive(false);
        m_loadingPage->hide();
        m_content->show();
        if (m_toolbar) m_toolbar->show();
        if (wasLoading) {
            setMinimumSize(640,qMin(560,m_finalSize.height()));
            if (!m_maximized) setGeometry(screenCentered(m_screenRect,m_finalSize));
        }
        if (m_content->layout()) m_content->layout()->activate();
        m_split->updateGeometry();
    }
    m_header->setVisible(!loading);
    auto *close = findChild<QToolButton *>(QStringLiteral("ocrClose"));
    if(loading) {
        close->setFocusPolicy(Qt::NoFocus);
        close->setParent(this);close->move(width()-44,12);close->show();close->raise();
    } else {
        close->setFocusPolicy(Qt::TabFocus);
        if(close->parentWidget()==this) { close->setParent(m_header);qobject_cast<QHBoxLayout *>(m_header->layout())->addWidget(close);close->show(); }
    }
    layout()->activate();
    if (!loading) applyLayout(true);
    if (!loading && wasLoading && !m_maximized)
        setGeometry(screenCentered(m_screenRect, size()));
}

int OcrDialog::layoutClass() const {
    if (m_translate) return width() >= 1250 ? 2 : width() >= 900 ? 1 : 0;
    return width() >= 1050 ? 0 : 1;
}

void OcrDialog::applyLayout(bool force) {
    if (!m_split || m_closed || m_applyingLayout) return;
    const int cls = layoutClass();
    if (!force && cls == m_layoutClass) return;
    m_applyingLayout = true;
    m_layoutClass = cls;
    const int handle = m_split->handleWidth();
    if (m_translate) {
        // Wide: image | original | translation. Medium: image | (original / translation).
        // Narrow: texts only. The image never takes a full row above the texts.
        const bool wide = cls == 2;
        const bool showImage = m_mode == 0 && cls != 0;
        m_rows = width() < 1000;   // Compact toolbar prefixes on the narrow card.
        m_imagePanel->setVisible(showImage);
        m_sourceBox->setVisible(m_mode < 2);
        m_split->setOrientation(Qt::Horizontal);
        const int outer = qMax(1,(m_split->width() > 0 ? m_split->width() : width()-32) - handle);
        const int image = showImage ? (wide ? outer*30/100 : qBound(320,outer*34/100,400)) : 0;
        const int textWidth = qMax(1,outer-image);
        m_split->setSizes({image,textWidth});
        m_split->setStretchFactor(0,showImage ? (wide ? 3 : 34) : 0);
        m_split->setStretchFactor(1,showImage ? (wide ? 7 : 66) : 1);
        if (m_textSplit) {
            m_textSplit->setOrientation(wide ? Qt::Horizontal : Qt::Vertical);
            const int innerHandle = m_textSplit->handleWidth();
            const int inner = qMax(1,(wide ? (m_textSplit->width() > 0 ? m_textSplit->width() : textWidth)
                                         : (m_textSplit->height() > 0 ? m_textSplit->height() : m_split->height()))
                                     - innerHandle);
            const int source = m_mode < 2 ? inner/2 : 0;
            m_textSplit->setSizes({source,qMax(1,inner-source)});
            m_textSplit->setStretchFactor(0,m_mode < 2 ? 1 : 0);
            m_textSplit->setStretchFactor(1,1);
        }
    } else {
        // OCR: side by side when the card is wide enough, otherwise image over text.
        const bool rows = cls == 1;
        m_rows = rows;
        m_imagePanel->setVisible(true);
        m_split->setOrientation(rows ? Qt::Vertical : Qt::Horizontal);
        const int span = rows ? (m_split->height() > 0 ? m_split->height() : height()-120)
                              : (m_split->width() > 0 ? m_split->width() : width()-32);
        const int content = qMax(1,span-handle);
        const int image = rows ? content*45/100 : content*40/100;
        m_split->setSizes({image,qMax(1,content-image)});
        m_split->setStretchFactor(0,rows ? 9 : 4);
        m_split->setStretchFactor(1,rows ? 11 : 6);
    }
    if (m_translate) syncPickers();
    // Narrow cards keep the top actions as icons; wide cards show their labels.
    for (QToolButton *button : m_toolbar->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        const QString fullText = button->property("fullText").toString();
        if (fullText.isEmpty()) continue;
        button->setText(m_rows ? QString() : fullText);
        button->setToolButtonStyle(m_rows ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
    m_applyingLayout = false;
}

QRect OcrDialog::screenCentered(const QRect &screen, const QSize &size) {
    QRect result(screen.left() + (screen.width() - size.width()) / 2,
                 screen.top() + (screen.height() - size.height()) / 2,
                 size.width(), size.height());
    // QRect::center rounds toward zero for negative origins. Nudge by the
    // one pixel that rounding can leave between the card and screen centers.
    if (result.center().x() < screen.center().x()) result.moveLeft(result.left() + 1);
    else if (result.center().x() > screen.center().x()) result.moveLeft(result.left() - 1);
    if (result.center().y() < screen.center().y()) result.moveTop(result.top() + 1);
    else if (result.center().y() > screen.center().y()) result.moveTop(result.top() - 1);
    return result;
}

QSize OcrDialog::loadingSize() {
    return QSize(640,160);
}

void OcrDialog::start() {
    if (m_barcode) { startBarcode(); return; }
    if (m_closed || m_watcher.isRunning()) return;
    const bool initialLoad = m_loadingPage && !m_loadingPage->isHidden();
    const bool userEdit = m_text->document()->isModified() || (m_sourcePane && m_sourcePane->document()->isModified());
    if (userEdit && QMessageBox::question(this,tr("重新识别"),
            tr("重新识别会替换已修改的文本，是否继续？"),
            QMessageBox::Yes|QMessageBox::No,QMessageBox::No) != QMessageBox::Yes) return;
    // The confirmation pumps events and may close the card before it returns.
    if (m_closed || m_watcher.isRunning()) return;
    auto &engine = OcrEngine::instance();
    bool expected = false;
    if (!engine.busy.compare_exchange_strong(expected,true)) {
        showLoading(false);
        setStatus(tr("上一项识别正在结束，请稍候再点击重新识别"));
        return;
    }
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    const QImage image = m_image;
    // Disabling the focused retry would hand focus to copy and outline it; park focus on the text.
    if (m_retry->hasFocus() || (m_again && m_again->hasFocus())) m_text->setFocus();
    m_previous = m_text->toPlainText();
    m_previousModified = m_text->document()->isModified();
    m_ocrLineTexts.clear();
    m_text->clear();
    m_text->setReadOnly(true);
    if (m_translate) {
        m_sourcePane->setReadOnly(true);
        {   const QSignalBlocker blocker(m_sourcePane);   // Not a user edit.
            m_sourcePane->clear();
        }
        m_translatedSource.clear();
        setActionsEnabled(false);
    } else {
        m_retry->setEnabled(false);
    }
    if (m_translate) {
        m_sourcePane->setPlaceholderText(tr("正在识别…"));
        m_text->setPlaceholderText(QString());
    }
    m_busy->setText(tr("正在识别…"));
    if (initialLoad) showLoading(true,tr("正在识别…"));
    else m_busy->show();
    setStatus(m_translate ? QString() : tr("正在识别…"));
    m_watcher.setFuture(QtConcurrent::run([&engine,image,cancel] {
        // A failed worker must not wedge the busy gate or rethrow on the GUI thread.
        OcrResult result;
        try {
            result = recognizeChunked(image,cancel);
        } catch (const std::exception &error) {
            result.error = OcrDialog::tr("识别失败：%1").arg(QString::fromUtf8(error.what()));
        } catch (...) {
            result.error = OcrDialog::tr("识别线程异常");
        }
        engine.busy.store(false);
        return result;
    }));
}

void OcrDialog::startBarcode()
{
    if (m_closed || m_barcodeWatcher.isRunning()) return;
    cancelDownload();
    m_barcodeCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_barcodeCancel;
    const bool initialLoad = m_loadingPage && !m_loadingPage->isHidden();
    m_barcodeItems.clear();
    m_barcodePicker->clear();
    m_barcodePicker->hide();
    m_barcodeLabel->setText(tr("正在识别…"));
    m_imageView->setBarcodeRegions({},-1);
    m_text->clear();
    m_text->setReadOnly(true);
    m_open->hide();
    m_download->hide();
    m_retry->setEnabled(false);
    m_status->clear();
    if (initialLoad) {
        showLoading(true,tr("正在识别二维码…"));
    } else {
        m_busy->setText(tr("正在识别…"));
        m_busy->show();
    }
    const QImage image = m_image;
    m_barcodeWatcher.setFuture(QtConcurrent::run([image,cancel] {
        return BarcodeEngine::recognize(image,cancel);
    }));
}

void OcrDialog::finishBarcode(const BarcodeResult &result)
{
    if (m_closed) return;
    m_busy->hide(); m_emptyIcon->hide(); m_emptyRetry->hide();
    m_retry->setEnabled(true);
    if (result.cancelled) {
        showLoading(false);
        return;
    }
    m_barcodeItems = result.items;
    m_barcodePicker->clear();
    if (!result.error.isEmpty()) {
        m_barcodeLabel->clear(); m_barcodeLabel->hide(); m_barcodePicker->hide(); m_text->clear();
        showEmptyState(tr("识别失败"),result.error,true);
        m_imageView->setBarcodeRegions({},-1);
    } else if (m_barcodeItems.isEmpty()) {
        m_barcodeLabel->clear(); m_barcodeLabel->hide(); m_barcodePicker->hide();
        m_text->clear();
        showEmptyState(tr("未识别到二维码或条形码"),tr("请确保图像清晰且码完整。"));
        m_imageView->setBarcodeRegions({},-1);
    } else {
        for (const BarcodeItem &item : m_barcodeItems) {
            QString preview = item.text.simplified();
            if (preview.size() > 72) preview = preview.left(69) + QStringLiteral("…");
            m_barcodePicker->addItem(QStringLiteral("%1 · %2").arg(item.format,preview));
        }
        m_barcodeLabel->show();
        m_barcodePicker->setVisible(m_barcodeItems.size() > 1);
        showBarcodeItem(0);
    }
    showLoading(false);
    m_status->clear();
    m_text->setFocus();
}

void OcrDialog::showBarcodeItem(int index)
{
    if (index < 0 || index >= m_barcodeItems.size()) return;
    const BarcodeItem &item = m_barcodeItems[index];
    m_busy->hide();m_emptyIcon->hide();m_emptyRetry->hide();m_text->setStyleSheet(QString());
    m_text->setPlainText(item.text);
    m_text->setStyleSheet(QStringLiteral("QPlainTextEdit{background:#111216;border:none;border-radius:8px;padding:12px;}"));
    m_barcodeLabel->setText(m_barcodeItems.size() == 1
        ? item.format
        : QStringLiteral("%1 · %2/%3").arg(item.format).arg(index+1).arg(m_barcodeItems.size()));
    const QUrl url = barcodeUrl(item.text);
    m_open->setVisible(!url.isEmpty());
    const QString scheme = url.scheme().toLower();
    m_download->setVisible(scheme == QStringLiteral("http") || scheme == QStringLiteral("https"));
    QVector<QPolygonF> regions;
    regions.reserve(m_barcodeItems.size());
    for (const BarcodeItem &entry : m_barcodeItems) regions.append(entry.quad);
    m_imageView->setBarcodeRegions(regions,index);
    m_status->clear();
}

void OcrDialog::downloadBarcodePage()
{
    const QUrl url = barcodeUrl(m_text->toPlainText());
    const QString scheme = url.scheme().toLower();
    if (m_downloadReply || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")))
        return;
    QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (directory.isEmpty()) directory = QDir::homePath();
    const QString path = QFileDialog::getSaveFileName(
        this,tr("下载网页"),QDir(directory).filePath(barcodeDownloadName(url)),
        tr("网页 (*.html *.htm);;所有文件 (*)"));
    if (path.isEmpty()) return;
    if (!m_downloader) m_downloader = new QNetworkAccessManager(this);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = m_downloader->get(request);
    m_downloadReply = reply;
    m_download->setEnabled(false);
    setStatus(tr("正在下载网页…"));
    connect(reply,&QNetworkReply::finished,this,[this,reply,path] {
        if (m_downloadReply != reply) return;
        m_downloadReply = nullptr;
        m_download->setEnabled(true);
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(tr("下载失败：%1").arg(reply->errorString()));
            return;
        }
        const QByteArray data = reply->readAll();
        // Response body only: scripts, styles and images are not fetched for an offline bundle.
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            setStatus(tr("保存失败：%1").arg(file.errorString()));
            return;
        }
        setStatus(tr("已保存到 %1").arg(QDir::toNativeSeparators(path)));
    });
}

void OcrDialog::cancelDownload()
{
    if (!m_downloadReply) return;
    QNetworkReply *reply = m_downloadReply;
    m_downloadReply = nullptr;
    reply->abort();
    reply->deleteLater();
    if (m_download) m_download->setEnabled(true);
}

void OcrDialog::translateResult(const OcrResult &result)
{
    // Recognized text goes into the editable source pane; typing there re-translates.
    m_ocrLineTexts.clear();
    QString joined;
    for (const OcrLine &line : result.lines) {
        m_ocrLineTexts.append(line.text);
        if (!joined.isEmpty()) joined += QLatin1Char('\n');
        joined += line.text;
    }
    {   const QSignalBlocker blocker(m_sourcePane);
        m_sourcePane->setPlainText(joined);
        m_sourcePane->document()->setModified(false);   // Recognized text is not a user edit.
    }
    m_sourcePane->setReadOnly(false);
    translateLines();
}

void OcrDialog::translateLines()
{
    m_emptyIcon->hide();m_emptyRetry->hide();
    if (m_closed || m_watcher.isRunning()) return;   // Recognition finishing will translate.
    const bool initialLoad = m_loadingPage && !m_loadingPage->isHidden();
    if (!m_gateway) m_gateway = new TranslateGateway(this);   // Child: aborted with the card.
    m_gateway->abort();          // A new choice supersedes the running request.
    const QStringList lines = m_sourcePane->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        showLoading(false);
        m_text->clear();
        m_text->setReadOnly(false);
        m_text->setPlaceholderText(tr("在上方输入原文后点击重新翻译"));
        m_status->clear();
        m_busy->hide();
        m_translatedSource = m_sourcePane->toPlainText();
        setActionsEnabled(true);
        applyLayout(true);
        return;
    }
    // Re-translating replaces the pane; a failure brings this text back.
    m_previous = m_text->toPlainText();
    m_previousModified = m_text->document()->isModified();
    if (m_retry->hasFocus() || m_again->hasFocus()) m_text->setFocus();
    const QString requested = m_sourcePane->toPlainText();
    setActionsEnabled(false);
    m_text->clear();
    m_text->setReadOnly(true);
    m_text->setPlaceholderText(QString());
    m_busy->setText(tr("正在翻译…"));
    if (initialLoad) showLoading(true,tr("正在翻译…"));
    else m_busy->show();
    m_status->clear();
    const auto provider = TranslateGateway::Provider(m_provider->currentData().toInt());
    // Auto walks the engines inside the gateway: one engine failing never reaches the card.
    m_gateway->translate(lines, m_source->currentData().toString(), m_target->currentData().toString(),
                         provider, [this, provider, requested](const TranslateResult &t) {
        if (m_closed || t.code == TranslateResult::Error::Cancelled) return;
        m_busy->hide();
        showLoading(false);
        if (t.code == TranslateResult::Error::None) m_translatedSource = requested;
        setActionsEnabled(true);   // Also brings the haze back if this run failed or the original changed meanwhile.
        if (t.code != TranslateResult::Error::None || !t.error.isEmpty()) {
            // Keep the previous translation; explain in plain words, never the raw engine error.
            m_text->setPlainText(m_previous);
            m_text->document()->setModified(m_previousModified);
            m_text->setReadOnly(m_text->toPlainText().isEmpty());
            QString message;
            if (t.code == TranslateResult::Error::Unsupported)
                message = tr("暂不支持这组语言，请更换原文或译文语言");
            else if (t.code == TranslateResult::Error::Invalid)
                message = tr("原文过长或格式不支持，请缩短后重试");
            else if (provider == TranslateGateway::Provider::Auto)
                message = tr("翻译服务暂时不可用，请检查网络后重试");
            else
                message = tr("%1暂时无法翻译，请稍后重试或切换为其他翻译引擎").arg(m_provider->currentText());
            m_text->setPlaceholderText(message);
            setStatus(message);
            if(m_previous.isEmpty()) {m_text->setPlaceholderText(QString());showEmptyState(tr("翻译未完成"),message,true);}
            applyLayout(true);
            return;
        }
        m_text->setReadOnly(false);
        m_raw = t.lines.join(QLatin1Char('\n'));
        m_text->setPlainText(m_raw);
        m_autoFormat->setChecked(true);
        // Automatic pickers name what this run actually used: "语言检测 (韩语)", "翻译引擎 (Edge)".
        bool known = false;
        const int engine = m_provider->findData(int(TranslateGateway::providerFromName(t.provider,&known)));
        const QString engineName = known && engine >= 0 ? m_provider->itemText(engine) : t.provider;
        if (m_source->currentIndex() == 0) {
            const int at = t.detectedSource.isEmpty() ? -1 : m_source->findData(t.detectedSource);
            m_source->setDetected(at > 0 ? m_source->itemText(at) : QString());
        }
        if (m_provider->currentIndex() == 0) m_provider->setDetected(engineName);
        m_status->clear();
        // Someone typing in the source keeps the caret there; otherwise the result takes focus.
        if (!m_sourcePane->hasFocus()) m_text->setFocus();
        applyLayout(true);
    });
}

void OcrDialog::applyHighlights(QPlainTextEdit *pane)
{
    if (!pane) return;
    QList<QTextEdit::ExtraSelection> marks;
    const auto add = [&marks](const QList<QTextEdit::ExtraSelection> &from) {
        for (const QTextEdit::ExtraSelection &mark : from) marks.append(mark);
    };
    if (pane == m_findTarget) add(m_findMarks);
    if (pane == m_text) add(m_resultLinkMarks);
    pane->setExtraSelections(marks);
}

static QTextEdit::ExtraSelection linkMark(const QTextCursor &cursor)
{
    QTextEdit::ExtraSelection mark;
    mark.cursor = cursor;
    // Same solid blue as a text selection, not a marker-pen wash.
    mark.format.setBackground(Bar::selection);
    mark.format.setForeground(Bar::ink);
    return mark;
}

void OcrDialog::setSelectionMark(QPlainTextEdit *pane, const QTextCursor &selection,
                                 QList<QTextEdit::ExtraSelection> &marks)
{
    marks.clear();
    if (pane && selection.hasSelection()) marks.append(linkMark(selection));
    applyHighlights(pane);
}

void OcrDialog::clearLinkHighlights()
{
    m_resultLinkMarks.clear();
    if (m_text) applyHighlights(m_text);
    if (m_imageView) m_imageView->highlightTextRange(-1,-1);
}

void OcrDialog::clearPaneSelection(QPlainTextEdit *pane)
{
    if (!pane) return;
    // Collapse the native selection without letting the change handler run twice.
    if (pane->textCursor().hasSelection()) {
        const QSignalBlocker blocker(pane);
        QTextCursor cursor = pane->textCursor();
        cursor.setPosition(cursor.selectionStart());
        pane->setTextCursor(cursor);
    }
    if (pane == m_text) {
        if (!m_translate) clearLinkHighlights();
    } else if (pane == m_sourcePane && m_imageView) {
        m_imageView->highlightText(QString());
    }
}

void OcrDialog::clearSelectionHighlights()
{
    clearPaneSelection(m_text);
    clearPaneSelection(m_sourcePane);
    if (m_imageView) m_imageView->clearSelection();
}

static bool mapTextRange(const QString &source, int from, int to,
                         const QString &target, int *start, int *end)
{
    if (!start || !end || from < 0 || to < from || to >= source.size()) return false;
    if (source == target) { *start = from; *end = to; return true; }
    const QString needle = source.mid(from,to-from+1);
    if (needle.isEmpty()) return false;
    const int at = target.indexOf(needle);
    if (at < 0 || target.indexOf(needle,at+1) >= 0) return false;
    *start = at; *end = at + needle.size() - 1;
    return true;
}

void OcrDialog::highlightImageForPane(QPlainTextEdit *pane)
{
    if (!pane || !m_imageView) return;
    const QTextCursor cursor = pane->textCursor();
    if (!cursor.hasSelection()) {
        m_imageView->highlightTextRange(-1,-1);
        return;
    }
    int from = -1, to = -1;
    const QString recognized = m_ocrLineTexts.join(QLatin1Char('\n'));
    if (mapTextRange(pane->toPlainText(),cursor.selectionStart(),cursor.selectionEnd()-1,
                     recognized,&from,&to))
        m_imageView->highlightTextRange(from,to);
    else
        m_imageView->highlightText(cursor.selectedText());
}

void OcrDialog::linkFromOcrTextSelection()
{
    if (m_translate || m_barcode || !m_text || !m_imageView) return;
    const QTextCursor cursor = m_text->textCursor();
    // Deselecting either pane drops every linked mark; clicking elsewhere does the same.
    if (!cursor.hasSelection()) {
        clearLinkHighlights();
        m_imageView->clearSelection();
        return;
    }
    m_imageView->clearSelection();
    setSelectionMark(m_text, cursor, m_resultLinkMarks);
    highlightImageForPane(m_text);
}

void OcrDialog::linkFromOcrImageSelection()
{
    if (m_barcode || !m_imageView) return;
    QPlainTextEdit *pane = m_translate ? m_sourcePane : m_text;
    if (!pane) return;
    int from = -1, to = -1;
    if (!m_imageView->selectedTextRange(&from,&to)) {
        clearLinkHighlights();
        clearPaneSelection(pane);
        return;
    }
    clearLinkHighlights();
    clearPaneSelection(pane);
    const QString recognized = m_ocrLineTexts.join(QLatin1Char('\n'));
    int start = -1, end = -1;
    if (mapTextRange(recognized,from,to,pane->toPlainText(),&start,&end)) {
        QTextCursor cursor(pane->document());
        cursor.setPosition(start);
        cursor.setPosition(end+1,QTextCursor::KeepAnchor);
        const QSignalBlocker block(pane);
        pane->setTextCursor(cursor);
        return;
    }
    // An edited or reformatted pane may no longer hold the exact range; fall back to its line(s).
    const QVector<int> lines = m_imageView->selectedLines();
    const QStringList textLines = pane->toPlainText().split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const bool untouched = pane->toPlainText() == recognized;
    QVector<int> blocks;
    for (int line : lines) {
        int block = -1;
        if (untouched) block = line;
        else if (line >= 0 && line < m_ocrLineTexts.size()) {
            const int at = textLines.indexOf(m_ocrLineTexts[line]);
            if (at >= 0 && textLines.indexOf(m_ocrLineTexts[line], at + 1) < 0) block = at;
        }
        if (block >= 0 && !blocks.contains(block)) blocks.append(block);
    }
    if (blocks.isEmpty()) return;
    const QTextBlock first = pane->document()->findBlockByNumber(blocks.first());
    const QTextBlock last = pane->document()->findBlockByNumber(blocks.last());
    if (!first.isValid() || !last.isValid()) return;
    QTextCursor cursor(pane->document());
    cursor.setPosition(first.position());
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    const QSignalBlocker block(pane);
    pane->setTextCursor(cursor);
}

void OcrDialog::populateLanguages(const QString &source, const QString &target)
{
    // Lists follow the chosen engine's language matrix; a choice it lacks falls back.
    const auto provider = TranslateGateway::Provider(m_provider->currentData().toInt());
    const QSignalBlocker blockSource(m_source), blockTarget(m_target);
    m_source->clear();
    m_source->addItem(tr("自动检测"),QString());
    for (const TranslateLanguage &language : TranslateGateway::supportedSourceLanguages(provider))
        m_source->addItem(language.name,language.code);
    const int s = m_source->findData(source);
    m_source->setCurrentIndex(s < 0 ? 0 : s);
    m_target->clear();
    for (const TranslateLanguage &language : TranslateGateway::supportedTargetLanguages(provider))
        m_target->addItem(language.name,language.code);
    int t = m_target->findData(target);
    if (t < 0) t = m_target->findData(QStringLiteral("zh"));
    m_target->setCurrentIndex(t < 0 ? 0 : t);
}

void OcrDialog::saveTranslateChoice() const
{
    LensSnipSettings settings;
    settings.remove(QStringLiteral("Translate/provider"));   // Engine is per card; drop the old stored choice.
    settings.setValue(QStringLiteral("Translate/source"),m_source->currentData().toString());
    settings.setValue(QStringLiteral("Translate/target"),m_target->currentData().toString());
}

void OcrDialog::setDisplay(int mode)
{
    // 0: image, original and translation; 1: original and translation; 2: translation only.
    m_mode = mode = qBound(0,mode,2);
    m_imagePanel->setVisible(mode == 0);
    m_sourceBox->setVisible(mode < 2);
    if (mode == 2 && m_findTarget == m_sourcePane && m_find->isVisible()) hideFind();
    for (QAction *action : m_view->menu()->actions())
        action->setChecked(action->data().toInt() == mode);
    if (m_content && m_content->isVisible()) applyLayout(true);
}

void OcrDialog::syncPickers()
{
    // A hand-picked item drops what an earlier automatic run detected. The stacked card is
    // only 500px wide, so its pickers use shorter prefixes than the three-column layout.
    m_source->setPrefix(m_rows ? tr("语言") : tr("语言检测"));
    m_provider->setPrefix(m_rows ? tr("引擎") : tr("翻译引擎"));
    for (PickerCombo *box : {m_source,m_provider})
        if (box->currentIndex() != 0) box->setDetected(QString());
}

void OcrDialog::setActionsEnabled(bool enabled)
{
    m_retry->setEnabled(enabled);
    if (m_again) m_again->setEnabled(enabled);
    updateMask();
}

void OcrDialog::updateMask()
{
    // Shown only between runs, while the original differs from the text the translation came from.
    if (!m_mask) return;
    const QString source = m_sourcePane->toPlainText();
    const bool stale = m_retry->isEnabled() && !m_sourcePane->isReadOnly()
                       && !source.trimmed().isEmpty() && source != m_translatedSource;
    if (stale && !m_mask->isVisible()) { m_mask->setGeometry(m_text->rect()); m_mask->raise(); }
    m_mask->setVisible(stale);
}

void OcrDialog::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Bar::surface);
    painter.drawRoundedRect(QRectF(rect()), 12, 12);
}

void OcrDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    applyLayout(false);
}

void OcrDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    applyLayout(true);
}

QString OcrDialog::formatText(const QString &text, TextFormat kind) {
    const auto alnum = [](QChar c) { return c.unicode()<0x80 && c.isLetterOrNumber(); };
    QString out;
    out.reserve(text.size());
    for (int i=0;i<text.size();++i) {
        const QChar c = text.at(i);
        const QChar before = i ? text.at(i-1) : QChar(), after = i+1<text.size() ? text.at(i+1) : QChar();
        switch (kind) {
        case TextFormat::Auto:
            break;
        case TextFormat::JoinLines:
            if (c==QLatin1Char('\n')) {
                // Latin words split across lines keep one space; CJK lines join directly.
                if (alnum(before) && alnum(after)) out += QLatin1Char(' ');
                continue;
            }
            break;
        case TextFormat::RemoveSpaces:
            if (c!=QLatin1Char('\n') && c.isSpace()) continue;
            break;
        case TextFormat::FullWidth:
            // Punctuation only; marks inside numbers, versions and URLs (3.14, a-b) stay half width.
            if (c.unicode()>0x20 && c.unicode()<0x7f && !alnum(c) && !(alnum(before) && alnum(after))) {
                out += c==QLatin1Char('.') ? QChar(0x3002) : QChar(c.unicode()+0xfee0);
                continue;
            }
            break;
        case TextFormat::HalfWidth:
            if (c.unicode()==0x3002) { out += QLatin1Char('.'); continue; }
            if (c.unicode()==0x3001) { out += QLatin1Char(','); continue; }
            if (c.unicode()>0xff00 && c.unicode()<0xff5f && !QChar(c.unicode()-0xfee0).isLetterOrNumber()) {
                out += QChar(c.unicode()-0xfee0);
                continue;
            }
            break;
        }
        out += c;
    }
    return out;
}

QList<QTextCursor> OcrDialog::matches() const {
    QList<QTextCursor> found;
    const QString needle = m_findEdit->text();
    if (needle.isEmpty()) return found;
    QTextCursor cursor = m_findTarget->document()->find(needle,0);
    while (!cursor.isNull() && found.size()<10000) {
        found.append(cursor);
        cursor = m_findTarget->document()->find(needle,cursor);
    }
    return found;
}

void OcrDialog::highlightMatches(bool reveal) {
    const QList<QTextCursor> found = matches();
    m_match = found.isEmpty() ? -1 : qBound(0,m_match,int(found.size())-1);
    m_findMarks.clear();
    for (int i=0;i<found.size();++i) {
        QTextEdit::ExtraSelection mark;
        mark.cursor = found[i];
        mark.format.setBackground(i==m_match ? QColor(0xff,0x9f,0x1a) : QColor(0xff,0xe5,0x4c));
        mark.format.setForeground(QColor(0x18,0x18,0x1b));   // Dark ink keeps find hits readable on amber.
        m_findMarks.append(mark);
    }
    applyHighlights(m_findTarget);
    m_findCount->setText(found.isEmpty() ? (m_findEdit->text().isEmpty() ? QString() : tr("无结果"))
                                         : QStringLiteral("%1/%2").arg(m_match+1).arg(found.size()));
    if (reveal && m_match>=0) {
        QTextCursor caret = found[m_match];
        caret.clearSelection();
        m_findTarget->setTextCursor(caret);
        m_findTarget->ensureCursorVisible();
    }
}

void OcrDialog::showFind(bool replace, QPlainTextEdit *target) {
    if (!target) target = m_text;
    if (target != m_findTarget) {
        // Move the strip into the pane being searched; find marks never linger in the
        // other one, and line-link marks stay where they are.
        QPlainTextEdit *previous = m_findTarget;
        m_findMarks.clear();
        m_findTarget = target;
        applyHighlights(previous);
        applyHighlights(m_findTarget);
        if (m_resultLayout) (target==m_sourcePane ? m_sourceLayout : m_resultLayout)->insertWidget(0,m_find);
    }
    const QString selected = target->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)) m_findEdit->setText(selected);
    m_find->show();
    m_replaceRow->setVisible(replace);
    m_replaceToggle->setIcon(cardIcon(replace ? Icon::CardDown : Icon::CardRight));
    m_findEdit->setFocus();
    m_findEdit->selectAll();
    highlightMatches(true);
}

void OcrDialog::hideFind() {
    m_find->hide();
    m_findMarks.clear();
    applyHighlights(m_findTarget);
    m_findTarget->setFocus();
}

void OcrDialog::findStep(int direction) {
    const int count = matches().size();
    if (!count) return;
    m_match = ((m_match<0 ? 0 : m_match+direction)%count+count)%count;
    highlightMatches(true);
}

void OcrDialog::replaceOne() {
    const QList<QTextCursor> found = matches();
    if (m_findTarget->isReadOnly() || m_match<0 || m_match>=found.size()) return;
    QTextCursor cursor = found[m_match];
    cursor.insertText(m_replaceEdit->text());   // m_match now points at the following hit.
    highlightMatches(true);
}

void OcrDialog::replaceAll() {
    const QList<QTextCursor> found = matches();
    if (m_findTarget->isReadOnly() || found.isEmpty()) return;
    QTextCursor edit(m_findTarget->document());
    edit.beginEditBlock();                       // One undo step, back to front so offsets stay valid.
    for (int i=found.size()-1;i>=0;--i) {
        edit.setPosition(found[i].selectionStart());
        edit.setPosition(found[i].selectionEnd(),QTextCursor::KeepAnchor);
        edit.insertText(m_replaceEdit->text());
    }
    edit.endEditBlock();
    highlightMatches();
}

bool OcrDialog::event(QEvent *event)  {
    if (event->type()==QEvent::WindowDeactivate) {
        // Popups (menus, combo lists) must not drop the selection they act on.
        QTimer::singleShot(0,this,[this] {
            if (!isActiveWindow() && !QApplication::activePopupWidget()) clearSelectionHighlights();
        });
    }
    return QDialog::event(event);
}

bool OcrDialog::eventFilter(QObject *object,QEvent *event)  {
    if (event->type()==QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(object);
        const bool onScrollBar = qobject_cast<QAbstractSlider *>(widget) != nullptr;
        if (mouse->button()==Qt::LeftButton && widget && !onScrollBar) {
            const bool onButton = qobject_cast<QAbstractButton *>(widget) != nullptr;
            const auto inside = [widget](QWidget *pane) {
                return pane && (widget==pane || pane->isAncestorOf(widget));
            };
            const bool inText = !onButton && inside(m_text);
            const bool inSource = !onButton && inside(m_sourcePane);
            const bool inImage = !onButton && inside(m_imageView);
            const bool inTextArea = m_text && (widget==m_text->viewport()
                                               || m_text->viewport()->isAncestorOf(widget));
            // Keep the selection in the pane being clicked; drop every other one.
            if (!inText) clearPaneSelection(m_text);
            else if (inTextArea && !m_translate) clearLinkHighlights();
            if (!inSource) clearPaneSelection(m_sourcePane);
            if (!inImage && m_imageView) m_imageView->clearSelection();
        }
    }
    if (object==m_findEdit || object==m_replaceEdit) {
        if (event->type()==QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key()==Qt::Key_Escape) { hideFind(); return true; }
            if (key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter) {
                if (object==m_replaceEdit) replaceOne();
                else findStep(key->modifiers().testFlag(Qt::ShiftModifier) ? -1 : 1);
                return true;
            }
        }
        return QDialog::eventFilter(object,event);
    }
    if (object==m_text) {
        if (event->type()==QEvent::Resize && m_mask) m_mask->setGeometry(m_text->rect());
        return QDialog::eventFilter(object,event);
    }
    return QDialog::eventFilter(object,event);
}

void OcrDialog::mousePressEvent(QMouseEvent *event)  {
    if (event->button()==Qt::LeftButton) {
        clearSelectionHighlights();
        const QPoint at = event->position().toPoint();
        if (const Qt::Edges edges = resizeEdges(at)) {
            // Resizing works maximized too: the card leaves the maximized state and keeps its current frame.
            if (m_maximized) { m_maximized = false; updateMaximizeButton(); }
            m_resizing = edges;
            m_pressGlobal = event->globalPosition().toPoint();
            m_pressGeometry = geometry();
            event->accept(); return;
        }
        if (dragsWindow(at)) {
            m_dragging = true;
            m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
            event->accept(); return;
        }
    }
    QDialog::mousePressEvent(event);
}

void OcrDialog::mouseMoveEvent(QMouseEvent *event)  {
    const QPoint global = event->globalPosition().toPoint();
    if (m_resizing) {
        // Opposite edges stay put; the moving edge stops where the card reaches its minimum size.
        const QSize least = minimumSize().expandedTo(minimumSizeHint());
        const QPoint delta = global - m_pressGlobal;
        QRect next = m_pressGeometry;
        if (m_resizing & Qt::LeftEdge) next.setLeft(qMin(m_pressGeometry.left()+delta.x(),m_pressGeometry.right()+1-least.width()));
        if (m_resizing & Qt::RightEdge) next.setRight(qMax(m_pressGeometry.right()+delta.x(),m_pressGeometry.left()+least.width()-1));
        if (m_resizing & Qt::TopEdge) next.setTop(qMin(m_pressGeometry.top()+delta.y(),m_pressGeometry.bottom()+1-least.height()));
        if (m_resizing & Qt::BottomEdge) next.setBottom(qMax(m_pressGeometry.bottom()+delta.y(),m_pressGeometry.top()+least.height()-1));
        setGeometry(next);
        event->accept(); return;
    }
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(global - m_dragOffset);
        event->accept(); return;
    }
    updateResizeCursor(resizeEdges(event->position().toPoint()));
    QDialog::mouseMoveEvent(event);
}

void OcrDialog::mouseReleaseEvent(QMouseEvent *event)  {
    if (event->button()!=Qt::LeftButton) { QDialog::mouseReleaseEvent(event); return; }
    const bool resized = bool(m_resizing);
    m_resizing = {};
    m_dragging = false;
    if (resized) updateResizeCursor(resizeEdges(mapFromGlobal(QCursor::pos())));
    event->accept();
}

void OcrDialog::mouseDoubleClickEvent(QMouseEvent *event)  {
    // Only the title bar maximizes; a double click anywhere else keeps its own meaning.
    const QPoint at = event->position().toPoint();
    if (event->button()==Qt::LeftButton && m_header->geometry().contains(at) && dragsWindow(at)) {
        m_resizing = {};
        m_dragging = false;
        toggleMaximize();
        event->accept(); return;
    }
    QDialog::mouseDoubleClickEvent(event);
}

void OcrDialog::leaveEvent(QEvent *event)  {
    if (!m_resizing) unsetCursor();
    QDialog::leaveEvent(event);
}

Qt::Edges OcrDialog::resizeEdges(const QPoint &point) const {
    // The outer half of the child-free margin reacts; corners combine two edges.
    const int band = qMax(1,ResizeBand/2);
    Qt::Edges edges;
    if (!rect().contains(point)) return edges;
    if (point.x() < band) edges |= Qt::LeftEdge;
    else if (point.x() > width()-1-band) edges |= Qt::RightEdge;
    if (point.y() < band) edges |= Qt::TopEdge;
    else if (point.y() > height()-1-band) edges |= Qt::BottomEdge;
    return edges;
}

void OcrDialog::updateResizeCursor(Qt::Edges edges) {
    const bool horizontal = edges & (Qt::LeftEdge|Qt::RightEdge), vertical = edges & (Qt::TopEdge|Qt::BottomEdge);
    if (horizontal && vertical)
        setCursor(bool(edges & Qt::LeftEdge) == bool(edges & Qt::TopEdge) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    else if (horizontal) setCursor(Qt::SizeHorCursor);
    else if (vertical) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
}

bool OcrDialog::dragsWindow(const QPoint &point) const {
    // Empty chrome, labels and block backgrounds move the card; controls keep their own drags.
    for (QWidget *widget = childAt(point); widget && widget != this; widget = widget->parentWidget())
        if (widget->property("NoWindowDrag").toBool() || qobject_cast<QAbstractButton *>(widget)
            || qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QLineEdit *>(widget)
            || qobject_cast<QComboBox *>(widget) || qobject_cast<QAbstractSlider *>(widget)
            || qobject_cast<QAbstractScrollArea *>(widget) || qobject_cast<QSplitterHandle *>(widget))
            return false;
    return rect().contains(point);
}

void OcrDialog::toggleMaximize() {
    // Fills the work area of the card's screen by geometry and keeps the frame resizable and draggable.
    if (!m_maximized) {
        const QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
        if (!screen) screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        m_restoreGeometry = geometry();
        setGeometry(screen->availableGeometry());
        m_maximized = true;
    } else {
        if (m_restoreGeometry.isValid()) setGeometry(m_restoreGeometry);
        m_maximized = false;
    }
    updateMaximizeButton();
    applyLayout(true);
}

void OcrDialog::updateMaximizeButton() {
    m_maximize->setIcon(cardIcon(m_maximized ? Icon::CardRestore : Icon::CardMaximize));
    const QString tip = m_maximized ? tr("还原") : tr("最大化");
    m_maximize->setToolTip(tip); m_maximize->setAccessibleName(tip);
}
