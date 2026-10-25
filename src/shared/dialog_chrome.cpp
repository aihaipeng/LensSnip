#include "shared/dialog_chrome.h"
#include "shared/settings.h"
#include "shared/widgets.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QInputDialog>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <functional>
#include <memory>

namespace {
class DialogTitle : public QWidget
{
public:
    explicit DialogTitle(QDialog *dialog) : QWidget(dialog), m_dialog(dialog) {
        setFixedHeight(28);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0,0,0,0);
        auto *label = new QLabel(dialog->windowTitle(), this);
        label->setStyleSheet(QStringLiteral("color:#f4f5f8;font-size:14px;font-weight:600;background:transparent;"));
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(label); row->addStretch();
        auto *close = new QToolButton(this);
        close->setObjectName(QStringLiteral("dialogClose"));
        close->setIcon(cardIcon(Icon::CardClose)); close->setIconSize(QSize(20,20)); close->setFixedSize(28,28);
        close->setAccessibleName(QCoreApplication::translate("OcrDialog","关闭 (Esc)"));
        close->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;}QToolButton:hover{background:#222326;border-radius:6px;}"));
        row->addWidget(close);
        connect(close,&QToolButton::clicked,dialog,&QDialog::reject);
    }
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button()==Qt::LeftButton) m_drag=event->globalPosition().toPoint()-m_dialog->pos();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (event->buttons()&Qt::LeftButton) m_dialog->move(event->globalPosition().toPoint()-m_drag);
    }
private:
    QDialog *m_dialog;
    QPoint m_drag;
};
class DialogDrag : public QObject
{
public:
    explicit DialogDrag(QDialog *dialog):QObject(dialog),m_dialog(dialog){}
    bool eventFilter(QObject *,QEvent *event) override {
        if(event->type()==QEvent::MouseButtonPress){auto *mouse=static_cast<QMouseEvent *>(event);
            m_pressed=mouse->button()==Qt::LeftButton&&mouse->position().y()<56;
            if(m_pressed)m_offset=mouse->globalPosition().toPoint()-m_dialog->pos();
        }else if(event->type()==QEvent::MouseMove&&m_pressed){auto *mouse=static_cast<QMouseEvent *>(event);
            if(mouse->buttons()&Qt::LeftButton)m_dialog->move(mouse->globalPosition().toPoint()-m_offset);
        }else if(event->type()==QEvent::MouseButtonRelease)m_pressed=false;
        return false;
    }
private:
    QDialog *m_dialog;QPoint m_offset;bool m_pressed=false;
};
void styleMessageBox(QMessageBox *message)
{
    if(message->property("lensSnipDialogChrome").toBool())return;
    message->setProperty("lensSnipDialogChrome",true);
    auto *surface=new QWidget(message);
    surface->setObjectName(QStringLiteral("messageSurface"));
    surface->setAttribute(Qt::WA_TransparentForMouseEvents);
    surface->setStyleSheet(QStringLiteral("background:#18191d;border:1px solid #2b2d33;border-radius:12px;"));
    surface->setGeometry(message->rect());
    surface->show();
    surface->lower();
    class SurfaceSync : public QObject {
    public:
        SurfaceSync(QMessageBox *message,QWidget *surface):QObject(message),m_message(message),m_surface(surface) {}
        bool eventFilter(QObject *,QEvent *event) override {
            if(event->type()==QEvent::Resize||event->type()==QEvent::Show){
                m_surface->setGeometry(m_message->rect());m_surface->lower();
            }
            return false;
        }
    private:
        QMessageBox *m_message;QWidget *m_surface;
    };
    message->installEventFilter(new SurfaceSync(message,surface));
    message->setStyleSheet(QStringLiteral(
        "QLabel{color:#f4f5f8;background:transparent;border:none;}"
        "QLabel#qt_msgbox_informativelabel{color:#9c9da1;font-size:12px;}"
        "QPushButton{background:#18191d;border:1px solid #2b2d33;color:#f4f5f8;"
        "border-radius:6px;padding:0 12px;min-height:32px;font-size:14px;text-align:center;}"
        "QPushButton:hover{background:#222326;}"
        "QPushButton:default{border:2px solid #828fff;padding:0 11px;}"));
    message->setMinimumSize(480,200);
    if(message->icon()==QMessageBox::Question||message->icon()==QMessageBox::Information)
        message->setIcon(QMessageBox::NoIcon);
    else if(message->icon()==QMessageBox::Warning||message->icon()==QMessageBox::Critical) {
        QPixmap warning=makeIcon(Icon::Warning,20,true).pixmap(20,20);QPainter p(&warning);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);p.fillRect(warning.rect(),QColor("#e9be74"));
        p.end();message->setIconPixmap(warning);
    }
    if(auto *label=message->findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
        label->setWordWrap(true);
        const int textWidth=label->fontMetrics().horizontalAdvance(message->text());
        label->setFixedWidth(qMin(432,qMax(220,textWidth+2)));
    }
    if(auto *detail=message->findChild<QLabel *>(QStringLiteral("qt_msgbox_informativelabel"))) {
        detail->setWordWrap(true);detail->setFixedWidth(432);
    }
    if(message->standardButtons().testFlag(QMessageBox::Yes)&&message->standardButtons().testFlag(QMessageBox::No)) {
        // Explicit action/danger properties keep translated titles out of button labelling.
        const bool destructive=message->property("lenssnipDestructive").toBool();
        const QString propertyAction=message->property("lenssnipAction").toString();
        const QString action=propertyAction.isEmpty()
                ?QCoreApplication::translate("OcrDialog","重新识别") : propertyAction;
        auto *yes=message->button(QMessageBox::Yes);
        yes->setText(action);
        const QString danger=QStringLiteral(
            "QPushButton{background:#c04a4a;border:1px solid #c04a4a;color:#ffffff;border-radius:6px;padding:0 12px;min-height:32px;}"
            "QPushButton:hover{background:#a83f3f;border-color:#a83f3f;}");
        const QString accent=QStringLiteral(
            "QPushButton{background:#5e6ad2;border:1px solid #5e6ad2;color:#ffffff;border-radius:6px;padding:0 12px;min-height:32px;}"
            "QPushButton:hover{background:#5661c4;border-color:#5661c4;}");
        yes->setStyleSheet(destructive?danger:accent);
        if(message->defaultButton()==yes)
            yes->setStyleSheet((destructive?danger:accent)
                +QStringLiteral("QPushButton{border:2px solid #f4f5f8;padding:0 11px;}"));
        message->button(QMessageBox::No)->setText(QCoreApplication::translate("QPlatformTheme","Cancel"));
    }
    if(auto *ok=message->button(QMessageBox::Ok))
        ok->setStyleSheet(QStringLiteral("background:#5e6ad2;border:1px solid #5e6ad2;color:#ffffff;border-radius:6px;padding:0 12px;min-height:32px;"
                                         "QPushButton:hover{background:#5661c4;border-color:#5661c4;}"));
}
class DialogPolish : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *object,QEvent *event) override {
        if (event->type()==QEvent::Polish)
            if (auto *dialog=qobject_cast<QDialog *>(object)) {
                if (auto *message=qobject_cast<QMessageBox *>(dialog)) {
                    message->setWindowFlag(Qt::FramelessWindowHint);
                    message->setAttribute(Qt::WA_TranslucentBackground);
                    QTimer::singleShot(0,message,[message] {
                        styleMessageBox(message);
                        message->setFixedSize(480,200);
                        if(auto *layout=message->layout()){layout->invalidate();layout->activate();}
                        message->update();
                    });
                } else if (qobject_cast<QInputDialog *>(dialog))
                    styleDialog(dialog);
            }
        return false;
    }
};
class ColorPlane : public QWidget
{
public:
    explicit ColorPlane(QWidget *parent):QWidget(parent) {setMinimumSize(280,200);setCursor(Qt::CrossCursor);}
    QColor color;
    std::function<void(qreal,qreal)> picked;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);p.setBrush(QColor::fromHsvF(qMax(0.0,color.hsvHueF()),1,1));p.drawRoundedRect(rect(),8,8);
        QLinearGradient white(0,0,width(),0);white.setColorAt(0,Qt::white);white.setColorAt(1,QColor(255,255,255,0));p.fillRect(rect(),white);
        QLinearGradient black(0,0,0,height());black.setColorAt(0,QColor(0,0,0,0));black.setColorAt(1,Qt::black);p.fillRect(rect(),black);
        const QPointF marker(color.hsvSaturationF()*(width()-1),(1-color.valueF())*(height()-1));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor("#18191d"),4));p.drawEllipse(marker,7,7);
        p.setPen(QPen(Qt::white,2));p.drawEllipse(marker,6,6);
    }
    void mousePressEvent(QMouseEvent *e) override {if(e->button()==Qt::LeftButton) pick(e->position());}
    void mouseMoveEvent(QMouseEvent *e) override {if(e->buttons()&Qt::LeftButton) pick(e->position());}
private:
    void pick(QPointF p){if(picked)picked(qBound(0.0,p.x()/qMax(1,width()-1),1.0),1-qBound(0.0,p.y()/qMax(1,height()-1),1.0));}
};
}

void styleDialog(QDialog *dialog,bool showTitle)
{
    if(dialog->property("lensSnipDialogChrome").toBool())return;
    dialog->setProperty("lensSnipDialogChrome",true);
    dialog->setWindowFlag(Qt::FramelessWindowHint);
    dialog->setAttribute(Qt::WA_TranslucentBackground);
    auto *body=new QWidget(dialog);
    body->setObjectName(QStringLiteral("dialogContent"));
    if (dialog->layout()) body->setLayout(dialog->layout());
    auto *outer=new QVBoxLayout(dialog);outer->setContentsMargins(0,0,0,0);
    auto *surface=new QWidget(dialog);surface->setObjectName(QStringLiteral("dialogSurface"));
    surface->setStyleSheet(QStringLiteral("QWidget#dialogSurface{background:#18191d;border:1px solid #2b2d33;border-radius:12px;}QWidget#dialogContent{background:transparent;border:none;}"));
    auto *column=new QVBoxLayout(surface);column->setContentsMargins(showTitle?24:0,showTitle?12:0,showTitle?24:0,showTitle?24:0);column->setSpacing(12);
    if(showTitle)column->addWidget(new DialogTitle(dialog));
    column->addWidget(body,1);outer->addWidget(surface);
    if(!showTitle)dialog->installEventFilter(new DialogDrag(dialog));
    if(qobject_cast<QInputDialog *>(dialog))dialog->setMinimumWidth(480);
}

void installDialogChrome(QApplication *application)
{
    application->installEventFilter(new DialogPolish(application));
}

namespace {
void showMessageCard(QWidget *parent,const QString &heading,const QString &detail,
                     bool alert)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("messageCard"));
    dialog.setWindowTitle(alert?QStringLiteral("LensSnip"):heading);
    auto *layout=new QVBoxLayout(&dialog);
    layout->setContentsMargins(0,0,0,0);
    auto *content=new QWidget(&dialog);
    auto *body=new QVBoxLayout(content);
    body->setContentsMargins(0,0,0,0);body->setSpacing(12);
    if(alert) {
        auto *headingRow=new QHBoxLayout;headingRow->setSpacing(12);
        auto *icon=new QLabel(content);
        QPixmap warning=makeIcon(Icon::Warning,20,true).pixmap(20,20);QPainter painter(&warning);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);painter.fillRect(warning.rect(),QColor("#e9be74"));
        painter.end();icon->setPixmap(warning);icon->setFixedSize(20,20);headingRow->addWidget(icon);
        auto *title=new QLabel(heading,content);
        title->setStyleSheet(QStringLiteral("color:#f4f5f8;font-size:16px;font-weight:600;background:transparent;"));
        headingRow->addWidget(title);headingRow->addStretch();body->addLayout(headingRow);
    } else {
        auto *title=new QLabel(heading,content);
        title->setStyleSheet(QStringLiteral("color:#f4f5f8;font-size:16px;font-weight:600;background:transparent;"));
        body->addWidget(title);
    }
    auto *text=new QLabel(detail,content);
    text->setWordWrap(true);
    text->setStyleSheet(QStringLiteral("color:#9c9da1;font-size:14px;background:transparent;"));
    body->addWidget(text);body->addStretch();
    auto *buttons=new QHBoxLayout;buttons->addStretch();
    auto *ok=new QPushButton(QCoreApplication::translate("OcrDialog","确定"),content);
    ok->setFixedSize(80,32);
    ok->setStyleSheet(QStringLiteral("QPushButton{background:#5e6ad2;border:1px solid #5e6ad2;color:#ffffff;border-radius:6px;font-size:14px;text-align:center;}QPushButton:hover{background:#6f7ae1;}"));
    buttons->addWidget(ok);body->addLayout(buttons);
    layout->addWidget(content);
    QObject::connect(ok,&QPushButton::clicked,&dialog,&QDialog::accept);
    styleDialog(&dialog,alert);
    if(auto *surface=dialog.findChild<QWidget *>(QStringLiteral("dialogSurface")))
        if(auto *column=surface->layout()) {
            column->setContentsMargins(alert?24:22,alert?12:20,alert?24:22,alert?20:18);
            column->setSpacing(alert?12:0);
        }
    dialog.setFixedSize(alert?524:300,alert?260:160);
    dialog.exec();
}
}

void showNotice(QWidget *parent,const QString &heading,const QString &detail)
{
    showMessageCard(parent,heading,detail,false);
}

void showAlert(QWidget *parent,const QString &heading,const QString &detail)
{
    showMessageCard(parent,heading,detail,true);
}

namespace {
QIcon colorSwatchIcon(const QColor &color)
{
    QPixmap pixmap(22,22);pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    if(color.isValid()){
        painter.setPen(QPen(QColor("#777980"),1));painter.setBrush(color);
        painter.drawRect(QRectF(0.5,0.5,21,21));
    }else{
        painter.setPen(QPen(QColor("#4b4b4b"),1,Qt::DashLine));painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(1.5,1.5,19,19));
    }
    return QIcon(pixmap);
}
}

ColorDialog::ColorDialog(const QColor &initial,QWidget *parent):QColorDialog(initial,parent)
{
    const auto text=[](const char *source){return QCoreApplication::translate("CaptureOverlay",source);};
    setOption(DontUseNativeDialog);setOption(ShowAlphaChannel);setOption(NoButtons);
    setWindowTitle(text("选择颜色"));
    auto *native=new QWidget(this);native->setObjectName(QStringLiteral("nativeColorControls"));
    if(layout())native->setLayout(layout());
    native->hide();
    auto *scroll=new QScrollArea(this);scroll->setObjectName(QStringLiteral("colorScroll"));
    scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content=new QWidget;
    auto *root=new QVBoxLayout(content);root->setContentsMargins(0,0,0,0);root->setSpacing(18);
    auto *fields=new QHBoxLayout;fields->setSpacing(28);
    auto section=[this](const QString &value){
        auto *label=new QLabel(value,this);label->setStyleSheet(QStringLiteral("color:#d0d6e0;font-size:13px;background:transparent;"));
        return label;
    };
    auto tiny=[](const QString &value){
        auto *label=new QLabel(value);label->setAlignment(Qt::AlignHCenter);
        label->setStyleSheet(QStringLiteral("color:#9c9da1;font-size:12px;background:transparent;"));
        return label;
    };
    auto *left=new QVBoxLayout;left->setSpacing(14);
    auto *plane=new ColorPlane(this);plane->setObjectName(QStringLiteral("colorPlane"));plane->setFixedHeight(340);left->addWidget(plane);
    auto *hue=new QSlider(Qt::Horizontal,this);hue->setObjectName(QStringLiteral("colorHue"));hue->setRange(0,359);hue->setFixedHeight(20);
    hue->setStyleSheet(QStringLiteral("QSlider::groove:horizontal{height:12px;border-radius:6px;background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #ff0000,stop:.17 #ffff00,stop:.33 #00ff00,stop:.5 #00ffff,stop:.67 #0000ff,stop:.83 #ff00ff,stop:1 #ff0000);}QSlider::handle:horizontal{width:10px;margin:-2px 0;background:white;border:1px solid #9c9da1;border-radius:4px;}"));left->addWidget(hue);
    auto *pick=new QPushButton(text("屏幕取色"),this);
    pick->setObjectName(QStringLiteral("colorPickScreen"));
    pick->setIcon(makeIcon(Icon::Picker,20,true));pick->setIconSize(QSize(20,20));
    pick->setFixedHeight(32);pick->setCursor(Qt::PointingHandCursor);
    pick->setAccessibleName(text("屏幕取色"));pick->setToolTip(text("从冻结画面取色，Esc 取消"));
    connect(pick,&QPushButton::clicked,this,&ColorDialog::screenPickRequested);
    left->addWidget(pick,0,Qt::AlignLeft);left->addStretch();
    fields->addLayout(left,1);

    auto *form=new QVBoxLayout;form->setSpacing(10);
    auto *hex=new QLineEdit(this);hex->setObjectName(QStringLiteral("colorHex"));hex->setFixedHeight(34);hex->setMaxLength(9);
    form->addWidget(section(QStringLiteral("HEX")));form->addWidget(hex);
    QList<QSpinBox *> channels;
    const char *channelNames[]={"R","G","B"};
    const char *channelObjects[]={"R","G","B"};
    const char *channelAccessible[]={"红色","绿色","蓝色"};
    auto *rgbGrid=new QGridLayout;rgbGrid->setHorizontalSpacing(6);rgbGrid->setVerticalSpacing(3);
    for(int i=0;i<3;++i){
        rgbGrid->addWidget(tiny(QString::fromLatin1(channelNames[i])),0,i);
        auto *spin=new QSpinBox(this);spin->setObjectName(QStringLiteral("color")+QString::fromLatin1(channelObjects[i]));
        spin->setRange(0,255);spin->setButtonSymbols(QAbstractSpinBox::NoButtons);spin->setKeyboardTracking(false);
        spin->setAlignment(Qt::AlignCenter);spin->setFixedHeight(32);
        spin->setAccessibleName(text(channelAccessible[i]));channels.append(spin);rgbGrid->addWidget(spin,1,i);
    }
    form->addWidget(section(QStringLiteral("RGB")));form->addLayout(rgbGrid);
    QList<QSpinBox *> hsv;
    const char *hsvNames[]={"H","S","V"};
    const char *hsvAccessible[]={"色相","饱和度","明度"};
    auto *hsvGrid=new QGridLayout;hsvGrid->setHorizontalSpacing(6);hsvGrid->setVerticalSpacing(3);
    for(int i=0;i<3;++i){
        hsvGrid->addWidget(tiny(QString::fromLatin1(hsvNames[i])),0,i);
        auto *spin=new QSpinBox(this);spin->setObjectName(QStringLiteral("color")+QString::fromLatin1(hsvNames[i]));
        spin->setRange(0,i==0?359:100);spin->setButtonSymbols(QAbstractSpinBox::NoButtons);spin->setKeyboardTracking(false);
        spin->setAlignment(Qt::AlignCenter);spin->setFixedHeight(32);
        spin->setAccessibleName(text(hsvAccessible[i]));hsv.append(spin);hsvGrid->addWidget(spin,1,i);
    }
    form->addWidget(section(QStringLiteral("HSV")));form->addLayout(hsvGrid);
    auto *alphaSpin=new QSpinBox(this);alphaSpin->setObjectName(QStringLiteral("colorAlpha"));
    alphaSpin->setRange(0,255);alphaSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);alphaSpin->setKeyboardTracking(false);
    alphaSpin->setAlignment(Qt::AlignCenter);alphaSpin->setFixedSize(56,32);
    alphaSpin->setAccessibleName(text("不透明度"));alphaSpin->setToolTip(text("不透明度：0 为全透明，255 为不透明"));
    channels.append(alphaSpin);
    auto *opacity=new QSlider(Qt::Horizontal,this);opacity->setObjectName(QStringLiteral("colorOpacity"));
    opacity->setRange(0,255);opacity->setFixedHeight(20);opacity->setStyleSheet(darkSliderQss);
    opacity->setAccessibleName(text("不透明度"));opacity->setToolTip(text("不透明度：0 为全透明，255 为不透明"));
    form->addWidget(section(text("不透明度")));
    auto *alphaRow=new QHBoxLayout;alphaRow->setSpacing(8);
    alphaRow->addWidget(opacity,1);alphaRow->addWidget(alphaSpin);form->addLayout(alphaRow);

    auto *customHead=new QHBoxLayout;customHead->setSpacing(8);
    customHead->addWidget(section(text("自定义颜色")));customHead->addStretch();
    auto *add=new QPushButton(text("保存当前颜色"),this);
    add->setObjectName(QStringLiteral("colorAddCustom"));add->setFixedHeight(28);add->setCursor(Qt::PointingHandCursor);
    add->setAccessibleName(text("保存当前颜色"));add->setToolTip(text("将当前颜色保存到自定义颜色"));
    customHead->addWidget(add);form->addLayout(customHead);
    auto *customRow=new QHBoxLayout;customRow->setSpacing(2);
    QVector<QToolButton *> customButtons;
    for(int i=0;i<8;++i){
        auto *button=new QToolButton(this);button->setObjectName(QStringLiteral("colorCustom%1").arg(i));
        button->setFixedSize(28,28);button->setIconSize(QSize(22,22));button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);button->setFocusPolicy(Qt::StrongFocus);
        button->setProperty("colorSwatch",true);customRow->addWidget(button);customButtons.append(button);
    }
    customRow->addStretch();form->addLayout(customRow);form->addStretch();
    auto *fieldWidget=new QWidget(this);fieldWidget->setLayout(form);fieldWidget->setFixedWidth(238);fields->addWidget(fieldWidget);
    root->addLayout(fields);root->addStretch();

    auto colors=std::make_shared<QVector<QColor>>();
    for(const QString &value:LensSnipSettings().value(QStringLiteral("Capture/customColors")).toStringList()){
        const QColor color(value);
        if(color.isValid())colors->append(color);
    }
    const auto refreshCustom=[colors,customButtons]{
        for(int i=0;i<customButtons.size();++i){
            const bool has=i<colors->size();
            customButtons[i]->setIcon(colorSwatchIcon(has?colors->at(i):QColor()));
            customButtons[i]->setToolTip(has?colors->at(i).name(QColor::HexArgb).toUpper():QString());
            customButtons[i]->setAccessibleName(has
                ?QCoreApplication::translate("CaptureOverlay","自定义颜色 %1").arg(colors->at(i).name(QColor::HexArgb).toUpper())
                :QString());
            customButtons[i]->setEnabled(has);
        }
    };
    refreshCustom();
    for(int i=0;i<customButtons.size();++i)
        connect(customButtons[i],&QToolButton::clicked,this,[this,colors,i]{if(i<colors->size())setCurrentColor(colors->at(i));});
    connect(add,&QPushButton::clicked,this,[this,colors,refreshCustom]{
        const QColor color=currentColor();
        colors->removeAll(color);colors->prepend(color);
        while(colors->size()>8)colors->removeLast();
        QStringList values;
        for(const QColor &entry:*colors)values<<entry.name(QColor::HexArgb);
        LensSnipSettings().setValue(QStringLiteral("Capture/customColors"),values);
        refreshCustom();
    });

    auto *buttons=new QHBoxLayout;buttons->setSpacing(12);buttons->addStretch();
    auto *cancel=new QPushButton(QCoreApplication::translate("QPlatformTheme","Cancel"),this);
    auto *ok=new QPushButton(QCoreApplication::translate("QPlatformTheme","OK"),this);
    cancel->setFixedSize(88,32);ok->setFixedSize(88,32);buttons->addWidget(cancel);buttons->addWidget(ok);
    connect(ok,&QPushButton::clicked,this,&QDialog::accept);connect(cancel,&QPushButton::clicked,this,&QDialog::reject);
    ok->setDefault(true);
    const auto refresh=[this,plane,hue,hex,channels,hsv,opacity](const QColor &color){
        plane->color=color;plane->update();
        const QSignalBlocker h(hue),x(hex);hue->setValue(qMax(0,color.hsvHue()));hex->setText(color.name(QColor::HexRgb).toUpper());
        const int values[]={color.red(),color.green(),color.blue(),color.alpha()};
        for(int i=0;i<channels.size();++i){const QSignalBlocker block(channels[i]);channels[i]->setValue(values[i]);}
        const QSignalBlocker opacityBlock(opacity);opacity->setValue(color.alpha());
        const int hsvValues[]={qMax(0,color.hsvHue()),qRound(color.hsvSaturationF()*100),qRound(color.valueF()*100)};
        for(int i=0;i<hsv.size();++i){const QSignalBlocker block(hsv[i]);hsv[i]->setValue(hsvValues[i]);}
    };
    connect(this,&QColorDialog::currentColorChanged,this,refresh);
    connect(hex,&QLineEdit::editingFinished,this,[this,hex]{QColor next(hex->text());if(next.isValid()){next.setAlpha(currentColor().alpha());setCurrentColor(next);}});
    for(auto *spin:channels)connect(spin,QOverload<int>::of(&QSpinBox::valueChanged),this,[this,channels]{setCurrentColor(QColor(channels[0]->value(),channels[1]->value(),channels[2]->value(),channels[3]->value()));});
    connect(opacity,&QSlider::valueChanged,alphaSpin,&QSpinBox::setValue);
    connect(alphaSpin,QOverload<int>::of(&QSpinBox::valueChanged),opacity,&QSlider::setValue);
    for(auto *spin:hsv)connect(spin,QOverload<int>::of(&QSpinBox::valueChanged),this,[this,hsv]{setCurrentColor(QColor::fromHsv(hsv[0]->value(),qRound(hsv[1]->value()*2.55),qRound(hsv[2]->value()*2.55),currentColor().alpha()));});
    connect(hue,&QSlider::valueChanged,this,[this](int value){const QColor c=currentColor();setCurrentColor(QColor::fromHsv(value,c.hsvSaturation(),c.value(),c.alpha()));});
    plane->picked=[this,hue](qreal saturation,qreal value){setCurrentColor(QColor::fromHsvF(hue->value()/360.0,saturation,value,currentColor().alphaF()));};
    setStyleSheet(QStringLiteral("QLabel{background:transparent;border:none;}"
                                 "QLineEdit{background:#111216;color:#f4f5f8;border:1px solid #777980;border-radius:8px;padding:0 10px;font-size:14px;}"
                                 "QSpinBox{background:#111216;color:#f4f5f8;border:1px solid #777980;border-radius:8px;padding:0;font-size:14px;}"
                                 "QSpinBox QLineEdit{background:transparent;border:none;padding:0 2px;}"
                                 "QLineEdit:focus,QSpinBox:focus{border:2px solid #828fff;padding:0 9px;}"
                                 "QSpinBox:focus{padding:0;}"
                                 "QSpinBox QLineEdit:focus{border:none;padding:0 2px;}"
                                 "QPushButton#colorPickScreen,QPushButton#colorAddCustom{background:transparent;color:#f4f5f8;border:1px solid #777980;border-radius:8px;padding:0 12px;font-size:13px;}"
                                 "QPushButton#colorPickScreen:hover,QPushButton#colorAddCustom:hover{background:#222326;}"
                                 "QPushButton#colorPickScreen:focus,QPushButton#colorAddCustom:focus{border:2px solid #828fff;padding:0 11px;}"
                                 "QToolButton[colorSwatch=\"true\"]{background:transparent;border:none;border-radius:6px;}"
                                 "QToolButton[colorSwatch=\"true\"]:hover{background:#222326;}"
                                 "QToolButton[colorSwatch=\"true\"]:focus{border:2px solid #828fff;}"
                                 "QScrollArea{background:transparent;border:none;}"
                                 "QScrollArea>QWidget>QWidget{background:transparent;}"));
    scroll->setWidget(content);
    auto *shell=new QVBoxLayout(this);shell->setContentsMargins(0,0,0,0);shell->setSpacing(12);
    shell->addWidget(scroll);shell->addLayout(buttons);
    refresh(initial);styleDialog(this);
    const QScreen *target=QGuiApplication::screenAt(QCursor::pos());
    if(!target)target=QGuiApplication::primaryScreen();
    const QSize avail=target?target->availableGeometry().size()-QSize(32,32):QSize(820,600);
    const QSize initialSize(qMin(820,avail.width()),qMin(600,avail.height()));
    const qreal scale=target?target->devicePixelRatio():1.0;
    const bool compact=initialSize.height()<560||scale>=1.5;
    plane->setFixedHeight(compact?qBound(160,initialSize.height()-340,300):340);
    if(compact){
        form->setSpacing(6);hex->setFixedHeight(28);
        for(QSpinBox *spin:channels)spin->setFixedHeight(28);
        for(QSpinBox *spin:hsv)spin->setFixedHeight(28);
    }
    setMinimumSize(qMin(640,initialSize.width()),qMin(420,initialSize.height()));
    resize(initialSize);
}
