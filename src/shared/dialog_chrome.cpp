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
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QSpinBox>
#include <QStyleOptionSpinBox>
#include <QStyleOptionSlider>
#include <QFocusEvent>
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
    explicit ColorPlane(QWidget *parent):QWidget(parent) {
        setMinimumSize(160,160);setCursor(Qt::CrossCursor);setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QCoreApplication::translate("CaptureOverlay","饱和度与明度"));
    }
    QColor color;
    std::function<void(qreal,qreal)> picked;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;clip.addRoundedRect(QRectF(rect()),6,6);p.setClipPath(clip);
        p.fillRect(rect(),QColor::fromHsvF(qMax(0.0,color.hsvHueF()),1,1));
        QLinearGradient white(0,0,qMax(1,width()-1),0);white.setColorAt(0,Qt::white);white.setColorAt(1,QColor(255,255,255,0));p.fillRect(rect(),white);
        // Reach opaque black before the final pixel center, including fractional DPRs.
        QLinearGradient black(0,0,0,qMax(1,height()-1));black.setColorAt(0,QColor(0,0,0,0));black.setColorAt(1,Qt::black);p.fillRect(rect(),black);
        const QPointF marker(color.hsvSaturationF()*(width()-1),(1-color.valueF())*(height()-1));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor("#18191d"),4));p.drawEllipse(marker,7,7);
        p.setPen(QPen(Qt::white,2));p.drawEllipse(marker,6,6);
        if(property("kbdFocus").toBool()) {p.setPen(QPen(QColor("#c5c7ce"),2));p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),5,5);}
    }
    void mousePressEvent(QMouseEvent *e) override {if(e->button()==Qt::LeftButton) pick(e->position());}
    void mouseMoveEvent(QMouseEvent *e) override {if(e->buttons()&Qt::LeftButton) pick(e->position());}
    void keyPressEvent(QKeyEvent *e) override {
        qreal s=color.hsvSaturationF(),v=color.valueF();
        switch(e->key()) {
        case Qt::Key_Left:s-=0.01;break;case Qt::Key_Right:s+=0.01;break;
        case Qt::Key_Up:v+=0.01;break;case Qt::Key_Down:v-=0.01;break;
        default:QWidget::keyPressEvent(e);return;
        }
        if(picked)picked(qBound(0.0,s,1.0),qBound(0.0,v,1.0));
    }
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
QIcon colorSwatchIcon(const QColor &color,qreal dpr,bool selected=false)
{
    QPixmap pixmap(qCeil(32*dpr),qCeil(32*dpr));pixmap.setDevicePixelRatio(dpr);pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    if(color.isValid()){
        for(int y=0;y<32;y+=4)for(int x=0;x<32;x+=4)
            painter.fillRect(x,y,4,4,(x/4+y/4)%2?QColor("#979aa3"):QColor("#d7d8dc"));
        painter.fillRect(QRect(0,0,32,32),color);
        painter.setPen(QPen(QColor("#777980"),1));painter.setBrush(Qt::NoBrush);painter.drawRect(QRectF(0.5,0.5,31,31));
        if(selected){
            painter.setRenderHint(QPainter::Antialiasing);
            const QPolygonF tick{QPointF(10,16),QPointF(14,20),QPointF(22,12)};
            painter.setPen(QPen(QColor(0,0,0,120),4));painter.drawPolyline(tick);
            painter.setPen(QPen(Qt::white,2));painter.drawPolyline(tick);
        }
    }else{
        painter.setPen(QPen(QColor("#4b4b4b"),1,Qt::DashLine));painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(0.5,0.5,31,31));
    }
    return QIcon(pixmap);
}

class PaletteFocus : public QObject
{
public:
    PaletteFocus(QWidget *control,QObject *parent):QObject(parent),m_control(control) {}
    bool eventFilter(QObject *,QEvent *event) override {
        if(event->type()==QEvent::FocusIn||event->type()==QEvent::FocusOut||event->type()==QEvent::MouseButtonPress){
            bool keyboard=false;
            if(event->type()==QEvent::FocusIn){
                const auto reason=static_cast<QFocusEvent *>(event)->reason();
                keyboard=reason==Qt::TabFocusReason||reason==Qt::BacktabFocusReason||reason==Qt::ShortcutFocusReason;
            }
            m_control->setProperty("kbdFocus",keyboard);
            m_control->style()->unpolish(m_control);m_control->style()->polish(m_control);m_control->update();
        }
        return false;
    }
private:
    QWidget *m_control;
};

class PaletteSpin : public QSpinBox
{
public:
    PaletteSpin(const QString &letter,QWidget *parent):QSpinBox(parent),m_letter(letter) {
        setKeyboardTracking(false);setAlignment(Qt::AlignCenter);setFixedHeight(36);
        setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
        setProperty("channelLetter",!letter.isEmpty());
    }
protected:
    void paintEvent(QPaintEvent *event) override {
        QSpinBox::paintEvent(event);
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        QFont label=font();label.setPixelSize(14);p.setFont(label);p.setPen(QColor("#b1b3bc"));
        p.drawText(QRect(6,0,12,height()),Qt::AlignCenter,m_letter);
        QStyleOptionSpinBox option;initStyleOption(&option);
        for(const auto control:{QStyle::SC_SpinBoxUp,QStyle::SC_SpinBoxDown}){
            const QRect r=style()->subControlRect(QStyle::CC_SpinBox,&option,control,this);
            const bool up=control==QStyle::SC_SpinBoxUp;
            const QPointF c=r.center();const qreal direction=up?1:-1;
            p.setPen(QPen(r.contains(mapFromGlobal(QCursor::pos()))?QColor("#b1b3bc"):QColor("#777980"),1.2));
            p.drawPolyline(QPolygonF{c+QPointF(-2.5,direction),c+QPointF(0,-direction*1.5),c+QPointF(2.5,direction)});
        }
    }
private:
    QString m_letter;
};

class PaletteSlider : public QSlider
{
public:
    PaletteSlider(bool hue,QWidget *parent):QSlider(Qt::Horizontal,parent),m_hue(hue) {
        setFixedHeight(32);setRange(0,hue?359:255);
        setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
        setStyleSheet(QStringLiteral("QSlider::groove:horizontal{height:12px;}QSlider::handle:horizontal{width:10px;margin:-4px 0;}"));
    }
    QColor color;
protected:
    void paintEvent(QPaintEvent *) override {
        QStyleOptionSlider option;initStyleOption(&option);
        const QRect handle=style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,this);
        const QRectF track(1,(height()-12)/2.0,width()-2,12);
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;clip.addRoundedRect(track,4,4);p.setClipPath(clip);
        QLinearGradient gradient(track.left(),0,track.right(),0);
        if(m_hue){
            for(int i=0;i<=6;++i)gradient.setColorAt(i/6.0,QColor::fromHsv(i==6?0:i*60,255,255));
        }else{
            for(int y=0;y<height();y+=4)for(int x=0;x<width();x+=4)
                p.fillRect(x,y,4,4,(x/4+y/4)%2?QColor("#979aa3"):QColor("#d7d8dc"));
            QColor transparent=color;transparent.setAlpha(0);QColor opaque=color;opaque.setAlpha(255);
            gradient.setColorAt(0,transparent);gradient.setColorAt(1,opaque);
        }
        p.fillRect(track,gradient);p.setClipping(false);
        const qreal diameter=14,radius=diameter/2;
        const qreal center=qBound(radius,qreal(handle.center().x()),qMax(radius,width()-radius));
        const QRectF thumb(center-radius,(height()-diameter)/2.0,diameter,diameter);
        p.setPen(QPen(QColor(0,0,0,140),3));p.setBrush(Qt::white);p.drawEllipse(thumb);
        if(property("kbdFocus").toBool()){p.setPen(QPen(QColor("#c5c7ce"),2));p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),5,5);}
    }
private:
    bool m_hue;
};
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
    auto *root=new QVBoxLayout(content);root->setContentsMargins(0,0,0,0);root->setSpacing(20);
    auto *fields=new QHBoxLayout;fields->setSpacing(24);
    auto section=[this](const QString &value){
        auto *label=new QLabel(value,this);label->setFixedHeight(16);
        label->setStyleSheet(QStringLiteral("color:#ffffff;font-size:12px;font-weight:600;background:transparent;"));
        return label;
    };
    auto *left=new QVBoxLayout;left->setSpacing(12);
    auto *plane=new ColorPlane(this);plane->setObjectName(QStringLiteral("colorPlane"));plane->setFixedSize(256,256);left->addWidget(plane);
    auto *hue=new PaletteSlider(true,this);hue->setObjectName(QStringLiteral("colorHue"));hue->setAccessibleName(text("色相"));
    auto *pick=new QPushButton(this);
    pick->setObjectName(QStringLiteral("colorPickScreen"));
    pick->setIcon(subtleIcon(Icon::Picker,18));pick->setIconSize(QSize(18,18));
    pick->setFixedSize(36,32);pick->setCursor(Qt::PointingHandCursor);
    pick->setAccessibleName(text("屏幕取色"));pick->setToolTip(text("从冻结画面取色，Esc 取消"));
    connect(pick,&QPushButton::clicked,this,&ColorDialog::screenPickRequested);
    auto *hueRow=new QHBoxLayout;hueRow->setSpacing(12);hueRow->addWidget(pick);hueRow->addWidget(hue,1);
    left->addLayout(hueRow);

    auto *form=new QVBoxLayout;form->setSpacing(8);
    auto *hex=new QLineEdit(this);hex->setObjectName(QStringLiteral("colorHex"));hex->setFixedHeight(36);hex->setMaxLength(9);
    hex->setAlignment(Qt::AlignCenter);hex->setAccessibleName(QStringLiteral("HEX"));hex->setTextMargins(40,0,40,0);
    auto *preview=new QLabel(hex);preview->setObjectName(QStringLiteral("colorPreview"));preview->setGeometry(10,7,22,22);
    preview->setAttribute(Qt::WA_TransparentForMouseEvents);
    form->addWidget(section(QStringLiteral("HEX")));form->addWidget(hex);
    QList<QSpinBox *> channels;
    const char *channelNames[]={"R","G","B"};
    const char *channelObjects[]={"R","G","B"};
    const char *channelAccessible[]={"红色","绿色","蓝色"};
    auto *rgbGrid=new QHBoxLayout;rgbGrid->setSpacing(8);
    for(int i=0;i<3;++i){
        auto *spin=new PaletteSpin(QString::fromLatin1(channelNames[i]),this);spin->setObjectName(QStringLiteral("color")+QString::fromLatin1(channelObjects[i]));
        spin->setRange(0,255);spin->setAccessibleName(text(channelAccessible[i]));channels.append(spin);rgbGrid->addWidget(spin,1);
    }
    form->addSpacing(8);form->addWidget(section(QStringLiteral("RGB")));form->addLayout(rgbGrid);
    QList<QSpinBox *> hsv;
    const char *hsvNames[]={"H","S","V"};
    const char *hsvAccessible[]={"色相","饱和度","明度"};
    auto *hsvGrid=new QHBoxLayout;hsvGrid->setSpacing(8);
    for(int i=0;i<3;++i){
        auto *spin=new PaletteSpin(QString::fromLatin1(hsvNames[i]),this);spin->setObjectName(QStringLiteral("color")+QString::fromLatin1(hsvNames[i]));
        spin->setRange(0,i==0?359:100);spin->setAccessibleName(text(hsvAccessible[i]));hsv.append(spin);hsvGrid->addWidget(spin,1);
    }
    form->addSpacing(8);form->addWidget(section(QStringLiteral("HSV")));form->addLayout(hsvGrid);
    auto *alphaSpin=new PaletteSpin(QString(),this);alphaSpin->setObjectName(QStringLiteral("colorAlpha"));
    alphaSpin->setRange(0,255);alphaSpin->setFixedSize(72,32);alphaSpin->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    alphaSpin->setAccessibleName(text("不透明度"));alphaSpin->setToolTip(text("不透明度：0 为全透明，255 为不透明"));
    channels.append(alphaSpin);
    auto *opacity=new PaletteSlider(false,this);opacity->setObjectName(QStringLiteral("colorOpacity"));
    opacity->setAccessibleName(text("不透明度"));opacity->setToolTip(text("不透明度：0 为全透明，255 为不透明"));
    form->addSpacing(8);form->addWidget(section(text("不透明度")));form->addStretch();
    auto *fieldWidget=new QWidget(this);fieldWidget->setLayout(form);form->setContentsMargins(0,0,0,0);

    auto *alphaRow=new QHBoxLayout;alphaRow->setSpacing(12);
    alphaRow->addWidget(opacity,1);alphaRow->addWidget(alphaSpin);
    auto *right=new QVBoxLayout;right->setSpacing(12);
    right->addWidget(fieldWidget,1);right->addLayout(alphaRow);
    fields->addLayout(left);fields->addLayout(right,1);root->addLayout(fields);
    auto *custom=new QVBoxLayout;
    auto *add=new QPushButton(text("保存"),this);
    add->setIcon(subtleIcon(Icon::Save,16));add->setIconSize(QSize(16,16));
    add->setObjectName(QStringLiteral("colorAddCustom"));add->setFixedSize(72,32);add->setCursor(Qt::PointingHandCursor);
    add->setAccessibleName(text("保存当前颜色"));add->setToolTip(text("将当前颜色保存到自定义颜色"));
    auto *reset=new QPushButton(text("重置"),this);reset->setObjectName(QStringLiteral("colorResetCustom"));
    reset->setIcon(subtleIcon(Icon::Refresh,16));reset->setIconSize(QSize(16,16));reset->setFixedSize(72,32);
    reset->setAccessibleName(text("重置自定义颜色"));reset->setToolTip(text("恢复打开时的自定义颜色"));
    auto *customRow=new QHBoxLayout;customRow->setSpacing(20);
    QVector<QToolButton *> customButtons;
    for(int i=0;i<6;++i){
        auto *button=new QToolButton(this);button->setObjectName(QStringLiteral("colorCustom%1").arg(i));
        button->setFixedSize(32,32);button->setIconSize(QSize(32,32));button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);button->setFocusPolicy(Qt::StrongFocus);
        button->setProperty("colorSwatch",true);customRow->addWidget(button);customButtons.append(button);
    }
    customRow->addStretch();
    auto *actions=new QHBoxLayout;actions->setSpacing(8);actions->addWidget(add);actions->addWidget(reset);customRow->addLayout(actions);
    custom->addLayout(customRow);root->addLayout(custom);root->addStretch();

    auto colors=std::make_shared<QVector<QColor>>();
    for(const QString &value:LensSnipSettings().value(QStringLiteral("Capture/customColors")).toStringList()){
        const QColor color(value);
        if(color.isValid())colors->append(color);
    }
    if(colors->isEmpty())for(const char *hex:{"#d3cec7","#c46b0b","#a63e4c","#d11ee7","#111216","#806647"})colors->append(QColor(hex));
    const QVector<QColor> openedColors=colors->mid(0,6);
    const auto refreshCustom=[this,colors,customButtons]{
        for(int i=0;i<customButtons.size();++i){
            const bool has=i<colors->size();
            customButtons[i]->setIcon(colorSwatchIcon(has?colors->at(i):QColor(),devicePixelRatioF(),has&&colors->at(i)==currentColor()));
            customButtons[i]->setToolTip(has?colors->at(i).name(QColor::HexArgb).toUpper():QString());
            customButtons[i]->setAccessibleName(has
                ?QCoreApplication::translate("CaptureOverlay","自定义颜色 %1").arg(colors->at(i).name(QColor::HexArgb).toUpper())
                :QString());
            customButtons[i]->setEnabled(has);
        }
    };
    const auto persistCustom=[colors,refreshCustom]{
        while(colors->size()>6)colors->removeLast();
        QStringList values;
        for(const QColor &entry:*colors)values<<entry.name(QColor::HexArgb);
        LensSnipSettings().setValue(QStringLiteral("Capture/customColors"),values);
        refreshCustom();
    };
    refreshCustom();
    for(int i=0;i<customButtons.size();++i)
        connect(customButtons[i],&QToolButton::clicked,this,[this,colors,i]{if(i<colors->size())setCurrentColor(colors->at(i));});
    connect(add,&QPushButton::clicked,this,[this,colors,persistCustom]{
        const QColor color=currentColor();
        colors->removeAll(color);colors->prepend(color);
        persistCustom();
    });
    connect(reset,&QPushButton::clicked,this,[colors,openedColors,persistCustom]{*colors=openedColors;persistCustom();});

    auto *buttons=new QHBoxLayout;buttons->setSpacing(8);buttons->addStretch();
    auto *cancel=new QPushButton(QCoreApplication::translate("QPlatformTheme","Cancel"),this);
    auto *ok=new QPushButton(QCoreApplication::translate("QPlatformTheme","OK"),this);
    cancel->setObjectName(QStringLiteral("colorCancel"));ok->setObjectName(QStringLiteral("colorAccept"));
    cancel->setFixedSize(76,32);ok->setFixedSize(88,32);buttons->addWidget(cancel);buttons->addWidget(ok);
    connect(ok,&QPushButton::clicked,this,&QDialog::accept);connect(cancel,&QPushButton::clicked,this,&QDialog::reject);
    ok->setDefault(true);
    const auto refresh=[this,plane,hue,hex,preview,channels,hsv,opacity,refreshCustom](const QColor &color){
        plane->color=color;plane->update();
        opacity->color=color;opacity->update();preview->setPixmap(colorSwatchIcon(color,devicePixelRatioF()).pixmap(QSize(22,22),devicePixelRatioF()));
        const QSignalBlocker h(hue),x(hex);hue->setValue(qMax(0,color.hsvHue()));hex->setText(color.name(QColor::HexRgb).toUpper());
        const int values[]={color.red(),color.green(),color.blue(),color.alpha()};
        for(int i=0;i<channels.size();++i){const QSignalBlocker block(channels[i]);channels[i]->setValue(values[i]);}
        const QSignalBlocker opacityBlock(opacity);opacity->setValue(color.alpha());
        const int hsvValues[]={qMax(0,color.hsvHue()),qRound(color.hsvSaturationF()*100),qRound(color.valueF()*100)};
        for(int i=0;i<hsv.size();++i){const QSignalBlocker block(hsv[i]);hsv[i]->setValue(hsvValues[i]);}
        refreshCustom();
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
                                 "QLineEdit{background:#111216;color:#f4f5f8;border:1px solid #777980;border-radius:6px;padding:0;font-size:14px;}"
                                 "QSpinBox{background:#111216;color:#f4f5f8;border:1px solid #777980;border-radius:6px;padding:0 0 0 4px;font-size:13px;}"
                                 "QSpinBox[channelLetter=\"true\"]{padding-left:18px;}"
                                 "QSpinBox QLineEdit{background:transparent;border:none;padding:0 2px;}"
                                 "QSpinBox::up-button{image:none;subcontrol-origin:border;subcontrol-position:top right;width:16px;height:17px;border:none;background:transparent;}"
                                 "QSpinBox::down-button{image:none;subcontrol-origin:border;subcontrol-position:bottom right;width:16px;height:17px;border:none;background:transparent;}"
                                 "QSpinBox::up-arrow,QSpinBox::down-arrow{image:none;}"
                                 "QSpinBox::up-button:hover,QSpinBox::down-button:hover{background:#222326;}"
                                 "QPushButton{background:transparent;color:#b1b3bc;border:none;border-radius:6px;padding:0;font-size:13px;text-align:center;}"
                                 "QPushButton:hover{background:#222326;}QPushButton:pressed{background:#2b2d33;}"
                                 "QPushButton:focus,QPushButton:default{border:none;}"
                                 "QPushButton#colorAccept{background:#e5e5e6;color:#09090a;font-weight:600;}"
                                 "QPushButton#colorAccept:hover{background:white;}QPushButton#colorAccept:pressed{background:#d0d6e0;}"
                                 "QPushButton#colorPickScreen,QPushButton#colorAddCustom,QPushButton#colorResetCustom{border:1px solid #777980;}"
                                 "QToolButton[colorSwatch=\"true\"]{background:transparent;border:none;border-radius:0;padding:0;}"
                                 "QToolButton[colorSwatch=\"true\"]:hover{border:1px solid #f4f5f8;}"
                                 "QWidget[kbdFocus=\"true\"]{border:2px solid #c5c7ce;}"
                                 "QSpinBox QLineEdit[kbdFocus=\"true\"]{border:none;}"
                                 "QScrollArea{background:transparent;border:none;}"
                                 "QScrollArea>QWidget>QWidget{background:transparent;}"));
    scroll->setWidget(content);
    auto *shell=new QVBoxLayout(this);shell->setContentsMargins(0,0,0,0);shell->setSpacing(20);
    shell->addWidget(scroll);shell->addLayout(buttons);
    refresh(initial);styleDialog(this);
    if(auto *surface=findChild<QWidget *>(QStringLiteral("dialogSurface")))
        if(auto *column=qobject_cast<QVBoxLayout *>(surface->layout())){
            column->setContentsMargins(24,20,24,24);column->setSpacing(20);
            if(QWidget *title=column->itemAt(0)->widget())if(auto *label=title->findChild<QLabel *>())
                label->setStyleSheet(QStringLiteral("color:#f4f5f8;font-size:16px;font-weight:600;background:transparent;"));
        }
    QList<QWidget *> controls{plane,pick,hue,hex};
    for(QSpinBox *spin:channels)if(spin!=alphaSpin)controls.append(spin);
    for(QSpinBox *spin:hsv)controls.append(spin);
    controls<<opacity<<alphaSpin;
    for(QToolButton *button:customButtons)controls.append(button);
    controls<<add<<reset<<cancel<<ok;
    if(auto *close=findChild<QToolButton *>(QStringLiteral("dialogClose")))controls.append(close);
    for(int i=0;i<controls.size();++i){
        QWidget *control=controls[i];auto *filter=new PaletteFocus(control,this);control->installEventFilter(filter);
        if(auto *spin=qobject_cast<QSpinBox *>(control))spin->findChild<QLineEdit *>()->installEventFilter(filter);
        if(i)QWidget::setTabOrder(controls[i-1],control);
    }
    hex->setFocus(Qt::OtherFocusReason);
    const QScreen *target=QGuiApplication::screenAt(QCursor::pos());
    if(!target)target=QGuiApplication::primaryScreen();
    const QSize avail=target?target->availableGeometry().size()-QSize(32,32):QSize(560,532);
    setMinimumSize(qMin(560,avail.width()),qMin(532,avail.height()));
    resize(qMin(560,avail.width()),qMin(532,avail.height()));
}

void ColorDialog::showEvent(QShowEvent *event)
{
    QColorDialog::showEvent(event);
    if(m_centered)return;
    m_centered=true;
    const QScreen *screen=QGuiApplication::screenAt(QCursor::pos());
    if(!screen)screen=QGuiApplication::primaryScreen();
    if(screen)move(screen->availableGeometry().center()-rect().center());
}
