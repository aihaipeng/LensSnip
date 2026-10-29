#include "app/controller.h"
#include "shared/dialog_chrome.h"
#include "shared/i18n.h"
#include "shared/settings.h"
#include "shared/widgets.h"
#include "history/history_store.h"
#include <QGuiApplication>
#include <QCursor>
#include <QPixmap>
#include <QSizeF>
#include <QSignalBlocker>
#include <QVector>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QScreen>
#include <QMenu>
#include <QtMath>
#include <QMessageBox>
#include <QSettings>
#include <QInputDialog>
#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <qt_windows.h>

namespace {
// Tray and taskbar identity: white crop marks on the application accent.
QIcon lensSnipIcon()
{
    QIcon icon;
    for (int size : {16, 20, 24, 32, 48, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Bar::accent);
        painter.drawRoundedRect(QRectF(0, 0, size, size), size * 0.24, size * 0.24);
        const qreal inset = size * 0.28, arm = size * 0.17;
        QPainterPath marks;
        marks.moveTo(inset, inset + arm); marks.lineTo(inset, inset); marks.lineTo(inset + arm, inset);
        marks.moveTo(size - inset - arm, inset); marks.lineTo(size - inset, inset); marks.lineTo(size - inset, inset + arm);
        marks.moveTo(size - inset, size - inset - arm); marks.lineTo(size - inset, size - inset); marks.lineTo(size - inset - arm, size - inset);
        marks.moveTo(inset + arm, size - inset); marks.lineTo(inset, size - inset); marks.lineTo(inset, size - inset - arm);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::white, qMax(1.0, size * 0.095), Qt::SolidLine, Qt::SquareCap));
        painter.drawPath(marks);
        icon.addPixmap(pixmap);
    }
    return icon;
}
}

bool HotkeyFilter::nativeEventFilter(const QByteArray &, void *message, qintptr *)
{
    MSG *msg = static_cast<MSG *>(message);
    if (msg->message == WM_HOTKEY && msg->wParam == 1 && onHotkey)
        onHotkey();
    return false;
}

Controller::Controller()
{
    const QIcon icon = lensSnipIcon();
    QApplication::setWindowIcon(icon);
    m_tray.setIcon(icon);
    m_tray.setToolTip(tr("LensSnip · F1 截图"));
    connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        // Left click starts a capture; the hovered menu stays on right click.
        if ((reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            && !QApplication::activeModalWidget())
            startCapture();
    });
    QMenu *m = &m_menu;
    m->setStyleSheet(darkMenuQss);
    m->addAction(tr("截图 (F1)"), this, [this] { startCapture(false); });
    m->addAction(tr("截图历史…"), this, &Controller::openHistory);
    m_historyEnabled = m->addAction(tr("记录截图历史"));
    m_historyEnabled->setCheckable(true);
    m_historyEnabled->setChecked(HistoryStore::enabled());
    m_historyEnabled->setToolTip(tr("默认关闭。开启后，未保存的截图也会复制到本机历史。"));
    connect(m_historyEnabled, &QAction::toggled, this, [this](bool enabled) {
        HistoryStore::setEnabled(enabled);
        if (m_history) m_history->setRecordingEnabled(enabled);
    });
    QMenu *detection = m->addMenu(tr("窗口/UI 检测"));
    LensSnipSettings settings;
    for (const auto &entry : {qMakePair(tr("自动检测窗口"), QStringLiteral("detect_windows")),
                              qMakePair(tr("自动检测 UI 元素"), QStringLiteral("detect_ui_elements"))}) {
        QAction *action = detection->addAction(entry.first);
        action->setCheckable(true);
        action->setChecked(settings.value(QStringLiteral("Snip/") + entry.second, true).toBool());
        const QString key = entry.second;
        connect(action, &QAction::toggled, this, [key](bool enabled) {
            LensSnipSettings().setValue(QStringLiteral("Snip/") + key, enabled);
        });
    }
    detection->addAction(tr("排除程序…"), this, [] {
        LensSnipSettings settings;
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(nullptr, Controller::tr("排除窗口检测"),
            Controller::tr("每行一个程序文件名，例如 example.exe（不区分大小写，下次截图生效）："),
            settings.value(QStringLiteral("Snip/exclude_from_detection")).toStringList().join(QLatin1Char('\n')), &ok);
        if (!ok) return;
        QStringList names;
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            const QString name = line.trimmed().toLower();
            if (!name.isEmpty() && !names.contains(name)) names.append(name);
        }
        settings.setValue(QStringLiteral("Snip/exclude_from_detection"), names);
    });
    QMenu *annotation = m->addMenu(tr("标注"));
    annotation->setIcon(barIcon(Icon::Watermark));
    QAction *autoWatermark = annotation->addAction(tr("截图时自动添加水印"));
    autoWatermark->setCheckable(true);
    autoWatermark->setChecked(LensSnipSettings().value(QStringLiteral("Watermark/AutoAdd"), false).toBool());
    connect(autoWatermark, &QAction::toggled, this, [](bool enabled) {
        LensSnipSettings().setValue(QStringLiteral("Watermark/AutoAdd"), enabled);
    });
    QAction *arrowComment = annotation->addAction(tr("双击箭头不出现评论"));
    arrowComment->setCheckable(true);
    arrowComment->setChecked(LensSnipSettings().value(
        QStringLiteral("Capture/DisableArrowDoubleClickComment"), false).toBool());
    connect(arrowComment, &QAction::toggled, this, [](bool enabled) {
        LensSnipSettings().setValue(QStringLiteral("Capture/DisableArrowDoubleClickComment"), enabled);
    });
    QMenu *language = m->addMenu(tr("语言"));
    QActionGroup *languageGroup = new QActionGroup(language);
    languageGroup->setExclusive(true);
    const QString configured = I18n::configuredLocale();
    const auto addLanguage = [&](const QString &locale) {
        QAction *action = language->addAction(I18n::displayName(locale));
        action->setCheckable(true);
        action->setChecked(configured == locale);
        languageGroup->addAction(action);
        connect(action, &QAction::triggered, this, [locale] {
            I18n::saveLocale(locale);
            showNotice(nullptr, QCoreApplication::translate("Controller", "语言已保存"),
                       QCoreApplication::translate("Controller", "将在下次启动 LensSnip 时生效。"));
        });
    };
    addLanguage(QStringLiteral("system"));
    for (const QString &locale : I18n::availableLocales())
        if (locale != QLatin1String("system")) addLanguage(locale);
    m->addSeparator();
    QAction *about = m->addAction(tr("关于 LensSnip"));
    connect(about, &QAction::triggered, this, [this] {
        showNotice(nullptr, tr("关于 LensSnip"),
                   tr("F1 或左键单击托盘图标开始截图。\n右键单击托盘图标打开此菜单。\n"
                      "贴图、长截图与 OCR 可在截图工具栏中使用。"));
    });
    m->addAction(tr("退出"), qApp, &QApplication::quit);
    const QList<QAction *> actions=m->actions();
    actions[0]->setIcon(barIcon(Icon::Crop));actions[0]->setText(tr("截图")+QStringLiteral("\tF1"));
    actions[1]->setIcon(barIcon(Icon::Clock));
    detection->setIcon(barIcon(Icon::View));language->setIcon(barIcon(Icon::Translate));
    actions.last()->setIcon(barIcon(Icon::Cancel));
    m_tray.setContextMenu(m);
    m_tray.show();
}

void Controller::startCapture(bool includePopups)
{
    if (m_startingCapture) return;                 // Tray click and F1 can race while the frame freezes.
    struct ResetFlag { bool &flag; ~ResetFlag() { flag = false; } } reset{m_startingCapture};
    m_startingCapture = true;
    const bool popupOpen = m_menu.isVisible() || QApplication::activePopupWidget();
    if (m_capture && m_capture->isVisible()) {
        if (!includePopups) {
            if (m_menu.isVisible()) m_menu.close();
            if (QWidget *popup = QApplication::activePopupWidget()) popup->close();
        }
        m_capture->raise();
        m_capture->activateWindow();
        return;
    }
    // A tray/menu click asks to close its own menu first. F1 must not: the open
    // menu is part of what the user wants to capture, so freeze and snapshot it
    // before dismissing the popup.
    if (!includePopups && popupOpen) {
        if (m_menu.isVisible()) m_menu.close();
        if (QWidget *popup = QApplication::activePopupWidget()) popup->close();
        QTimer::singleShot(0, this, [this] { startCapture(false); });
        return;
    }
    const QVector<ScreenFrame> frames = freezeScreens();   // Freeze before showing the selection overlay.
    if (frames.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("LensSnip"), tr("无法读取屏幕像素"));
        return;
    }
    LensSnipSettings settings;
    DetectionOptions options;
    options.windows = settings.value(QStringLiteral("Snip/detect_windows"), true).toBool();
    options.elements = settings.value(QStringLiteral("Snip/detect_ui_elements"), true).toBool();
    options.excludedPrograms = settings.value(QStringLiteral("Snip/exclude_from_detection")).toStringList();
    options.disabledUiPrograms = settings.value(QStringLiteral("Snip/crashes")).toStringList();
    // Snapshot before the overlay activates: browser bubbles such as Chrome's
    // page QR code close on focus loss but are still present in the frozen frame.
    const WindowTargets nativeWindows = options.windows
        ? detectWindowRects(nullptr, options.excludedPrograms) : WindowTargets();
    if (includePopups && popupOpen) {
        if (m_menu.isVisible()) m_menu.close();
        if (QWidget *popup = QApplication::activePopupWidget()) popup->close();
    }
    auto *ov = new CaptureOverlay(frames, nativeWindows, options, true);
    m_capture = ov;
    connect(ov, &CaptureOverlay::grabbed, this, [](const QPixmap &pix, const QRect &region) {
        if (!pix.isNull() && !region.isEmpty()) new PinWindow(pix, region);
    });
    connect(ov, &CaptureOverlay::captured, this, [](const QPixmap &pix, const QRect &) {
        if (!pix.isNull()) HistoryStore::record(pix.toImage(), QStringLiteral("capture"));
    });
    connect(ov, &CaptureOverlay::longRequested, this, [](const QRect &region) {
        auto *lc = new LongCapture(region);
        connect(lc, &LongCapture::captured, lc, [](const QImage &image) {
            HistoryStore::record(image, QStringLiteral("longshot"));
        });
        connect(lc, &LongCapture::finished, lc, [lc](const QPixmap &pix, bool edit) {
            const QRect region = lc->region();   // The selection may have moved along the scroll axis.
            const QScreen *scr = QGuiApplication::screenAt(region.center());
            if (!scr) scr = QGuiApplication::primaryScreen();
            // The pin and the editor share one centered, proportionally fitted placement.
            auto *pin = new PinWindow(pix, LongCapture::editorGeometry(pix.size(), scr->devicePixelRatio(),
                                                                       scr->availableGeometry()));
            if (!edit)
                return;
            // The editor can come back to the still-open capture.
            QPointer<LongCapture> session(lc);
            connect(pin, &PinWindow::editorLeft, lc, [lc](bool back) { back ? lc->resume() : lc->cancel(); });
            connect(pin, &QObject::destroyed, lc, [session] { if (session && session->suspended()) session->cancel(); });
            pin->openEditor(true);
        });
        lc->start();
    });
}

void Controller::openHistory()
{
    if (m_history) {
        m_history->raise();
        m_history->activateWindow();
        return;
    }

    auto *dialog = new HistoryDialog;
    m_history = dialog;
    connect(dialog, &HistoryDialog::pinRequested, this, [](const QImage &image) {
        if (image.isNull()) return;
        QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const QRect available = screen->availableGeometry();
        const qreal dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
        QSizeF logical(image.width() / dpr, image.height() / dpr);
        const QSizeF limit(available.width() * 0.8, available.height() * 0.8);
        if (logical.width() > limit.width() || logical.height() > limit.height())
            logical.scale(limit, Qt::KeepAspectRatio);
        QRect rect(QPoint(0, 0), logical.toSize());
        rect.moveCenter(available.center());
        new PinWindow(QPixmap::fromImage(image), rect);
    });
    connect(dialog, &HistoryDialog::enabledChanged, this, [this](bool enabled) {
        if (!m_historyEnabled || m_historyEnabled->isChecked() == enabled) return;
        const QSignalBlocker blocker(m_historyEnabled);
        m_historyEnabled->setChecked(enabled);
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
