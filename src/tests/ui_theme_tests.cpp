#include "capture/capture_overlay.h"
#include "capture/tool_layout_dialog.h"
#include "app/controller.h"
#include "shared/dialog_chrome.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QGraphicsTextItem>
#include <QPushButton>
#include "pin/pin_window.h"
#include "longshot/long_capture.h"
#include "text/cards/ocr_dialog.h"
#include "shared/widgets.h"
#include "shared/settings.h"
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QAbstractItemView>
#include <algorithm>
#include <climits>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QEventLoop>
#include <QTimer>
#include <QTranslator>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTextDocument>
#include <cstdio>
#include <cmath>

namespace {
QImage documentImage()
{
    QImage image(900, 560, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    p.setRenderHint(QPainter::TextAntialiasing);
    QFont font = QApplication::font();
    font.setPixelSize(12); p.setFont(font); p.setPen(QColor("#85909f"));
    p.drawText(32, 42, QStringLiteral("PROJECT NOTE / 03"));
    font.setPixelSize(26); font.setWeight(QFont::DemiBold); p.setFont(font); p.setPen(QColor("#202734"));
    p.drawText(32, 88, QStringLiteral("让信息更清晰，让协作更轻松。"));
    font.setPixelSize(15); font.setWeight(QFont::Normal); p.setFont(font); p.setPen(QColor("#586373"));
    const QStringList lines{QStringLiteral("好的工具应当在需要时出现，在完成后安静退场。"),
        QStringLiteral("从屏幕选取内容，标注关键信息，再把结果分享出去。"),
        QStringLiteral("将截图、文字与上下文放在一起，让每一次沟通都清楚。")};
    for (int i = 0; i < lines.size(); ++i) p.drawText(32, 176 + i * 30, lines[i]);
    return image;
}
struct Review {
    QString prefix;
    bool pass = true;
    void check(bool condition, const char *message) {
        std::printf("%s UI: %s\n", condition ? "PASS" : "FAIL", message);
        pass = pass && condition;
    }
    void save(const QPixmap &image, const QString &suffix) {
        if (prefix.isEmpty()) return;
        QDir().mkpath(QFileInfo(prefix).absolutePath());
        check(image.save(prefix + suffix + QStringLiteral(".png")), "save the actual rendered UI preview");
    }
};
}

int CaptureOverlay::testThemeUi(const QString &previewPath)
{
    Review review{previewPath};
    review.check(Bar::accent == QColor("#5e6ad2") && Bar::surface == QColor("#18191d")
                     && Bar::ink == QColor("#f4f5f8") && Bar::divider == QColor("#2b2d33"),
                 "chrome colors match the approved design tokens");
    review.check(Bar::button == 28 && Bar::icon == 20, "compact toolbar hit boxes and icon sizes match the design");
    QImage desktop(1440, 900, QImage::Format_RGB32);
    desktop.fill(QColor("#e6e9ee"));
    { QPainter painter(&desktop); painter.drawImage(QPoint(280, 140), documentImage()); }
    const QRect source(QGuiApplication::primaryScreen()->geometry().topLeft(), desktop.size());
    const bool hadToolDock = LensSnipSettings().contains(QStringLiteral("Capture/toolDock"));
    const QString previousToolDock = LensSnipSettings().value(QStringLiteral("Capture/toolDock")).toString();
    const bool hadToolLayoutBefore = LensSnipSettings().contains(QStringLiteral("Capture/toolLayout"));
    const QStringList previousToolLayoutBefore = LensSnipSettings().value(QStringLiteral("Capture/toolLayout")).toStringList();
    const bool hadToolLayoutV2 = LensSnipSettings().contains(QStringLiteral("Capture/toolLayoutV2"));
    const QStringList previousToolLayoutV2 = LensSnipSettings().value(QStringLiteral("Capture/toolLayoutV2")).toStringList();
    const bool hadToolOrder = LensSnipSettings().contains(QStringLiteral("Capture/toolOrder"));
    const QStringList previousToolOrder = LensSnipSettings().value(QStringLiteral("Capture/toolOrder")).toStringList();
    LensSnipSettings().remove(QStringLiteral("Capture/toolDock"));
    LensSnipSettings().remove(QStringLiteral("Capture/toolLayout"));
    LensSnipSettings().remove(QStringLiteral("Capture/toolLayoutV2"));
    LensSnipSettings().remove(QStringLiteral("Capture/toolOrder"));
    CaptureOverlay overlay({ScreenFrame{source, QRect(nativeFromLogical(source).topLeft(), desktop.size()), desktop}});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    overlay.presetSelection(overlay.localScreenRect(QRect(source.topLeft() + QPoint(280, 180), QSize(900, 480))), ATool::Pen);
    overlay.m_wmSize = 22; overlay.m_wmOpacity = 30; overlay.m_wmGap = 0;
    overlay.m_wmText = QStringLiteral("内部资料 {{yyyy-MM-dd HH:mm}}");
    if (auto *wm = overlay.ensureWatermarkItem()) {
        wm->text = overlay.m_wmText; wm->font = 22; wm->watermarkOpacity = 30;
        wm->watermarkGap = 0; wm->watermarkPosition = 0;
    }
    QApplication::processEvents();
    review.save(overlay.grab(overlay.localScreenRect(source)), QStringLiteral("-capture"));
    overlay.m_mode=Selecting; overlay.m_hover=overlay.m_sel; overlay.update();QApplication::processEvents();
    review.save(overlay.grab(overlay.localScreenRect(source)),QStringLiteral("-02-detection"));
    overlay.m_mode=Adjusting;overlay.m_hover={};
    for(const auto &entry:QList<QPair<ATool,QString>>{{ATool::Rect,QStringLiteral("01-rect")},{ATool::Text,QStringLiteral("03-text")},
           {ATool::Mosaic,QStringLiteral("04-mosaic")},{ATool::Pen,QStringLiteral("19-pen")},
           {ATool::Watermark,QStringLiteral("20-watermark")}}){
        overlay.selectTool(entry.first);overlay.update();QApplication::processEvents();
        if(entry.first==ATool::Text){overlay.beginTextEdit(overlay.m_sel.topLeft()+QPoint(100,100));overlay.m_editor->setPlainText(QStringLiteral("把重点留在画面里。"));}
        review.save(overlay.grab(overlay.localScreenRect(source)),QStringLiteral("-")+entry.second);
        if(overlay.editorActive())overlay.commitTextEdit();
    }
    overlay.selectTool(ATool::Pen);
    const QRect bars = overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry());
    review.save(overlay.grab(bars), QStringLiteral("-capture-bars"));
    overlay.selectTool(ATool::Arrow);
    const QRect arrowBars = overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry());
    review.save(overlay.grab(arrowBars), QStringLiteral("-arrow-bars"));
    auto *arrowCombo = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("arrowStyleCombo"));
    auto *lineCombo = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("arrowLineStyleCombo"));
    review.check(arrowCombo && arrowCombo->count() == 10 && lineCombo && lineCombo->count() == 5,
                 "arrow dropdowns expose ten shapes and five line styles");
    if (arrowCombo) {
        arrowCombo->showPopup();
        QApplication::processEvents();
        review.check(arrowCombo->view()->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff
                         && !arrowCombo->view()->verticalScrollBar()->isVisible(),
                     "a ten-row arrow dropdown hides the scrollbar");
        review.save(arrowCombo->view()->window()->grab(), QStringLiteral("-arrow-popup"));
        arrowCombo->hidePopup();
    }
    if (lineCombo) {
        lineCombo->showPopup();
        QApplication::processEvents();
        review.check(lineCombo->view()->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff
                         && !lineCombo->view()->verticalScrollBar()->isVisible(),
                     "a five-row line dropdown hides the scrollbar");
        review.save(lineCombo->view()->window()->grab(), QStringLiteral("-line-popup"));
        lineCombo->hidePopup();
    }
    overlay.selectTool(ATool::Pen);
    QApplication::processEvents();
    review.check(overlay.m_colorBoard && overlay.m_colorBoard->width() == 187
                     && overlay.m_colorBoard->height() == 28
                     && ColorBoard::CommonCount == 6 && !ColorBoard::swatchRect(6).isValid(),
                 "the strip exposes six square colors without an extra entry");
    overlay.showSizePopup(overlay.m_sizeButton, 1, 50);
    QApplication::processEvents();
    auto *label = overlay.m_sizePopup->findChild<QLabel *>(QStringLiteral("sizePopupLabel"));
    auto *value = overlay.m_sizePopup->findChild<QLabel *>(QStringLiteral("sizePopupValue"));
    review.check(label && value && label->geometry().right() < overlay.m_sizeSlider->x()
                     && overlay.m_sizeSlider->geometry().right() < value->x()
                     && qAbs(label->geometry().center().y() - value->geometry().center().y()) <= 1
                     && value->text().endsWith(QStringLiteral(" px")),
                 "width label, slider and unit value share one aligned row");
    const QRect strip(overlay.m_toolOptions->mapToGlobal(QPoint()), overlay.m_toolOptions->size());
    const QRect actual = overlay.m_sizePopup->frameGeometry();
    const QScreen *popupScreen = QGuiApplication::screenAt(actual.center());
    const QRect available = (popupScreen ? popupScreen : QGuiApplication::primaryScreen())->availableGeometry();
    const bool canAlign = strip.left() >= available.left() && strip.left() + actual.width() <= available.right() + 1;
    review.check(available.contains(actual) && actual.right() >= strip.left() && actual.left() <= strip.right()
                     && (canAlign ? qAbs(actual.left() - strip.left()) <= 1
                                  : actual.left() == available.left() || actual.right() == available.right()),
                 "the width popup is left-aligned when space permits and stays inside its actual screen");
    review.save(overlay.m_sizePopup->grab(), QStringLiteral("-width"));
    overlay.m_sizePopup->close();
    QApplication::processEvents();
    overlay.updateTip();
    overlay.showCustomSizePopup();
    QApplication::processEvents();
    review.check(!overlay.m_customSizePopup->findChild<QLabel *>(QStringLiteral("customHint"))
                     && overlay.m_customX && overlay.m_customY && overlay.m_customWidth
                     && overlay.m_customHeight && overlay.m_presetCombo,
                 "custom size keeps the position, size and preset controls");
    review.check(overlay.m_presetCombo && overlay.m_presetCombo->currentText().isEmpty(),
                 "the preset combo has no placeholder item");
    review.check(overlay.m_sizeBar && overlay.m_sizeBar->isVisible()
                     && overlay.m_sizeUnitButton && overlay.m_sizeUnitButton->isVisible()
                     && overlay.m_sizeCornerButton && overlay.m_sizeCornerButton->isVisible()
                     && overlay.m_sizeRadiusSpin
                     && overlay.m_sizeRadiusSpin->isVisible() == overlay.m_roundEnabled
                     && (overlay.m_sizeRadiusSpin->alignment() & Qt::AlignHCenter)
                     && overlay.m_sizeLockButton && overlay.m_sizeLockButton->isVisible()
                     && overlay.m_sizeUnitButton->x() > overlay.m_sizeTip->x()
                     && overlay.m_sizeLockButton->x() > overlay.m_sizeUnitButton->x(),
                 "size, unit, corner/radius and aspect lock share the selection strip");
    for (QWidget *control : QList<QWidget *>{overlay.m_customX, overlay.m_customY,
             overlay.m_customWidth, overlay.m_customHeight, overlay.m_presetCombo}) {
        const QRect bounds(control->mapTo(overlay.m_customSizePopup, QPoint()), control->size());
        QWidget *hit = overlay.m_customSizePopup->childAt(bounds.center());
        review.check(control->isVisible() && control->height() >= 24
                         && overlay.m_customSizePopup->rect().contains(bounds)
                         && (hit == control || control->isAncestorOf(hit)),
                     "custom-size controls are visible, laid out and receive input");
    }
    review.save(overlay.m_customSizePopup->grab(), QStringLiteral("-custom-size"));
    review.save(overlay.m_sizeBar->grab(), QStringLiteral("-size-bar"));
    overlay.m_customSizePopup->hide();
    overlay.m_roundEnabled=true;overlay.m_roundRadius=24;overlay.applyRoundSettings(false);QApplication::processEvents();
    review.save(overlay.grab(overlay.localScreenRect(source)),QStringLiteral("-06-round"));
    overlay.m_desktopPreview=true;overlay.update();QApplication::processEvents();
    review.save(overlay.grab(overlay.localScreenRect(source)),QStringLiteral("-07-refresh"));
    overlay.m_desktopPreview=false;
    PixelInspector inspector(nullptr);inspector.setSample({source,QRect(QPoint(),desktop.size()),desktop},QPoint(300,200));inspector.show();QApplication::processEvents();
    review.save(inspector.grab(),QStringLiteral("-09-inspector"));inspector.hide();
    overlay.showColorPalette();
    QApplication::processEvents();
    review.save(overlay.m_colorDialog->grab(), QStringLiteral("-color-dialog"));
    auto *hex=overlay.m_colorDialog->findChild<QLineEdit *>(QStringLiteral("colorHex"));
    auto *alpha=overlay.m_colorDialog->findChild<QSpinBox *>(QStringLiteral("colorAlpha"));
    auto *opacity=overlay.m_colorDialog->findChild<QSlider *>(QStringLiteral("colorOpacity"));
    auto *hueField=overlay.m_colorDialog->findChild<QSpinBox *>(QStringLiteral("colorH"));
    auto *pick=overlay.m_colorDialog->findChild<QPushButton *>(QStringLiteral("colorPickScreen"));
    auto *custom=overlay.m_colorDialog->findChild<QToolButton *>(QStringLiteral("colorCustom0"));
    auto *colorScroll=overlay.m_colorDialog->findChild<QScrollArea *>(QStringLiteral("colorScroll"));
    const QRect alphaVisible=colorScroll&&alpha
        ? QRect(alpha->mapTo(colorScroll->viewport(),QPoint()),alpha->size()) : QRect();
    review.check(hex&&alpha&&opacity&&hueField&&pick&&custom,
                 "the color surface exposes hex, opacity, HSV, screen picking and custom colors");
    review.check(colorScroll&&colorScroll->viewport()->rect().contains(alphaVisible),
                 "color fields fit without scrolling at this DPI");
    hex->setText(QStringLiteral("#123ABC"));QMetaObject::invokeMethod(hex,"editingFinished");
    review.check(overlay.m_colorDialog->currentColor().name()==QStringLiteral("#123abc"),"hex input updates the real current color");
    const QColor beforeInvalid=overlay.m_colorDialog->currentColor();hex->setText(QStringLiteral("invalid"));QMetaObject::invokeMethod(hex,"editingFinished");
    review.check(overlay.m_colorDialog->currentColor()==beforeInvalid,"invalid hex cannot overwrite the selected color");
    alpha->setValue(80);review.check(overlay.m_colorDialog->currentColor().alpha()==80&&opacity->value()==80,"alpha input is retained in the QColorDialog contract and moves the slider");
    bool pickerOpened=false,pickerEscaped=false;
    QTimer fallback;fallback.setSingleShot(true);fallback.setInterval(3000);
    QObject::connect(&fallback,&QTimer::timeout,[]{if(QWidget *modal=QApplication::activeModalWidget())modal->close();});
    fallback.start();
    QTimer::singleShot(150,[&]{
        QWidget *modal=QApplication::activeModalWidget();
        if(!modal||modal==overlay.m_colorDialog)return;
        pickerOpened=true;
        QKeyEvent esc(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        QApplication::sendEvent(modal,&esc);
        pickerEscaped=true;
    });
    pick->click();
    fallback.stop();
    review.check(pickerOpened&&pickerEscaped&&overlay.m_colorDialog&&overlay.m_colorDialog->isVisible(),
                 "the screen sampler opens over the frozen capture and Esc returns to the palette");
    overlay.m_colorDialog->reject();

    review.check(overlay.m_tools && overlay.m_tools->isVisible()
                     && overlay.toolSide(QStringLiteral("pen")) == QStringLiteral("bottom")
                     && overlay.toolSide(QStringLiteral("more")) == QStringLiteral("bottom")
                     && overlay.m_toolsTop && !overlay.m_toolsTop->isVisible()
                     && overlay.m_toolsLeft && !overlay.m_toolsLeft->isVisible()
                     && overlay.m_toolsRight && !overlay.m_toolsRight->isVisible(),
                 "the fresh layout keeps every visible tool on the bottom bar");
    review.check(overlay.m_moreButton && overlay.m_moreButton->isVisible()
                     && overlay.m_moreButton->parentWidget() == overlay.m_tools
                     && overlay.m_moreButton->width() < Bar::button,
                 "the dropdown entry is a compact button in the visible bar");

    overlay.m_toolSide[QStringLiteral("refresh")] = QStringLiteral("hidden");
    overlay.relayoutToolButtons();
    overlay.placeToolBars();
    overlay.rebuildMorePanel();
    const QList<QToolButton *> moreButtons = overlay.m_morePanel->findChildren<QToolButton *>();
    const bool hasRefresh = std::any_of(moreButtons.cbegin(), moreButtons.cend(), [](QToolButton *b) {
        return b->toolTip() == QStringLiteral("刷新");
    });
    const bool hasAdjust = std::any_of(moreButtons.cbegin(), moreButtons.cend(), [](QToolButton *b) {
        return b->objectName() == QStringLiteral("toolMoreAdjust") && b->text().isEmpty();
    });
    review.check(hasRefresh && hasAdjust,
                 "the dropdown lists hidden tools and the adjustment action");
    overlay.m_moreButton->click(); QApplication::processEvents();
    review.check(overlay.m_morePanel->isVisible(),
                 "the dropdown entry expands the hidden tools");
    review.save(overlay.m_morePanel->grab(), QStringLiteral("-toolbar-more"));
    review.save(overlay.grab(overlay.localScreenRect(source)), QStringLiteral("-toolbar-more-open"));
    overlay.m_moreButton->click(); QApplication::processEvents();
    review.check(!overlay.m_morePanel->isVisible(),
                 "the dropdown entry collapses the hidden tools");

    overlay.saveToolLayout();
    const QStringList savedLayout = LensSnipSettings().value(QStringLiteral("Capture/toolLayoutV2")).toStringList();
    review.check(savedLayout.contains(QStringLiteral("refresh=hidden")),
                 "the per-button side and order persist in the new layout key");
    overlay.m_toolSide[QStringLiteral("refresh")] = QStringLiteral("bottom");
    overlay.relayoutToolButtons();
    overlay.placeToolBars();
    review.check(overlay.m_refreshButton->isVisible(),
                 "a hidden tool can be returned to a visible side");

    overlay.m_toolSide[QStringLiteral("pen")] = QStringLiteral("right");
    overlay.m_toolSide[QStringLiteral("arrow")] = QStringLiteral("top");
    overlay.m_toolSide[QStringLiteral("ocr")] = QStringLiteral("left");
    overlay.relayoutToolButtons();
    overlay.placeToolBars();
    QApplication::processEvents();
    review.check(overlay.m_toolsRight->isVisible()
                     && overlay.m_toolsRight->height() > overlay.m_toolsRight->width()
                     && overlay.m_toolsRight->geometry().left() > overlay.m_sel.right()
                     && overlay.toolSide(QStringLiteral("pen")) == QStringLiteral("right"),
                 "a button assigned to the right side creates a vertical bar");
    review.check(overlay.m_toolsTop->isVisible()
                     && overlay.m_toolsTop->width() > overlay.m_toolsTop->height()
                     && overlay.m_toolsTop->geometry().bottom() < overlay.m_sel.top()
                     && overlay.toolSide(QStringLiteral("arrow")) == QStringLiteral("top"),
                 "a button assigned to the top creates a horizontal bar above the selection");
    review.check(overlay.m_toolsLeft->isVisible()
                     && overlay.m_toolsLeft->height() > overlay.m_toolsLeft->width()
                     && overlay.m_toolsLeft->geometry().right() < overlay.m_sel.left()
                     && overlay.toolSide(QStringLiteral("ocr")) == QStringLiteral("left"),
                 "a button assigned to the left creates a vertical bar");
    review.check(overlay.m_tools->isVisible() && overlay.m_tools->layout()->count() > 0,
                 "the remaining buttons stay on the bottom bar while side bars are visible");
    review.check(overlay.m_toolOptions->isVisible(),
                 "the active pen keeps its options strip visible");
    const QRect penBar = overlay.m_toolsRight->geometry();
    const QRect penOptions = overlay.m_toolOptions->geometry();
    review.check(!penOptions.intersects(penBar)
                     && (penOptions.left() >= penBar.right() + 1
                         || penOptions.right() <= penBar.left()),
                 "the options strip follows the active pen side");
    review.save(overlay.grab(overlay.localScreenRect(source)), QStringLiteral("-toolbar-sides"));
    overlay.selectTool(ATool::Arrow);
    QApplication::processEvents();
    review.check(overlay.m_toolOptions->isVisible()
                     && overlay.m_toolsTop->isVisible()
                     && (overlay.m_toolOptions->geometry().bottom() < overlay.m_toolsTop->geometry().top()
                         || overlay.m_toolOptions->geometry().top() > overlay.m_toolsTop->geometry().bottom()),
                 "the options strip follows the active top tool");
    overlay.selectTool(ATool::Pen);
    QApplication::processEvents();

    QPixmap liveCard;
    QPoint liveCardCenter;
    bool cardOverlapsToolbar = false;
    const auto beforeCancel = overlay.currentToolLayout();
    const QVariant settingsBeforeCancel = LensSnipSettings().value(QStringLiteral("Capture/toolLayoutV2"));
    QTimer::singleShot(100, [&] {
        auto *dialog = qobject_cast<ToolLayoutDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        liveCard = dialog->grab();
        review.save(QPixmap::fromImage(grabNative(nativeFromLogical(dialog->geometry()))),
                    QStringLiteral("-tool-layout-desktop"));
        liveCardCenter = dialog->mapToGlobal(dialog->rect().center());
        for (QWidget *bar : {overlay.m_tools, overlay.m_toolsTop,
                             overlay.m_toolsLeft, overlay.m_toolsRight, overlay.m_toolOptions})
            if (bar && bar->isVisible() && bar->geometry().intersects(overlay.m_layoutCardRect))
                cardOverlapsToolbar = true;
        dialog->moveItem(QStringLiteral("pen"), QStringLiteral("left"), 0);
        review.check(overlay.toolSide(QStringLiteral("pen")) == QStringLiteral("left"),
                     "editing the card updates the actual toolbar preview");
        dialog->reject();
    });
    overlay.showToolLayoutDialog();
    bool cancelRestored = true;
    for (const ToolLayoutItem &item : beforeCancel)
        cancelRestored = cancelRestored && overlay.toolSide(item.id) == item.side;
    review.check(cancelRestored && LensSnipSettings().value(QStringLiteral("Capture/toolLayoutV2")) == settingsBeforeCancel,
                 "cancel restores the live toolbar without writing layout settings");
    bool barsRestored = true;
    for (QWidget *bar : {overlay.m_tools, overlay.m_toolsTop, overlay.m_toolsLeft, overlay.m_toolsRight})
        if (bar->layout()->count() > 0) barsRestored = barsRestored && bar->isVisible();
    review.check(barsRestored && overlay.m_toolOptions->isVisible(),
                 "closing a space-constrained editor restores preview toolbars and active options");
    const QRect liveAvailable = QGuiApplication::primaryScreen()->availableGeometry();
    review.check(!liveCard.isNull()
                     && qAbs(liveCardCenter.x() - liveAvailable.center().x()) <= 2
                     && qAbs(liveCardCenter.y() - liveAvailable.center().y()) <= 2
                     && !cardOverlapsToolbar,
                 "the live adjustment card is centered and every visible bar avoids it");
    review.save(liveCard, QStringLiteral("-tool-layout-live"));

    QVector<ToolLayoutItem> dialogItems{
        {QStringLiteral("pen"), QStringLiteral("画笔"), Icon::Pen, QStringLiteral("bottom"), false},
        {QStringLiteral("refresh"), QStringLiteral("刷新"), Icon::Refresh, QStringLiteral("hidden"), false},
        {QStringLiteral("ocr"), QStringLiteral("文字识别"), Icon::Ocr, QStringLiteral("left"), false},
        {QStringLiteral("more"), QStringLiteral("下拉"), Icon::More, QStringLiteral("bottom"), true}};
    QVector<ToolLayoutItem> defaultItems;
    for (const ToolLayoutItem &item : dialogItems) {
        ToolLayoutItem entry = item;
        entry.side = QStringLiteral("bottom");
        defaultItems.append(entry);
    }
    const QRect screenAvailable = QGuiApplication::primaryScreen()->availableGeometry();
    ToolLayoutDialog layoutDialog(dialogItems, defaultItems);
    review.check(layoutDialog.findChild<QLabel *>(QStringLiteral("toolLayoutHint"))
                     && layoutDialog.findChildren<QWidget *>(QStringLiteral("toolLayoutZone")).size() == 4
                     && layoutDialog.findChildren<QToolButton *>(QStringLiteral("toolLayoutHiddenChip")).size() == 1
                     && layoutDialog.findChildren<QComboBox *>().isEmpty()
                     && layoutDialog.items().size() == 4,
                 "the adjustment card exposes four spatial zones, hidden chips and no legacy combo boxes");
    layoutDialog.fitToScreen(screenAvailable);
    layoutDialog.show(); QApplication::processEvents();
    review.check(qAbs(layoutDialog.geometry().center().x() - screenAvailable.center().x()) <= 2
                     && qAbs(layoutDialog.geometry().center().y() - screenAvailable.center().y()) <= 2,
                 "the adjustment card is centered on the capture screen");
    review.save(layoutDialog.grab(), QStringLiteral("-tool-layout-dialog"));
    const auto controlVisible = [](QWidget &dialog, QWidget *control) {
        const QRect bounds(control->mapTo(&dialog, QPoint()), control->size());
        return control->isVisible() && dialog.rect().contains(bounds)
            && dialog.childAt(bounds.center()) == control;
    };
    auto *closeLayout = layoutDialog.findChild<QToolButton *>(QStringLiteral("toolLayoutClose"));
    auto *saveLayout = layoutDialog.findChild<QPushButton *>(QStringLiteral("toolLayoutSave"));
    auto *cancelLayout = layoutDialog.findChild<QPushButton *>(QStringLiteral("toolLayoutCancel"));
    for (const QSize &size : {QSize(640, 480), QSize(320, 480)}) {
        layoutDialog.fitToScreen(QRect(screenAvailable.topLeft(), size));
        QApplication::processEvents();
        layoutDialog.m_scroll->verticalScrollBar()->setValue(layoutDialog.m_scroll->verticalScrollBar()->maximum());
        layoutDialog.m_scroll->horizontalScrollBar()->setValue(layoutDialog.m_scroll->horizontalScrollBar()->maximum());
        review.check(layoutDialog.width() <= size.width() - 24 && layoutDialog.height() <= size.height() - 24
                         && controlVisible(layoutDialog, closeLayout) && controlVisible(layoutDialog, saveLayout)
                         && controlVisible(layoutDialog, cancelLayout),
                     "narrow and short screens keep close, cancel and save visible while the editor scrolls");
        review.save(layoutDialog.grab(), QStringLiteral("-tool-layout-%1").arg(size.width()));
    }
    layoutDialog.fitToScreen(screenAvailable);
    layoutDialog.m_scroll->verticalScrollBar()->setValue(0);
    layoutDialog.m_scroll->horizontalScrollBar()->setValue(0);
    QApplication::processEvents();
    int layoutSignals = 0;
    QObject::connect(&layoutDialog, &ToolLayoutDialog::layoutChanged,
                     &layoutDialog,
                     [&layoutSignals](const QVector<ToolLayoutItem> &) { ++layoutSignals; });
    QPushButton *restore = nullptr;
    for (QPushButton *button : layoutDialog.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("恢复默认")) restore = button;
    review.check(restore != nullptr, "the card exposes restore defaults");
    if (restore) {
        restore->click();
        QApplication::processEvents();
        bool allBottom = true;
        for (const ToolLayoutItem &item : layoutDialog.items())
            if (item.side != QStringLiteral("bottom")) allBottom = false;
        review.check(allBottom, "restore defaults returns every button to the bottom bar");
    }
    const auto sideOf = [&layoutDialog](const QString &id) {
        for (const ToolLayoutItem &item : layoutDialog.items())
            if (item.id == id) return item.side;
        return QString();
    };
    layoutDialog.moveItem(QStringLiteral("pen"), QStringLiteral("right"), 0);
    review.check(sideOf(QStringLiteral("pen")) == QStringLiteral("right"),
                 "moving a button to a side updates the layout model");
    layoutDialog.moveItem(QStringLiteral("refresh"), QStringLiteral("hidden"), 0);
    review.check(sideOf(QStringLiteral("refresh")) == QStringLiteral("hidden"),
                  "moving a button to the hidden tray updates the dropdown contents");
    const auto layoutButton = [&layoutDialog](const QString &id) -> QToolButton * {
        for (QToolButton *button : layoutDialog.findChildren<QToolButton *>())
            if (button->property("lenssnipLayoutId").toString() == id) return button;
        return nullptr;
    };
    bool menuMoved = false;
    QTimer::singleShot(0, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (QAction *action : menu->actions()) {
            if (action->data().toString() != QStringLiteral("top")) continue;
            menuMoved = true;
            menu->setActiveAction(action);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter);
            return;
        }
        menu->close();
    });
    QKeyEvent openMenu(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(layoutButton(QStringLiteral("pen")), &openMenu);
    QApplication::processEvents();
    QApplication::processEvents();
    review.check(menuMoved && sideOf(QStringLiteral("pen")) == QStringLiteral("top")
                     && QApplication::focusWidget() == layoutButton(QStringLiteral("pen"))
                     && layoutDialog.result() != QDialog::Accepted,
                 "Enter opens the move menu instead of saving and restores focus to the moved tool");

    layoutDialog.moveItem(QStringLiteral("ocr"), QStringLiteral("bottom"), 0);
    layoutDialog.moveItem(QStringLiteral("pen"), QStringLiteral("bottom"), 0);
    QApplication::processEvents();
    bool menuReordered = false;
    QTimer::singleShot(0, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (QAction *action : menu->actions()) if (action->text() == QStringLiteral("向后移动")) {
            menuReordered = action->isEnabled();
            menu->setActiveAction(action);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter);
            return;
        }
        menu->close();
    });
    layoutButton(QStringLiteral("pen"))->click();
    QApplication::processEvents();
    QApplication::processEvents();
    QStringList bottomOrder;
    for (const ToolLayoutItem &item : layoutDialog.items())
        if (item.side == QStringLiteral("bottom")) bottomOrder << item.id;
    review.check(menuReordered && bottomOrder.indexOf(QStringLiteral("pen")) == 1,
                 "the pointer menu reorders tools without dragging");

    layoutDialog.m_dragId = QStringLiteral("pen");
    QMimeData toolMime;
    toolMime.setData("application/x-lenssnip-tool", "pen");
    auto *targetButton = layoutButton(QStringLiteral("more"));
    QDragEnterEvent dragEnter(targetButton->rect().center(), Qt::MoveAction, &toolMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(targetButton, &dragEnter);
    QDragMoveEvent dragMove(targetButton->rect().center(), Qt::MoveAction, &toolMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(targetButton, &dragMove);
    review.check(dragEnter.isAccepted() && layoutDialog.m_dropIndicator->isVisible()
                     && layoutDialog.m_dropIndicator->width() == 2,
                 "dragging over a child button shows the exact insertion marker");
    review.save(layoutDialog.grab(), QStringLiteral("-tool-layout-drop"));
    QDropEvent drop(targetButton->rect().center(), Qt::MoveAction, &toolMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(targetButton, &drop);
    review.check(layoutDialog.m_dropId == QStringLiteral("pen")
                     && layoutDialog.m_dropSide == QStringLiteral("bottom")
                     && !layoutDialog.m_dropIndicator->isVisible(),
                 "a child-button drop records the owning side and clears its marker");
    layoutDialog.m_dragId = QStringLiteral("more");
    toolMime.setData("application/x-lenssnip-tool", "more");
    QDragEnterEvent denied(QPoint(12, 12), Qt::MoveAction, &toolMime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(layoutDialog.m_hiddenTray, &denied);
    review.check(!denied.isAccepted(), "the hidden tray rejects the dropdown before showing a valid drop target");
    layoutDialog.m_dragId.clear();
    layoutDialog.m_dropId.clear();
    layoutDialog.moveItem(QStringLiteral("pen"), QStringLiteral("right"), 0);
    layoutDialog.moveItem(QStringLiteral("ocr"), QStringLiteral("right"), 0);
    QApplication::processEvents();
    layoutDialog.m_dragId = QStringLiteral("refresh");
    layoutDialog.showDropIndicator(QStringLiteral("right"), 1);
    auto *verticalGroup = layoutDialog.m_groups.value(QStringLiteral("right"));
    const auto verticalButtons = verticalGroup->findChildren<QToolButton *>();
    const QPoint secondCenter = verticalButtons.at(1)->mapTo(verticalGroup, verticalButtons.at(1)->rect().center());
    review.check(layoutDialog.m_dropIndicator->height() == 2
                     && layoutDialog.dropIndex(verticalGroup, QStringLiteral("right"), secondCenter - QPoint(0, 2)) == 1,
                 "vertical tool groups use a horizontal marker and the correct insertion index");
    layoutDialog.m_dragId.clear();
    layoutDialog.m_dropIndicator->hide();
    layoutDialog.moveItem(QStringLiteral("more"), QStringLiteral("hidden"), 0);
    review.check(sideOf(QStringLiteral("more")) == QStringLiteral("bottom")
                     && layoutSignals >= 3,
                 "the dropdown entry cannot be hidden and every move emits the live layout");
    layoutDialog.hide();

    QVector<ToolLayoutItem> completeItems = overlay.currentToolLayout();
    for (ToolLayoutItem &item : completeItems) item.side = QStringLiteral("bottom");
    ToolLayoutDialog completeDialog(completeItems, completeItems);
    completeDialog.fitToScreen(QRect(screenAvailable.topLeft(), QSize(1280, 900)));
    completeDialog.show(); QApplication::processEvents();
    review.check(completeDialog.m_scroll->horizontalScrollBar()->maximum() == 0
                     && completeDialog.m_scroll->verticalScrollBar()->maximum() == 0,
                 "all default tools fit the normal card without scrolling");
    review.save(completeDialog.grab(), QStringLiteral("-tool-layout-default"));
    for (const ToolLayoutItem &item : completeItems)
        if (!item.special) completeDialog.moveItem(item.id, QStringLiteral("hidden"), completeItems.size());
    QApplication::processEvents();
    bool chipsFit = true;
    bool stageFits = !completeDialog.m_stage->geometry().intersects(completeDialog.m_hiddenTray->geometry());
    for (QWidget *zone : completeDialog.m_zones)
        stageFits = stageFits && completeDialog.m_stage->rect().contains(zone->geometry())
            && !zone->geometry().intersects(completeDialog.m_preview->geometry());
    review.check(stageFits && completeDialog.m_stage->rect().contains(completeDialog.m_preview->geometry()),
                 "growing the hidden tray preserves preview and zone bounds without overlap");
    int firstY = -1, lastY = -1;
    for (QToolButton *button : completeDialog.m_hiddenGroup->findChildren<QToolButton *>()) {
        chipsFit = chipsFit && completeDialog.m_hiddenGroup->rect().contains(button->geometry());
        if (firstY < 0) firstY = button->y();
        lastY = button->y();
    }
    review.check(chipsFit && lastY > firstY && completeDialog.m_scroll->horizontalScrollBar()->maximum() == 0,
                 "many hidden tools wrap into readable rows instead of widening the dialog");
    const auto chips = completeDialog.m_hiddenGroup->findChildren<QToolButton *>();
    completeDialog.m_dragId = QStringLiteral("more");
    bool wrappedIndex = true;
    for (int i = 0; i < chips.size(); ++i) {
        const QPoint position = chips.at(i)->geometry().center() - QPoint(2, 0);
        wrappedIndex = wrappedIndex
            && completeDialog.dropIndex(completeDialog.m_hiddenGroup, QStringLiteral("hidden"), position) == i;
    }
    completeDialog.m_dragId.clear();
    review.check(wrappedIndex, "wrapped hidden rows preserve the insertion order at each chip");
    completeDialog.m_scroll->verticalScrollBar()->setValue(completeDialog.m_scroll->verticalScrollBar()->maximum());
    review.save(completeDialog.grab(), QStringLiteral("-tool-layout-hidden"));
    completeDialog.hide();

    const QFont normalFont = QApplication::font();
    QFont largeFont = normalFont;
    largeFont.setPointSizeF(largeFont.pointSizeF() * 1.5);
    QApplication::setFont(largeFont);
    {
        ToolLayoutDialog largeDialog(dialogItems, defaultItems);
        largeDialog.fitToScreen(screenAvailable);
        largeDialog.show(); QApplication::processEvents();
        bool textFits = true;
        for (QPushButton *button : largeDialog.findChildren<QPushButton *>())
            textFits = textFits && button->width() >= button->fontMetrics().horizontalAdvance(button->text()) + 24
                && button->height() >= button->fontMetrics().height() + 4;
        review.check(textFits, "larger system text keeps action labels legible without shrinking the font");
        review.save(largeDialog.grab(), QStringLiteral("-tool-layout-large-font"));
        largeDialog.hide();
    }
    QApplication::setFont(normalFont);
    {
        QTranslator translator;
        review.check(translator.load(QStringLiteral("language/lenssnip_en_US.qm"), QCoreApplication::applicationDirPath()),
                     "load the packaged English translation for the adjustment card");
        QApplication::installTranslator(&translator);
        QVector<ToolLayoutItem> englishItems = dialogItems;
        for (ToolLayoutItem &item : englishItems) item.name = item.id == QStringLiteral("ocr")
            ? QStringLiteral("Text recognition") : QStringLiteral("Refresh");
        ToolLayoutDialog englishDialog(englishItems, englishItems);
        englishDialog.fitToScreen(screenAvailable);
        englishDialog.show(); QApplication::processEvents();
        review.check(englishDialog.findChild<QLabel *>(QStringLiteral("toolLayoutHiddenLabel"))->text()
                         == QStringLiteral("Tools in dropdown")
                         && englishDialog.findChild<QPushButton *>(QStringLiteral("toolLayoutSave"))->text() == QStringLiteral("Save"),
                     "the revised adjustment labels use the shipped English translations");
        review.save(englishDialog.grab(), QStringLiteral("-tool-layout-en"));
        englishDialog.hide();
        QApplication::removeTranslator(&translator);
    }

    const auto luminance = [](const QColor &color) {
        const auto channel = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
    };
    review.check((luminance(Bar::muted) + 0.05) / (luminance(Bar::hover) + 0.05) >= 4.5
                     && (luminance(Bar::primaryFill) + 0.05) / (luminance(Bar::primaryInk) + 0.05) >= 4.5
                     && (luminance(Bar::focusRing) + 0.05) / (luminance(Bar::surface) + 0.05) >= 3,
                 "supporting text, primary action and keyboard focus meet their contrast targets");

    overlay.m_toolSide[QStringLiteral("pen")] = QStringLiteral("right");
    overlay.m_toolSide[QStringLiteral("refresh")] = QStringLiteral("hidden");
    overlay.saveToolLayout();
    const QStringList finalLayout = LensSnipSettings().value(QStringLiteral("Capture/toolLayoutV2")).toStringList();
    review.check(finalLayout.contains(QStringLiteral("pen=right"))
                     && finalLayout.contains(QStringLiteral("refresh=hidden"))
                     && finalLayout.contains(QStringLiteral("more=bottom")),
                 "the new layout keeps per-button sides including the dropdown entry");

    overlay.close();
    if (hadToolDock) LensSnipSettings().setValue(QStringLiteral("Capture/toolDock"), previousToolDock);
    else LensSnipSettings().remove(QStringLiteral("Capture/toolDock"));
    if (hadToolLayoutBefore) LensSnipSettings().setValue(QStringLiteral("Capture/toolLayout"), previousToolLayoutBefore);
    else LensSnipSettings().remove(QStringLiteral("Capture/toolLayout"));
    if (hadToolLayoutV2) LensSnipSettings().setValue(QStringLiteral("Capture/toolLayoutV2"), previousToolLayoutV2);
    else LensSnipSettings().remove(QStringLiteral("Capture/toolLayoutV2"));
    if (hadToolOrder) LensSnipSettings().setValue(QStringLiteral("Capture/toolOrder"), previousToolOrder);
    else LensSnipSettings().remove(QStringLiteral("Capture/toolOrder"));

    const QImage image = documentImage();
    {
        OcrDialog loading(image);loading.m_closed=true;loading.show();QApplication::processEvents();
        review.check(loading.size()==QSize(640,160)&&!loading.m_header->isVisible(),"loading card has one surface and no redundant title row");
        review.save(loading.grab(),QStringLiteral("-20-loading"));loading.close();
    }
    const QString original = QStringLiteral("好的工具应当在需要时出现，\n在完成后安静退场。\n\n从屏幕选取内容，标注关键信息，\n再把结果分享出去。");
    const QString translatedText = QStringLiteral("A good tool appears when you need it,\nand quietly steps aside when you are done.\n\nCapture what matters on your screen,\nannotate the key points, and share the result.\n\nKeep images, words, and context together\nso that every conversation stays clear.");
    for (bool translated : {false, true}) {
        OcrDialog card(image, {}, translated);
        card.m_closed = true;                       // Drain the queued startup without starting OCR or a network request.
        card.show(); QApplication::processEvents();
        card.m_closed = false;
        card.showLoading(false);
        card.m_raw = original;
        card.m_text->setPlainText(translated ? translatedText : original);
        card.m_status->clear();
        card.m_busy->hide();
        card.m_text->setFocus();
        card.m_text->setReadOnly(false);
        card.m_retry->setEnabled(true);
        if (translated) {
            card.populateLanguages(QStringLiteral("zh"), QStringLiteral("en"));
            card.m_source->setDetected(QStringLiteral("简体中文"));
            card.m_provider->setDetected(QStringLiteral("Edge"));
            card.m_sourcePane->setPlainText(original);
            card.m_sourcePane->setReadOnly(false);
            card.m_sourcePane->document()->setModified(false);
            card.m_translatedSource = original;
            card.m_mask->hide(); card.setDisplay(0);
        }
        const QList<int> widths = translated ? QList<int>{1500, 1100, 680} : QList<int>{1100, 660};
        for (int width : widths) {
            card.resize(width, 650); card.applyLayout(true); QApplication::processEvents();
            review.check(card.m_text->isVisible() && card.m_content->isVisible(), "result content survives responsive layout changes");
            const QImage surface = card.grab().toImage();
            const qreal dpr = surface.devicePixelRatio();
            review.check(surface.pixelColor(qRound(5 * dpr), qRound(card.height() / 2 * dpr)) == QColor("#18191d")
                             && surface.pixelColor(0, 0).alpha() == 0,
                         "the rounded card keeps an opaque surface and transparent outer corners");
            review.save(card.grab(), (translated ? QStringLiteral("-translate-%1") : QStringLiteral("-ocr-%1")).arg(width));
        }
        if (translated) {
            card.resize(1500, 650); card.applyLayout(true); card.m_mask->show(); QApplication::processEvents();
            review.save(card.grab(), QStringLiteral("-translate-edited"));
            card.m_mask->hide();card.m_text->clear();card.showEmptyState(QStringLiteral("翻译未完成"),QStringLiteral("请检查网络或服务后重试。"),true);
            review.save(card.grab(),QStringLiteral("-30-translate-error"));card.m_emptyIcon->hide();card.m_busy->hide();card.m_emptyRetry->hide();
            card.m_busy->hide();card.m_emptyIcon->hide();card.m_emptyRetry->hide();
            card.m_text->setPlainText(translatedText);
            card.setDisplay(2);card.resize(900,650);card.applyLayout(true);QApplication::processEvents();
            review.check(card.m_text->isVisible()&&!card.m_text->toPlainText().isEmpty(),
                         "translate-only keeps the translated text visible");
            review.save(card.grab(),QStringLiteral("-31-translate-only"));
        } else {
            card.resize(1100, 650); card.applyLayout(true); card.showFind(true); QApplication::processEvents();
            review.save(card.grab(), QStringLiteral("-ocr-find"));
            card.hideFind(); card.m_text->clear();card.showEmptyState(QStringLiteral("未识别到文字"),QStringLiteral("请调整截图范围后重新识别。"));
            review.save(card.grab(), QStringLiteral("-24-ocr-empty"));
            card.showEmptyState(QStringLiteral("文字识别暂不可用"),QStringLiteral("请检查本地识别资源后重试。"),true);
            review.save(card.grab(),QStringLiteral("-25-ocr-unavailable"));
            QMessageBox question(QMessageBox::Question,QStringLiteral("重新识别"),QStringLiteral("当前文字已被编辑。重新识别将覆盖你的修改。"),QMessageBox::Yes|QMessageBox::No,&card);
            question.setDefaultButton(QMessageBox::No);question.show();QApplication::processEvents();
            review.save(question.grab(),QStringLiteral("-35-retry-confirm"));question.button(QMessageBox::No)->click();
            review.check(question.result()==QMessageBox::No,"styled confirmation preserves the safe cancel result");
        }
        card.close();
    }
    return review.pass ? 0 : 1;
}

int PinWindow::testThemeUi(const QString &previewPath)
{
    Review review{previewPath};
    PinWindow pin(QPixmap::fromImage(documentImage()), QRect(180, 120, 900, 560));
    pin.setAttribute(Qt::WA_DeleteOnClose, false);
    pin.openEditor(); QApplication::processEvents();
    review.check(pin.m_toolbar && pin.m_toolbar->isVisible(), "pin tools use the actual shared toolbar");
    review.save(pin.grab(), QStringLiteral("-pin"));
    review.save(pin.m_toolbar->grab(), QStringLiteral("-pin-bar"));
    pin.openEditor(true);QApplication::processEvents();review.save(pin.m_toolbar->grab(),QStringLiteral("-17-longshot-editor"));
    pin.openEditor(false);
    pin.ensureSelToolbar();pin.m_selToolbar->show();QApplication::processEvents();
    review.save(pin.m_selToolbar->grab(),QStringLiteral("-11-pin-markup"));pin.m_selToolbar->hide();
    if (pin.m_canvas) {
        pin.m_canvas->armTool(CaptureOverlay::ATool::Pen); QApplication::processEvents();
        review.save(pin.m_canvas->optionsStrip()->grab(), QStringLiteral("-pin-options"));
    }
    QMenu *menu = pin.createContextMenu();
    menu->popup(pin.mapToGlobal(QPoint(100, 100))); QApplication::processEvents();
    review.save(menu->grab(), QStringLiteral("-pin-menu"));
    for(auto *action:menu->actions())if(action->menu()){
        QMenu *sub=action->menu();sub->popup(pin.mapToGlobal(QPoint(160,100)));QApplication::processEvents();
        review.save(sub->grab(),QStringLiteral("-12-pin-processing"));sub->close();break;
    }
    menu->close();
    pin.close();
    return review.pass ? 0 : 1;
}

int LongCapture::testThemeUi(const QString &previewPath)
{
    Review review{previewPath};
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    LongCapture capture(QRect(screen.topLeft() + QPoint(200, 120), QSize(640, 420)));
    capture.buildUi(); capture.addFrame(documentImage()); capture.updateButtons(); capture.updatePreview();
    QApplication::processEvents();
    review.check(capture.m_bar && capture.m_bar->isVisible(), "scrolling capture keeps every existing action in the compact bar");
    review.save(capture.m_bar->grab(), QStringLiteral("-longshot-bar"));
    if (capture.m_preview) review.save(capture.m_preview->grab(), QStringLiteral("-longshot-preview"));
    capture.m_autoScroll = 1; capture.m_auto->setChecked(true); capture.updateButtons();
    review.save(capture.m_bar->grab(), QStringLiteral("-longshot-auto"));
    review.check(capture.m_feedback&&capture.m_feedback->isVisible(),"automatic scrolling has visible directional feedback");
    review.save(capture.m_feedback->grab(),QStringLiteral("-14-longshot-status"));
    capture.m_autoScroll = 0;
    capture.m_alignmentLost=true;capture.updatePreview();QApplication::processEvents();
    if(capture.m_preview)review.save(capture.m_preview->grab(),QStringLiteral("-15-longshot-match"));
    review.check(capture.m_feedback->text().contains(QStringLiteral("匹配失败")),"match failure exposes a recovery instruction without discarding the session");
    review.save(capture.m_feedback->grab(),QStringLiteral("-15-longshot-status"));
    capture.setUiVisible(false);
    review.check(capture.m_feedbackAffinity||!capture.m_feedback->isVisible(),"feedback is excluded or hidden before capturing the scroll region");
    capture.setUiVisible(true);
    capture.m_cropPanel->show();QApplication::processEvents();review.save(capture.m_cropPanel->grab(),QStringLiteral("-16-longshot-crop"));
    capture.setVertical(false);QApplication::processEvents();review.save(capture.m_bar->grab(),QStringLiteral("-longshot-bar-horizontal"));
    capture.cleanupUi();
    return review.pass ? 0 : 1;
}

int Controller::testThemeUi(const QString &previewPath)
{
    Review review{previewPath};Controller controller;
    review.check(!controller.m_tray.icon().isNull(), "the tray exposes the application icon");
    review.check(controller.m_tray.contextMenu() == &controller.m_menu, "the tray keeps the menu on right click");
    review.save(controller.m_tray.icon().pixmap(64, 64),QStringLiteral("-40-tray-icon"));
    controller.m_menu.popup(QGuiApplication::primaryScreen()->availableGeometry().center());QApplication::processEvents();
    review.save(controller.m_menu.grab(),QStringLiteral("-40-tray"));
    int index=0;
    for(auto *action:controller.m_menu.actions())if(action->menu()){
        action->menu()->popup(controller.m_menu.pos()+QPoint(controller.m_menu.width(),30));QApplication::processEvents();
        review.save(action->menu()->grab(),index++==0?QStringLiteral("-41-detection-menu"):QStringLiteral("-42-language-menu"));action->menu()->close();
    }
    controller.m_menu.close();
    const auto captureCard=[&](bool alert,const QString &heading,const QString &detail,const QString &suffix) {
        QTimer::singleShot(0,[&] {
            for(QWidget *widget:QApplication::topLevelWidgets())
                if(auto *dialog=qobject_cast<QDialog *>(widget);dialog&&dialog->objectName()==QStringLiteral("messageCard")){
                    review.save(dialog->grab(),suffix);dialog->accept();return;
                }
        });
        if(alert)showAlert(nullptr,heading,detail);
        else showNotice(nullptr,heading,detail);
    };
    captureCard(true,QStringLiteral("无法注册截图热键"),QStringLiteral("F1 已被其他程序占用。\n请释放该快捷键后重新启动 LensSnip。"),QStringLiteral("-43-hotkey"));
    captureCard(true,QStringLiteral("保存失败"),QStringLiteral("请检查保存路径或磁盘空间后重试。\n当前截图与标注仍然保留。"),QStringLiteral("-44-save-error"));
    captureCard(false,QStringLiteral("语言已保存"),QStringLiteral("将在下次启动 LensSnip 时生效。"),QStringLiteral("-42-language-notice"));
    for(bool multiline:{false,true}){
        QInputDialog input;input.setWindowTitle(multiline?QStringLiteral("排除窗口检测"):QStringLiteral("保存预设"));
        input.setLabelText(multiline?QStringLiteral("每行一个程序文件名，下次截图生效。"):QStringLiteral("预设名称"));
        if(multiline)input.setOption(QInputDialog::UsePlainTextEditForTextInput);input.setTextValue(multiline?QStringLiteral("example.exe"):QStringLiteral("产品截图 900 × 480"));
        input.show();QApplication::processEvents();review.save(input.grab(),multiline?QStringLiteral("-41-exclusions"):QStringLiteral("-45-preset-name"));input.reject();
    }
    if(!previewPath.isEmpty() && qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
        QTemporaryDir directory;
        QFileDialog save(nullptr,QCoreApplication::translate("CaptureOverlay","保存图片"),directory.path(),QStringLiteral("PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)"));
        save.setAcceptMode(QFileDialog::AcceptSave);save.selectFile(QStringLiteral("capture.png"));
        bool captured=false;
        QTimer::singleShot(700,&save,[&] {
            struct Find {HWND handle=nullptr;DWORD pid=GetCurrentProcessId();} found;
            EnumWindows([](HWND window,LPARAM data)->BOOL {
                auto *f=reinterpret_cast<Find *>(data);DWORD pid=0;GetWindowThreadProcessId(window,&pid);
                wchar_t kind[64]={};GetClassNameW(window,kind,64);
                if(pid==f->pid&&IsWindowVisible(window)&&QString::fromWCharArray(kind)==QLatin1String("#32770")){f->handle=window;return FALSE;}
                return TRUE;
            },reinterpret_cast<LPARAM>(&found));
            if(found.handle){RECT rect{};GetWindowRect(found.handle,&rect);const QImage image=grabNative(QRect(rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top));
                review.save(QPixmap::fromImage(image),QStringLiteral("-46-native-save"));captured=!image.isNull();}
            save.reject();
        });
        save.exec();
        review.check(captured,"capture the native save dialog inside an empty temporary folder without writing a file");
        review.check(QDir(directory.path()).entryList(QDir::Files).isEmpty(),"native save preview leaves no output files");
    }
    return review.pass?0:1;
}
