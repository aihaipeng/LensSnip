#include "capture/tool_layout_dialog.h"
#include "shared/widgets.h"
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <functional>

namespace {
const char kToolMime[] = "application/x-lenssnip-tool";

QString normalizedSide(const QString &side)
{
    return (side == QStringLiteral("top") || side == QStringLiteral("right")
            || side == QStringLiteral("left") || side == QStringLiteral("hidden"))
        ? side : QStringLiteral("bottom");
}

void setDropHighlight(QWidget *widget, bool on)
{
    if (!widget || widget->property("drop").toBool() == on) return;
    widget->setProperty("drop", on);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

class LayoutButton : public QToolButton
{
public:
    explicit LayoutButton(const QString &id, QWidget *parent = nullptr)
        : QToolButton(parent), m_id(id) {}
    QString id() const { return m_id; }
    std::function<void(const QString &, LayoutButton *)> dragStarted;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_press = event->position().toPoint();
            m_armed = true;
        }
        QToolButton::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_armed && (event->buttons() & Qt::LeftButton)
            && (event->position().toPoint() - m_press).manhattanLength()
                >= QApplication::startDragDistance()) {
            m_armed = false;
            setDown(false);
            if (dragStarted) dragStarted(m_id, this);
            return;
        }
        QToolButton::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_armed = false;
        QToolButton::mouseReleaseEvent(event);
    }

private:
    QString m_id;
    QPoint m_press;
    bool m_armed = false;
};

class TitleBar : public QWidget
{
public:
    TitleBar(QDialog *dialog, const QString &title, QWidget *parent)
        : QWidget(parent), m_dialog(dialog)
    {
        setMinimumHeight(32);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto *label = new QLabel(title, this);
        label->setObjectName(QStringLiteral("toolLayoutTitle"));
        QFont font = label->font();
        font.setPointSizeF(15);   // 20 px panel-title tier.
        label->setFont(font);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(label);
        row->addStretch();
        auto *close = new QToolButton(this);
        close->setObjectName(QStringLiteral("toolLayoutClose"));
        close->setIcon(barIcon(Icon::Cancel, 20));
        close->setIconSize(QSize(20, 20));
        close->setFixedSize(32, 32);
        close->setCursor(Qt::PointingHandCursor);
        close->setFocusPolicy(Qt::StrongFocus);
        close->setToolTip(QCoreApplication::translate("ToolLayoutDialog", "关闭"));
        close->setAccessibleName(close->toolTip());
        connect(close, &QToolButton::clicked, dialog, &QDialog::reject);
        row->addWidget(close);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_drag = event->globalPosition().toPoint() - m_dialog->pos();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((event->buttons() & Qt::LeftButton) && !m_drag.isNull())
            m_dialog->move(event->globalPosition().toPoint() - m_drag);
    }
    void mouseReleaseEvent(QMouseEvent *) override { m_drag = QPoint(); }

private:
    QDialog *m_dialog;
    QPoint m_drag;
};
}

ToolLayoutDialog::ToolLayoutDialog(const QVector<ToolLayoutItem> &items,
                                   const QVector<ToolLayoutItem> &defaults,
                                   QWidget *parent)
    : QDialog(parent), m_items(items), m_defaults(defaults)
{
    setWindowTitle(tr("调整工具栏"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_TranslucentBackground, true);
    resize(920, 740);
    setStyleSheet(QStringLiteral(
        "QDialog{background:transparent;}"
        "QWidget#toolLayoutContent,QScrollArea,QScrollArea > QWidget > QWidget{background:transparent;border:none;}"
        "QWidget#toolLayoutCard{background:%1;border:1px solid %4;border-radius:12px;}"
        "QLabel{color:%2;background:transparent;}"
        "QLabel#toolLayoutTitle{font-weight:600;}"
        "QLabel#toolLayoutHint,QLabel#toolLayoutZoneLabel,QLabel#toolLayoutHiddenHint{color:%3;}"
        "QLabel#toolLayoutPreview{background:#000000;border:1px solid %4;border-radius:8px;color:%3;}"
        "QWidget#toolLayoutZone{border:1px dashed %4;border-radius:8px;background:transparent;}"
        "QWidget#toolLayoutZone[drop=\"true\"]{border-color:%7;background:%6;}"
        "QWidget#toolLayoutGroup{background:transparent;border:none;}"
        "QToolButton#toolLayoutTool,QToolButton#toolLayoutHiddenChip{color:%2;background:%5;"
        "border:1px solid %4;border-radius:8px;padding:0px;}"
        "QToolButton#toolLayoutHiddenChip{padding:0px 10px;}"
        "QToolButton#toolLayoutTool:hover,QToolButton#toolLayoutHiddenChip:hover{background:%6;}"
        "QToolButton#toolLayoutTool:pressed,QToolButton#toolLayoutHiddenChip:pressed{background:%4;}"
        "QToolButton#toolLayoutTool:focus,QToolButton#toolLayoutHiddenChip:focus{border:2px solid %7;}"
        "QWidget#toolLayoutHiddenTray{border:1px dashed %4;border-radius:8px;background:transparent;}"
        "QWidget#toolLayoutHiddenTray[drop=\"true\"]{border-color:%7;background:%6;}"
        "QWidget#toolLayoutDropIndicator{background:%7;border:none;}"
        "QToolButton#toolLayoutClose{background:transparent;border:none;border-radius:6px;}"
        "QToolButton#toolLayoutClose:hover{background:%6;}"
        "QToolButton#toolLayoutClose:pressed{background:%4;}"
        "QToolButton#toolLayoutClose:focus{border:2px solid %7;}"
        "QPushButton{color:%3;background:transparent;border:2px solid transparent;border-radius:8px;"
        "padding:0px 12px;}"
        "QPushButton:hover{color:%2;background:%6;}"
        "QPushButton:pressed{color:%2;background:%4;}"
        "QPushButton:focus{border-color:%7;}"
        "QPushButton#toolLayoutSave{color:%9;background:%8;}"
        "QPushButton#toolLayoutSave:hover{background:%2;}"
        "QPushButton#toolLayoutSave:pressed{background:%3;}"
        "QScrollBar:vertical{background:transparent;width:8px;margin:0px;}"
        "QScrollBar:horizontal{background:transparent;height:8px;margin:0px;}"
        "QScrollBar::handle:vertical,QScrollBar::handle:horizontal{background:%3;border-radius:4px;min-height:28px;min-width:28px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0px;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical,"
        "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}")
        .arg(Bar::surface.name(), Bar::ink.name(), Bar::muted.name(), Bar::divider.name(),
             Bar::field.name(), Bar::hover.name(), Bar::focusRing.name(),
             Bar::primaryFill.name(), Bar::primaryInk.name()));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(0);
    auto *card = new QWidget(this);
    card->setObjectName(QStringLiteral("toolLayoutCard"));
    root->addWidget(card);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(20, 16, 20, 16);
    cardLayout->setSpacing(12);

    auto *title = new TitleBar(this, tr("调整工具栏"), card);
    cardLayout->addWidget(title);
    auto *hint = new QLabel(tr("拖动按钮调整位置，或点击按钮选择移动与排序。"), card);
    hint->setObjectName(QStringLiteral("toolLayoutHint"));
    hint->setWordWrap(true);
    cardLayout->addWidget(hint);

    m_scroll = new QScrollArea(card);
    m_scroll->setObjectName(QStringLiteral("toolLayoutScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_content = new QWidget(m_scroll);
    m_content->setObjectName(QStringLiteral("toolLayoutContent"));
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(0, 0, 8, 0);
    contentLayout->setSpacing(16);
    contentLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);

    m_stage = new QWidget(m_content);
    m_stage->setObjectName(QStringLiteral("toolLayoutStage"));
    auto *grid = new QGridLayout(m_stage);
    grid->setSizeConstraint(QLayout::SetMinimumSize);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 56);
    grid->setColumnStretch(1, 1);
    grid->setColumnMinimumWidth(2, 56);
    grid->setRowMinimumHeight(0, 60);
    grid->setRowStretch(1, 1);
    grid->setRowMinimumHeight(2, 64);

    QWidget *top = buildZone(QStringLiteral("top"), tr("上方"));
    QWidget *left = buildZone(QStringLiteral("left"), tr("左侧"));
    QWidget *right = buildZone(QStringLiteral("right"), tr("右侧"));
    QWidget *bottom = buildZone(QStringLiteral("bottom"), tr("下方"));
    right->setToolTip(tr("把按钮拖到这里，右侧变竖栏"));

    m_preview = new QLabel(tr("截图区域"), m_stage);
    m_preview->setObjectName(QStringLiteral("toolLayoutPreview"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFixedSize(620, 310);

    grid->addWidget(top, 0, 1);
    grid->addWidget(left, 1, 0);
    grid->addWidget(m_preview, 1, 1, Qt::AlignCenter);
    grid->addWidget(right, 1, 2);
    grid->addWidget(bottom, 2, 1);
    contentLayout->addWidget(m_stage);

    m_hiddenTray = new QWidget(m_content);
    m_hiddenTray->setObjectName(QStringLiteral("toolLayoutHiddenTray"));
    m_hiddenTray->setAcceptDrops(true);
    m_hiddenTray->installEventFilter(this);
    m_hiddenTray->setMinimumHeight(48);
    auto *hiddenLayout = new QVBoxLayout(m_hiddenTray);
    hiddenLayout->setSizeConstraint(QLayout::SetMinimumSize);
    hiddenLayout->setContentsMargins(12, 10, 12, 10);
    hiddenLayout->setSpacing(8);
    auto *hiddenLabel = new QLabel(tr("下拉中的工具"), m_hiddenTray);
    hiddenLabel->setObjectName(QStringLiteral("toolLayoutHiddenLabel"));
    hiddenLabel->setProperty("lenssnipSide", QStringLiteral("hidden"));
    hiddenLabel->setAcceptDrops(true);
    hiddenLabel->installEventFilter(this);
    m_hiddenPlaceholder = new QLabel(
        tr("拖到这里收起工具；下拉入口始终保留。"), m_hiddenTray);
    m_hiddenPlaceholder->setObjectName(QStringLiteral("toolLayoutHiddenHint"));
    m_hiddenPlaceholder->setWordWrap(true);
    m_hiddenPlaceholder->setProperty("lenssnipSide", QStringLiteral("hidden"));
    m_hiddenPlaceholder->setAcceptDrops(true);
    m_hiddenPlaceholder->installEventFilter(this);
    m_hiddenGroup = new QWidget(m_hiddenTray);
    m_hiddenGroup->setObjectName(QStringLiteral("toolLayoutHiddenGroup"));
    m_hiddenGroup->setProperty("lenssnipSide", QStringLiteral("hidden"));
    m_hiddenGroup->setAcceptDrops(true);
    m_hiddenGroup->installEventFilter(this);
    auto *hiddenGroupLayout = new QGridLayout(m_hiddenGroup);
    hiddenGroupLayout->setSizeConstraint(QLayout::SetMinimumSize);
    hiddenGroupLayout->setContentsMargins(0, 0, 0, 0);
    hiddenGroupLayout->setSpacing(6);
    hiddenLayout->addWidget(hiddenLabel);
    hiddenLayout->addWidget(m_hiddenPlaceholder);
    hiddenLayout->addWidget(m_hiddenGroup);
    contentLayout->addWidget(m_hiddenTray);
    contentLayout->addStretch(1);

    m_scroll->setWidget(m_content);
    m_scroll->viewport()->installEventFilter(this);
    cardLayout->addWidget(m_scroll, 1);
    m_dropIndicator = new QWidget(m_content);
    m_dropIndicator->setObjectName(QStringLiteral("toolLayoutDropIndicator"));
    m_dropIndicator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_dropIndicator->hide();

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *restore = new QPushButton(tr("恢复默认"), card);
    restore->setObjectName(QStringLiteral("toolLayoutRestore"));
    auto *cancel = new QPushButton(tr("取消"), card);
    cancel->setObjectName(QStringLiteral("toolLayoutCancel"));
    auto *save = new QPushButton(tr("保存"), card);
    save->setObjectName(QStringLiteral("toolLayoutSave"));
    save->setDefault(true);
    for (QPushButton *button : {restore, cancel, save}) {
        button->setMinimumHeight(34);
        button->setAutoDefault(false);
    }
    actions->addWidget(restore);
    actions->addStretch();
    actions->addWidget(cancel);
    actions->addWidget(save);
    cardLayout->addLayout(actions);

    connect(restore, &QPushButton::clicked, this, &ToolLayoutDialog::restoreDefaults);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &QDialog::accept);
    rebuild();
    save->setFocus();
}

void ToolLayoutDialog::fitToScreen(const QRect &available)
{
    const int width = qMax(1, qMin(920, available.width() - 24));
    const int height = qMax(1, qMin(740, available.height() - 24));
    resize(width, height);
    move(available.center() - rect().center());
}

QWidget *ToolLayoutDialog::buildZone(const QString &side, const QString &labelText)
{
    auto *zone = new QWidget(m_stage);
    zone->setObjectName(QStringLiteral("toolLayoutZone"));
    zone->setProperty("lenssnipSide", side);
    zone->setAcceptDrops(true);
    zone->installEventFilter(this);
    auto *layout = new QVBoxLayout(zone);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);
    auto *label = new QLabel(labelText, zone);
    label->setObjectName(QStringLiteral("toolLayoutZoneLabel"));
    label->setAlignment(Qt::AlignCenter);
    label->setProperty("lenssnipSide", side);
    label->setAcceptDrops(true);
    label->installEventFilter(this);
    auto *group = new QWidget(zone);
    group->setObjectName(QStringLiteral("toolLayoutGroup"));
    group->setProperty("lenssnipSide", side);
    group->setAcceptDrops(true);
    group->installEventFilter(this);
    if (side == QStringLiteral("left") || side == QStringLiteral("right")) {
        auto *box = new QVBoxLayout(group);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(4);
    } else {
        auto *box = new QHBoxLayout(group);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(4);
    }
    group->hide();
    if (side == QStringLiteral("left") || side == QStringLiteral("right")) {
        layout->addWidget(label, 0, Qt::AlignHCenter | Qt::AlignTop);
        layout->addWidget(group, 0, Qt::AlignHCenter | Qt::AlignTop);
        layout->addStretch(1);
    } else {
        layout->addWidget(label, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(group, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addStretch(1);
    }
    m_zones.insert(side, zone);
    m_groups.insert(side, group);
    m_zoneLabels.insert(side, label);
    return zone;
}

void ToolLayoutDialog::clearLayout(QLayout *layout)
{
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) delete widget;
        delete item;
    }
}

void ToolLayoutDialog::rebuild()
{
    QString focusedId;
    if (QWidget *focused = QApplication::focusWidget())
        if (isAncestorOf(focused)) focusedId = focused->property("lenssnipLayoutId").toString();
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it)
        clearLayout(it.value()->layout());
    clearLayout(m_hiddenGroup->layout());

    for (const ToolLayoutItem &entry : m_items) {
        const QString side = normalizedSide(entry.side);
        const bool hidden = side == QStringLiteral("hidden");
        if (hidden && entry.special) continue;
        QWidget *parent = hidden ? m_hiddenGroup : m_groups.value(side);
        if (!parent) continue;
        auto *button = new LayoutButton(entry.id, parent);
        button->setProperty("lenssnipLayoutId", entry.id);
        button->setProperty("lenssnipSide", side);
        button->setAcceptDrops(true);
        button->installEventFilter(this);
        button->setIcon(barIcon(entry.icon, 20));
        button->setIconSize(QSize(20, 20));
        button->setToolTip(entry.name);
        button->setAccessibleName(entry.name);
        button->setAccessibleDescription(tr("点击选择移动与排序，也可拖动。"));
        button->setCursor(Qt::OpenHandCursor);
        button->setFocusPolicy(Qt::StrongFocus);
        if (hidden) {
            button->setObjectName(QStringLiteral("toolLayoutHiddenChip"));
            button->setText(entry.name);
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setFixedHeight(qMax(34, button->fontMetrics().height() + 12));
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            static_cast<QGridLayout *>(parent->layout())->addWidget(button);
        } else {
            button->setObjectName(QStringLiteral("toolLayoutTool"));
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setFixedSize(34, 34);
            auto *layout = qobject_cast<QBoxLayout *>(parent->layout());
            if (layout) layout->addWidget(button);
        }
        connect(button, &QToolButton::clicked, this, [this, id = entry.id] { showItemMenu(id); });
        button->dragStarted = [this](const QString &id, LayoutButton *source) {
            if (!m_dragId.isEmpty()) return;
            m_dragId = id;
            auto *drag = new QDrag(this);
            auto *mime = new QMimeData;
            mime->setData(kToolMime, id.toUtf8());
            drag->setMimeData(mime);
            drag->setPixmap(source->grab());
            drag->setHotSpot(source->rect().center());
            drag->exec(Qt::MoveAction);
            drag->deleteLater();
            for (QWidget *zone : m_zones) setDropHighlight(zone, false);
            setDropHighlight(m_hiddenTray, false);
            m_dropIndicator->hide();
            const QString dropId = m_dropId;
            const QString dropSide = m_dropSide;
            const int drop = m_dropIndex;
            m_dropId.clear();
            m_dragId.clear();
            // Rebuilding here would delete the source button while its mouseMoveEvent
            // is still on the stack. Defer until the drag event has fully unwound.
            if (!dropId.isEmpty()) {
                QTimer::singleShot(0, this, [this, dropId, dropSide, drop] {
                    moveItem(dropId, dropSide, drop);
                });
            }
        };
    }

    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        QWidget *group = it.value();
        const bool hasItems = group->layout() && group->layout()->count() > 0;
        group->setVisible(hasItems);
        if (QLabel *label = m_zoneLabels.value(it.key())) label->setVisible(!hasItems);
        group->adjustSize();
    }
    const bool hasHidden = m_hiddenGroup->layout() && m_hiddenGroup->layout()->count() > 0;
    m_hiddenGroup->setVisible(hasHidden);
    layoutHiddenItems();
    QWidget *previous = findChild<QToolButton *>(QStringLiteral("toolLayoutClose"));
    for (const QString &side : {QStringLiteral("top"), QStringLiteral("left"),
                               QStringLiteral("right"), QStringLiteral("bottom"), QStringLiteral("hidden")}) {
        QWidget *group = side == QStringLiteral("hidden") ? m_hiddenGroup : m_groups.value(side);
        for (QToolButton *button : group->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (previous) QWidget::setTabOrder(previous, button);
            previous = button;
        }
    }
    if (previous) QWidget::setTabOrder(previous, findChild<QPushButton *>(QStringLiteral("toolLayoutRestore")));
    m_stage->layout()->activate();
    m_hiddenGroup->layout()->activate();
    m_hiddenTray->layout()->activate();
    m_content->layout()->activate();
    m_content->setMinimumSize(m_content->minimumSizeHint());
    if (!focusedId.isEmpty()) focusItem(focusedId);
}

void ToolLayoutDialog::layoutHiddenItems()
{
    auto *grid = static_cast<QGridLayout *>(m_hiddenGroup->layout());
    const auto buttons = m_hiddenGroup->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
    if (buttons.isEmpty()) return;
    int widest = 1;
    for (QToolButton *button : buttons) widest = qMax(widest, button->sizeHint().width());
    const int width = qMax(m_stage->minimumSizeHint().width(), m_scroll->viewport()->width() - 8) - 26;
    const int columns = qMax(1, width / (widest + grid->horizontalSpacing()));
    while (QLayoutItem *item = grid->takeAt(0)) delete item;
    for (int i = 0; i < buttons.size(); ++i)
        grid->addWidget(buttons.at(i), i / columns, i % columns, Qt::AlignLeft);
    grid->activate();
}

void ToolLayoutDialog::focusItem(const QString &id)
{
    for (QToolButton *button : findChildren<QToolButton *>()) {
        if (button->property("lenssnipLayoutId").toString() != id) continue;
        button->setFocus(Qt::OtherFocusReason);
        m_scroll->ensureWidgetVisible(button, 12, 12);
        break;
    }
}

void ToolLayoutDialog::showItemMenu(const QString &id)
{
    ToolLayoutItem item;
    int index = 0, count = 0;
    for (const ToolLayoutItem &entry : m_items) if (entry.id == id) item = entry;
    if (item.id.isEmpty()) return;
    for (const ToolLayoutItem &entry : m_items) {
        if (entry.side != item.side) continue;
        if (entry.id == id) index = count;
        ++count;
    }
    QMenu menu(this);
    menu.setStyleSheet(darkMenuQss);
    menu.addSection(item.name);
    const QStringList sides{QStringLiteral("top"), QStringLiteral("right"), QStringLiteral("bottom"),
                            QStringLiteral("left"), QStringLiteral("hidden")};
    const QStringList names{tr("上方"), tr("右侧"), tr("下方"), tr("左侧"), tr("下拉中的工具")};
    for (int i = 0; i < sides.size(); ++i) {
        QAction *action = menu.addAction(names.at(i));
        action->setData(sides.at(i));
        action->setCheckable(true);
        action->setChecked(item.side == sides.at(i));
        action->setEnabled(!item.special || sides.at(i) != QStringLiteral("hidden"));
        if (!action->isEnabled()) action->setText(tr("下拉入口不能隐藏"));
    }
    menu.addSeparator();
    QAction *before = menu.addAction(tr("向前移动"));
    QAction *after = menu.addAction(tr("向后移动"));
    before->setEnabled(index > 0);
    after->setEnabled(index + 1 < count);
    QWidget *source = nullptr;
    for (QToolButton *button : findChildren<QToolButton *>())
        if (button->property("lenssnipLayoutId").toString() == id) { source = button; break; }
    if (!source) return;
    QAction *selected = menu.exec(source->mapToGlobal(QPoint(0, source->height())));
    if (!selected) return;
    const QString side = selected == before || selected == after ? item.side : selected->data().toString();
    const int target = selected == before ? index - 1 : selected == after ? index + 1
        : side == item.side ? index : m_items.size();
    QTimer::singleShot(0, this, [this, id, side, target] { moveItem(id, side, target); });
}

void ToolLayoutDialog::showDropIndicator(const QString &side, int index)
{
    QWidget *group = side == QStringLiteral("hidden") ? m_hiddenGroup : m_groups.value(side);
    QList<QToolButton *> buttons;
    for (QToolButton *button : group->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly))
        if (button->property("lenssnipLayoutId").toString() != m_dragId) buttons << button;
    if (buttons.isEmpty()) { m_dropIndicator->hide(); return; }
    const bool vertical = side == QStringLiteral("left") || side == QStringLiteral("right");
    const bool atEnd = index >= buttons.size();
    QToolButton *button = buttons.at(atEnd ? buttons.size() - 1 : qMax(0, index));
    const QRect bounds(button->mapTo(m_content, QPoint()), button->size());
    m_dropIndicator->setGeometry(vertical
        ? QRect(bounds.left(), atEnd ? bounds.bottom() + 2 : bounds.top() - 3, bounds.width(), 2)
        : QRect(atEnd ? bounds.right() + 2 : bounds.left() - 3, bounds.top(), 2, bounds.height()));
    m_dropIndicator->show();
    m_dropIndicator->raise();
}

QVector<ToolLayoutItem> ToolLayoutDialog::items() const
{
    return m_items;
}

int ToolLayoutDialog::dropIndex(QWidget *zone, const QString &side, const QPoint &position) const
{
    QWidget *group = side == QStringLiteral("hidden") ? m_hiddenGroup : m_groups.value(side);
    if (!group) return 0;
    const bool vertical = side == QStringLiteral("left") || side == QStringLiteral("right");
    int index = 0;
    const auto buttons = group->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
    for (QToolButton *button : buttons) {
        if (!button->isVisible() || button->property("lenssnipLayoutId").toString() == m_dragId)
            continue;
        const QPoint center = button->mapTo(zone, button->rect().center());
        const QRect bounds(button->mapTo(zone, QPoint()), button->size());
        const bool beyond = side == QStringLiteral("hidden")
            ? position.y() > bounds.bottom() || (position.y() >= bounds.top() && position.x() > center.x())
            : vertical ? position.y() > center.y() : position.x() > center.x();
        if (beyond) ++index;
        else break;
    }
    return index;
}

void ToolLayoutDialog::moveItem(const QString &id, const QString &side, int index)
{
    int source = -1;
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).id == id) { source = i; break; }
    if (source < 0) return;
    const QString targetSide = normalizedSide(side);
    if (targetSide == QStringLiteral("hidden") && m_items.at(source).special) {
        QToolTip::showText(QCursor::pos(), tr("下拉按钮不能隐藏"), this);
        return;
    }
    ToolLayoutItem item = m_items.takeAt(source);
    item.side = targetSide;
    QStringList sameSide;
    for (const ToolLayoutItem &entry : m_items)
        if (normalizedSide(entry.side) == targetSide) sameSide << entry.id;
    index = qBound(0, index, sameSide.size());
    int target = m_items.size();
    if (index < sameSide.size()) {
        const QString before = sameSide.at(index);
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items.at(i).id == before) { target = i; break; }
    } else if (!sameSide.isEmpty()) {
        const QString after = sameSide.last();
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items.at(i).id == after) { target = i + 1; break; }
    }
    m_items.insert(target, item);
    rebuild();
    focusItem(id);
    emitChanged();
}

void ToolLayoutDialog::restoreDefaults()
{
    m_items = m_defaults;
    for (ToolLayoutItem &entry : m_items) entry.side = QStringLiteral("bottom");
    rebuild();
    emitChanged();
}

void ToolLayoutDialog::emitChanged()
{
    emit layoutChanged(m_items);
}

bool ToolLayoutDialog::eventFilter(QObject *obj, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(obj);
    if (!widget) return QDialog::eventFilter(obj, event);
    if (widget == m_scroll->viewport() && event->type() == QEvent::Resize)
        layoutHiddenItems();
    if (event->type() == QEvent::KeyPress && !widget->property("lenssnipLayoutId").toString().isEmpty()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Menu) {
            showItemMenu(widget->property("lenssnipLayoutId").toString());
            return true;
        }
    }
    QString side = widget->property("lenssnipSide").toString();
    if (side.isEmpty() && widget == m_hiddenTray) side = QStringLiteral("hidden");
    for (QWidget *parent = widget->parentWidget();
         side.isEmpty() && parent; parent = parent->parentWidget())
        side = parent->property("lenssnipSide").toString();
    if (side.isEmpty()) return QDialog::eventFilter(obj, event);
    const bool hidden = side == QStringLiteral("hidden");
    QWidget *highlight = hidden ? m_hiddenTray : m_zones.value(side);
    if (!highlight) return QDialog::eventFilter(obj, event);
    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        if (m_dragId.isEmpty() || !drag->mimeData()->hasFormat(kToolMime)) break;
        if (hidden) {
            for (const ToolLayoutItem &item : m_items)
                if (item.id == m_dragId && item.special) { drag->ignore(); return true; }
        }
        drag->acceptProposedAction();
        setDropHighlight(highlight, true);
        const QPoint point = highlight->mapFromGlobal(widget->mapToGlobal(drag->position().toPoint()));
        showDropIndicator(side, dropIndex(highlight, side, point));
        return true;
    }
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (m_dragId.isEmpty()) break;
        drag->acceptProposedAction();
        const QPoint point = highlight->mapFromGlobal(widget->mapToGlobal(drag->position().toPoint()));
        showDropIndicator(side, dropIndex(highlight, side, point));
        return true;
    }
    case QEvent::DragLeave:
        setDropHighlight(highlight, false);
        m_dropIndicator->hide();
        return true;
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        if (m_dragId.isEmpty()) break;
        drop->acceptProposedAction();
        if (hidden && m_dragId == QStringLiteral("more")) {
            QToolTip::showText(QCursor::pos(), tr("下拉按钮不能隐藏"), this);
            setDropHighlight(highlight, false);
            m_dropIndicator->hide();
            return true;
        }
        m_dropId = m_dragId;
        m_dropSide = side;
        const QPoint global = widget->mapToGlobal(drop->position().toPoint());
        m_dropIndex = dropIndex(highlight, side, highlight->mapFromGlobal(global));
        setDropHighlight(highlight, false);
        m_dropIndicator->hide();
        return true;
    }
    default:
        break;
    }
    return QDialog::eventFilter(obj, event);
}
