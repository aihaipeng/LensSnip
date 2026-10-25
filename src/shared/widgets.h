#pragma once

#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QIcon>
#include <QColor>
#include <QPaintEvent>
#include <QWidget>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QStyledItemDelegate>
#include <qt_windows.h>

// ---------- Shared icon set for all buttons. ----------
// Sharp-cornered SVG glyphs compiled in from icons/icons.qrc; the enum order matches
// the file-name table in makeIcon.

enum class Icon { Save, Copy, Pin, LongShot, Ocr, Cancel, Rect, Ellipse, Arrow, Pen, Marker, Text,
                  Number, Mosaic, Eraser, Undo, Redo, Bold, Italic, Minimize, Maximize, Restore, Refresh,
                  Fill, LineWidth, MarkerRect, FitView, Search, TextFormat, SplitColumns, SplitRows,
                  ChevronUp, ChevronDown, ChevronRight, WavyLine, Underline, Strikethrough, Check, Translate, View,
                  CardFit, CardTranslate, CardReload, CardCopy, CardZoomIn,
                  CardZoomOut, CardActual, CardPin, CardMinimize, CardMaximize, CardRestore, CardClose, CardSearch,
                  CardUp, CardDown, CardRight, CardFormat, Back, Crop, CropStart, CropEnd, Barcode, CardLink,
                  RoundRect, CornerSquare, CornerRounded, AspectLock, Warning, Sliders, Clock, Picker, MagicEraser,
                  AutoMosaic,
                  Watermark, More, Folder };

// dark: glyphs for a dark surface (light ink) instead of the light bars.
QIcon makeIcon(Icon icon, int size = 20, bool dark = false);

// Text-card glyphs: muted ink, accent when hovered (QIcon::Active) or checked, dim when disabled.
QIcon cardIcon(Icon icon, int size = 20);
// Muted glyphs that brighten to ink on hover; used by the save card's close/folder actions.
QIcon subtleIcon(Icon icon, int size = 20);

QToolButton *iconButton(Icon icon, const QString &tip);

// Shared dark chrome tokens: one surface, ink, accent and radius scale for every floating bar and menu.
namespace Bar {
inline const QColor background{"#101114"}, surface{"#18191d"}, content{"#0c0d10"},
    field{"#111216"}, ink{"#f4f5f8"}, muted{"#9c9da1"}, dim{"#73757f"},
    accent{"#5e6ad2"}, divider{"#2b2d33"}, hover{"#222326"}, selection{"#32345b"},
    focusRing{"#828fff"}, primaryFill{"#e5e5e6"}, primaryInk{"#09090a"};
constexpr int button = 28, icon = 20, radius = 8, gap = 8;
}
// Neutral modal surfaces share the action and focus colors with the floating bars.
namespace DialogTheme {
inline const QColor surface{"#1b1b1c"}, field{"#121213"}, ink{"#f7f8f8"}, secondary{"#d0d6e0"},
    muted{"#8a8f98"}, controlBorder{"#777980"}, hover{"#242425"},
    pressed{"#2b2b2c"}, selected{"#29292a"}, primaryHover{"#ffffff"},
    primaryPressed{"#d0d6e0"}, border{255, 255, 255, 23},
    strongBorder{255, 255, 255, 36}, shadow{0, 0, 0, 102};
}
// Capture bar glyphs: light ink, accent when checked, dim when disabled.
QIcon barIcon(Icon icon, int size = Bar::icon);
QToolButton *barButton(Icon icon, const QString &tip);
// Pill combo for the dark capture bars: flat popup rows and a thin chevron.
QComboBox *flatComboBox();

// Short dropdowns stay chrome-free: ten rows or fewer hide the scrollbar and fit without padding.
void prepareComboPopup(QComboBox *combo);

// Popup rows keep the default left-aligned content and paint the shared rounded hover/selection wash.
class PopupItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setContentInset(int inset) { m_contentInset = inset; }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
protected:
    static void paintRow(QPainter *painter, const QStyleOptionViewItem &option);
private:
    int m_contentInset = 12;
};

// Rounded track slider for the dark capture bars.
extern const QString darkSliderQss;

// A toolbar button with a solid color chip; checkable by the caller.
QToolButton *colorButton(const QColor &color, const QString &tip, int size = 32);

// Dark menu used by the tray, pin and selection menus.
extern const QString darkMenuQss;

// 1px vertical divider between toolbar groups, styled by the bar stylesheet.
QWidget *barSeparator(bool dark = false);

// Rounded top-level tool surfaces: keep the opaque sheet background and clip the window corners.
void applyRoundedMask(QWidget *widget, int radius = 8);

// ---------- Compact color strip with the complete palette available on demand. ----------
class ColorBoard : public QWidget
{
    Q_OBJECT
public:
    explicit ColorBoard(const QColor &current, QWidget *parent = nullptr, int commonCount = 6);
    void setCurrentColor(const QColor &color);
    QColor currentColor() const { return m_current; }
    int commonCount() const { return m_commonCount; }
    QRect currentRect() const { return QRect(2, 4, 20, 20); }
    static QRect swatchRect(int index, int commonCount = 6);
    static const QColor &preset(int index);
    static constexpr int PresetCount = 20, CommonCount = 6;
    static int testRendering();
signals:
    void presetPicked(const QColor &color);
    void customRequested();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override { setFocus(Qt::MouseFocusReason); e->accept(); }
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
private:
    int swatchAt(const QPoint &pos) const;
    const QColor &swatchColor(int index) const;
    QColor m_current;
    int m_hover = -1;
    int m_commonCount = 6;
    int m_keyIndex = 0;              // 0 = current color chip; 1..N = preset cells.
};
