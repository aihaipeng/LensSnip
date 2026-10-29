#include "shared/widgets.h"
#include "shared/dialog_chrome.h"
#include "shared/settings.h"
#include "platform/output.h"
#include "tests/test_support.h"
#include "pin/pin_window.h"
#include "longshot/long_capture.h"
#include "text/cards/ocr_dialog.h"
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QColor>
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QLabel>
#include <QStyleOptionSpinBox>
#include <QRegularExpression>
#include <QCursor>
#include <QScreen>
#include <QTranslator>
#include <QtMath>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>

int ColorBoard::testRendering()
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    for (int i = 0; i <= int(Icon::More); ++i) {   // Every glyph renders ink, from a resource or inline markup.
        const QImage img = makeIcon(Icon(i)).pixmap(QSize(16, 16)).toImage();
        bool ink = false;
        for (int y = 0; y < img.height() && !ink; ++y)
            for (int x = 0; x < img.width() && !ink; ++x) ink = qAlpha(img.pixel(x, y)) > 128;
        check(ink, "every toolbar icon renders from its SVG resource");
    }
    ColorBoard board(QColor("#ed1c24"));
    board.show();
    QApplication::processEvents();
    check(board.currentRect() == QRect(2, 4, 20, 20) && board.focusPolicy() == Qt::StrongFocus
              && board.cursor().shape() == Qt::PointingHandCursor && board.accessibleName() == QStringLiteral("颜色"),
          "the current color is the same size as the presets and the board accepts keyboard focus");
    for (int i = 0; i < CommonCount; ++i) {
        const QRect r = swatchRect(i);
        check(r.size() == QSize(20, 20) && board.rect().contains(r.adjusted(-1, -1, 1, 1))
                  && !r.intersects(board.currentRect()) && board.swatchAt(r.center()) == i,
              "six common cells are 20x20, inside the board, and hit-test to their own index");
        if (i) check(r.left() - swatchRect(i - 1).right() == 7 && board.swatchAt(r.topLeft() - QPoint(1, 0)) == -1,
                     "preset columns have a 6px gap that picks nothing");
    }
    check(swatchRect(0).top() == 4 && preset(0) == QColor("#000000")
              && preset(3) == QColor("#ed1c24") && preset(10) == QColor("#ffffff") && preset(19) == QColor("#c8bfe7"),
          "the established color values remain unchanged");
    check(board.width() == 187 && board.height() == 28 && !swatchRect(6).isValid()
              && board.findChildren<QToolButton *>().isEmpty(),
          "compact palette has six square colors and no extra action");
    auto click = [&board](const QPoint &at) {
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, QPointF(at), QPointF(board.mapToGlobal(at)), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&board, &event);
        }
    };
    QColor picked;
    int custom = 0;
    QObject::connect(&board, &ColorBoard::presetPicked, [&picked](const QColor &c) { picked = c; });
    QObject::connect(&board, &ColorBoard::customRequested, [&custom] { ++custom; });
    click(swatchRect(5).center());
    check(picked == QColor("#00a2e8") && custom == 0, "a preset click emits its color");
    picked = QColor();
    click(board.currentRect().center());
    check(custom == 1 && !picked.isValid(), "the current color square requests the full color dialog");
    click(swatchRect(1).topRight() + QPoint(2, 0));
    check(custom == 1 && !picked.isValid(), "clicking a gap does nothing");
    board.setFocus();
    picked = QColor();
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(&board, &right);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(&board, &enter);
    check(picked == QColor("#000000"), "arrow keys and Enter select a preset without the mouse");
    const qreal dpr = board.devicePixelRatioF();
    auto pixel = [&board, dpr](const QPoint &at) {
        const QImage image = board.grab().toImage();
        return image.pixelColor(qFloor((at.x() + 0.5) * dpr), qFloor((at.y() + 0.5) * dpr));
    };
    const QColor divider = pixel(QPoint(28, 14));
    const QColor dividerGap = pixel(QPoint(31, 14));
    check(divider != dividerGap && divider.lightness() >= 30 && divider.lightness() < 100,
          "a low-contrast divider separates the current color from the presets");
    const QRect cell = swatchRect(2);
    const QPoint borderAt(cell.left(), cell.center().y());
    const QPoint innerAt(cell.left() + 1, cell.center().y());
    const QColor idleBorder = pixel(borderAt);
    check(idleBorder.lightness() >= 80 && idleBorder.lightness() < 145,
          "an idle cell keeps a neutral outline on its own edge");
    QMouseEvent hover(QEvent::MouseMove, QPointF(cell.center()), QPointF(board.mapToGlobal(cell.center())),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&board, &hover);
    check(pixel(innerAt).lightness() > idleBorder.lightness() + 15
              && swatchRect(2) == cell,
          "the hovered cell draws a thicker brighter outline inside the same rect");
    board.setCurrentColor(QColor("#3f48cc"));
    const QColor border = pixel(QPoint(3, 14));
    check(board.currentColor() == QColor("#3f48cc") && pixel(QPoint(12, 14)) == QColor("#3f48cc")
              && border != QColor("#3f48cc") && border.saturation() > 150 && border.value() > 150,
          "the current-color preview repaints inside a bright rainbow border");
    if (!qgetenv("COLOR_BOARD_SHOT").isEmpty())
        check(board.grab().save(QString::fromLocal8Bit(qgetenv("COLOR_BOARD_SHOT"))), "save the color board preview");

    LensSnipSettings settings;
    const bool hadCustom = settings.contains(QStringLiteral("Capture/customColors"));
    const QStringList customBefore = settings.value(QStringLiteral("Capture/customColors")).toStringList();
    settings.remove(QStringLiteral("Capture/customColors"));
    {
        ColorDialog dialog(QColor("#112233"));
        auto *hex = dialog.findChild<QLineEdit *>(QStringLiteral("colorHex"));
        auto *alpha = dialog.findChild<QSpinBox *>(QStringLiteral("colorAlpha"));
        auto *opacity = dialog.findChild<QSlider *>(QStringLiteral("colorOpacity"));
        auto *hue = dialog.findChild<QSpinBox *>(QStringLiteral("colorH"));
        auto *saturation = dialog.findChild<QSpinBox *>(QStringLiteral("colorS"));
        auto *value = dialog.findChild<QSpinBox *>(QStringLiteral("colorV"));
        auto *pick = dialog.findChild<QPushButton *>(QStringLiteral("colorPickScreen"));
        auto *add = dialog.findChild<QPushButton *>(QStringLiteral("colorAddCustom"));
        auto *reset = dialog.findChild<QPushButton *>(QStringLiteral("colorResetCustom"));
        auto *custom0 = dialog.findChild<QToolButton *>(QStringLiteral("colorCustom0"));
        check(hex && alpha && opacity && hue && saturation && value && pick && add && reset && custom0,
              "the palette exposes hex, opacity, HSV, screen picking and custom colors");
        dialog.show();QApplication::processEvents();
        const QScreen *current=QGuiApplication::screenAt(QCursor::pos());
        if(!current)current=QGuiApplication::primaryScreen();
        check(dialog.pos()==current->availableGeometry().center()-QRect(QPoint(),dialog.size()).center(),
              "the palette opens centered on the current screen");
        for(auto *spin:dialog.findChildren<QSpinBox *>())if(spin->objectName().startsWith(QStringLiteral("color"))){
            check(spin->parentWidget()->rect().contains(spin->geometry()),"numeric fields stay within their form");
            check(spin->findChild<QLineEdit *>()->width()>=spin->fontMetrics().horizontalAdvance(QStringLiteral("359"))+4,
                  "numeric fields display three digits without clipping");
        }
        auto *plane=dialog.findChild<QWidget *>(QStringLiteral("colorPlane"));
        check(dialog.width()==560&&plane&&plane->size()==QSize(256,256)
                  &&hex->alignment().testFlag(Qt::AlignHCenter)&&pick->text().isEmpty(),
              "the compact palette has a square plane, centered HEX and icon-only screen picker");
        check(dialog.findChildren<QToolButton *>(QRegularExpression(QStringLiteral("^colorCustom[0-9]+$"))).size()==6,
              "the custom palette exposes six slots");
        auto *custom1=dialog.findChild<QToolButton *>(QStringLiteral("colorCustom1"));
        check(custom0->size()==QSize(32,32)&&custom1->x()-custom0->x()==52,
              "custom colors are 32px squares with 20px gaps");
        auto *hueSlider=dialog.findChild<QSlider *>(QStringLiteral("colorHue"));
        const auto centerY=[&dialog](QWidget *control){return control->mapTo(&dialog,QPoint(0,control->height()/2)).y();};
        check(hueSlider&&qAbs(centerY(opacity)-centerY(hueSlider))<=1,
              "the opacity slider is level with the hue slider");
        bool customHeading=false;
        for(auto *label:dialog.findChildren<QLabel *>())
            customHeading=customHeading||label->text()==QStringLiteral("自定义颜色");
        check(!customHeading,"the custom color swatches have no heading text");
        hex->setFocus(Qt::MouseFocusReason);QApplication::processEvents();
        check(!hex->property("kbdFocus").toBool(),"mouse focus does not add a colored field outline");
        const QImage fieldImage=hex->grab().toImage();
        check(fieldImage.pixelColor(fieldImage.width()/2,0).saturation()<20,
              "the clicked HEX border stays neutral instead of accent-colored");
        hex->clearFocus();hex->setFocus(Qt::TabFocusReason);QApplication::processEvents();
        check(hex->property("kbdFocus").toBool(),"keyboard focus retains a visible neutral indicator");
        auto *red=dialog.findChild<QSpinBox *>(QStringLiteral("colorR"));
        QStyleOptionSpinBox option;option.initFrom(red);option.rect=red->rect();
        option.buttonSymbols=QAbstractSpinBox::UpDownArrows;
        const QRect up=red->style()->subControlRect(QStyle::CC_SpinBox,&option,QStyle::SC_SpinBoxUp,red);
        const QRect down=red->style()->subControlRect(QStyle::CC_SpinBox,&option,QStyle::SC_SpinBoxDown,red);
        auto clickStep=[red](QPoint at){
            for(auto type:{QEvent::MouseButtonPress,QEvent::MouseButtonRelease}){
                QMouseEvent e(type,at,red->mapToGlobal(at),Qt::LeftButton,
                              type==QEvent::MouseButtonPress?Qt::LeftButton:Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(red,&e);
            }
        };
        const int beforeStep=red->value();clickStep(up.center());
        check(red->value()==beforeStep+1,"the actual up subcontrol increments the real color channel");
        clickStep(down.center());check(red->value()==beforeStep,"the actual down subcontrol decrements the channel");
        red->setValue(255);clickStep(up.center());check(red->value()==255,"the up button clamps at the channel maximum");
        red->setValue(0);clickStep(down.center());check(red->value()==0,"the down button clamps at the channel minimum");
        red->setValue(beforeStep);
        const QImage planeImage=plane->grab().toImage();
        const int corner=qCeil(8*plane->devicePixelRatioF());
        for(int x=corner;x<planeImage.width()-corner;++x)
            check(planeImage.pixelColor(x,planeImage.height()-1).value()<=2,
                  "the color plane bottom edge is black without a bright seam");
        bool requested = false;
        QObject::connect(&dialog, &ColorDialog::screenPickRequested, [&requested] { requested = true; });
        pick->click();
        check(requested, "the screen-pick button requests a sampler without changing the color");
        hue->setValue(120);
        saturation->setValue(100);
        value->setValue(100);
        check(dialog.currentColor().hsvHue() >= 119 && dialog.currentColor().hsvHue() <= 121
                  && dialog.currentColor().green() == 255,
              "HSV fields drive the real current color");
        alpha->setValue(80);
        check(dialog.currentColor().alpha() == 80 && opacity->value() == 80,
              "the opacity field and slider stay in sync");
        opacity->setValue(60);
        check(dialog.currentColor().alpha() == 60 && alpha->value() == 60,
              "the opacity slider updates the real current color");
        alpha->setValue(80);
        const QColor savedColor = dialog.currentColor();
        add->click();
        const QStringList stored = settings.value(QStringLiteral("Capture/customColors")).toStringList();
        check(stored.size() == 6 && custom0->isEnabled()&&QColor(stored[0])==savedColor,
              "saving the current color prepends it and keeps six custom slots");
        hue->setValue(0);
        custom0->click();
        check(dialog.currentColor() == savedColor, "a custom swatch restores its saved color");
        {
            ColorDialog reopened(Qt::red);
            reopened.findChild<QToolButton *>(QStringLiteral("colorCustom0"))->click();
            check(reopened.currentColor()==savedColor,"saved alpha and color survive reopening the dialog");
        }
        reset->click();
        check(settings.value(QStringLiteral("Capture/customColors")).toStringList().size()==6
                  &&QColor(settings.value(QStringLiteral("Capture/customColors")).toStringList()[0])==QColor("#d3cec7")
                  &&dialog.currentColor()==savedColor,
                  "reset restores the opening palette without changing the selected color");
        {
            ColorDialog reopened(Qt::red);
            reopened.findChild<QToolButton *>(QStringLiteral("colorCustom0"))->click();
            check(reopened.currentColor()==QColor("#d3cec7"),"reset colors survive reopening the dialog");
        }
        for(auto *label:dialog.findChildren<QLabel *>())
            check(!label->text().contains(QStringLiteral("已保存"))&&!label->text().contains(QStringLiteral("已恢复")),
                  "save and reset have no success text");
        if(!qgetenv("COLOR_DIALOG_SHOT").isEmpty())
            check(dialog.grab().save(QString::fromLocal8Bit(qgetenv("COLOR_DIALOG_SHOT"))),"save the color dialog preview");
        settings.remove(QStringLiteral("Capture/customColors"));   // Do not leak test state if the checks pass.
    }
    {
        const QStringList legacy{QStringLiteral("#ff112233"),QStringLiteral("#ff223344"),QStringLiteral("#ff334455"),
            QStringLiteral("#ff445566"),QStringLiteral("#ff556677"),QStringLiteral("#ff667788"),
            QStringLiteral("#ff778899"),QStringLiteral("#ff8899aa")};
        settings.setValue(QStringLiteral("Capture/customColors"),legacy);
        ColorDialog dialog(Qt::red);
        check(settings.value(QStringLiteral("Capture/customColors")).toStringList()==legacy,
              "opening the six-slot palette does not truncate legacy saved colors");
        dialog.findChild<QPushButton *>(QStringLiteral("colorResetCustom"))->click();
        check(settings.value(QStringLiteral("Capture/customColors")).toStringList()==legacy.mid(0,6),
              "explicit reset restores the opening six colors");
    }
    if (hadCustom) settings.setValue(QStringLiteral("Capture/customColors"), customBefore);
    else settings.remove(QStringLiteral("Capture/customColors"));

    {
        QTranslator english;
        check(english.load(QCoreApplication::applicationDirPath()+QStringLiteral("/language/lenssnip_en_US.qm")),
              "the English language pack loads for the palette");
        qApp->installTranslator(&english);
        ColorDialog dialog(Qt::red);
        check(dialog.findChild<QPushButton *>(QStringLiteral("colorAddCustom"))->text()==QStringLiteral("Save")
                  &&dialog.findChild<QPushButton *>(QStringLiteral("colorResetCustom"))->text()==QStringLiteral("Reset")
                  &&dialog.findChild<QPushButton *>(QStringLiteral("colorResetCustom"))->accessibleName()==QStringLiteral("Reset custom colors"),
              "the palette actions and accessible names are translated");
        qApp->removeTranslator(&english);
    }

    QImage pickImage(120, 90, QImage::Format_RGB32);
    pickImage.fill(QColor("#123456"));
    const ScreenFrame pickFrame{QRect(0, 0, 120, 90), QRect(0, 0, 120, 90), pickImage};
    check(ScreenColorPicker::sample({pickFrame}, QPoint(20, 30)) == QColor("#123456")
              && !ScreenColorPicker::sample({pickFrame}, QPoint(200, 30)).isValid(),
          "screen sampling reads the frozen frame and rejects points outside it");
    QImage scaledImage(240, 180, QImage::Format_RGB32);
    scaledImage.fill(QColor("#123456"));
    scaledImage.setPixelColor(60, 80, QColor("#abcdef"));
    const ScreenFrame scaledFrame{QRect(0, 0, 120, 90), QRect(0, 0, 240, 180), scaledImage};
    check(ScreenColorPicker::sample({scaledFrame}, QPoint(30, 40)) == QColor("#abcdef"),
          "screen sampling maps logical cursor coordinates onto native pixels at high DPI");
    ScreenColorPicker picker({pickFrame});
    picker.show();
    QApplication::processEvents();
    const QPoint pickAt(30, 40);
    QMouseEvent move(QEvent::MouseMove, QPointF(pickAt), QPointF(picker.mapToGlobal(pickAt)),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&picker, &move);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pickAt), QPointF(picker.mapToGlobal(pickAt)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&picker, &press);
    check(picker.result() == QDialog::Accepted && picker.pickedColor() == QColor("#123456"),
          "a click in the screen picker returns the sampled color");
    picker.close();

    std::printf("PASS: color board geometry, hit testing, presets, custom request, and hover at DPR %.2f\n", double(dpr));
    return 0;
}
