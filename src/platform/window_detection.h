#pragma once

#include "platform/screen_capture.h"
#include <QImage>
#include <QVector>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QtMath>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QPointer>
#include <qt_windows.h>
#include <oleacc.h>

// ---------- Window and UI-element detection ----------
struct DetectionOptions {
    bool windows = true;
    bool elements = false;
    QStringList excludedPrograms;
    QStringList disabledUiPrograms;
};

// Keep hover walking responsive without turning one slow provider into a
// session-wide downgrade; results are asynchronous and stale replies are filtered.
inline constexpr int kUiaQueryBudgetMs = 240;

// Rectangles use native pixels in the detector and local logical coordinates in the overlay.
struct DetectedWindow {
    QRect native;
    QVector<QRect> nativeChildren;
    HWND hwnd = nullptr;
    QString program;
    bool maximized = false;
    DetectedWindow(const QRect &rect = QRect(), const QVector<QRect> &children = {})
        : native(rect), nativeChildren(children) {}
};
typedef QVector<DetectedWindow> WindowTargets;
Q_DECLARE_METATYPE(WindowTargets)

// Class names that never carry capturable content but often own fullscreen
// rects (host surfaces and floating overlays).
static const wchar_t *const kExcludedWindowClasses[] = {
    L"Progman",                        // Desktop background.
    L"WorkerW",                        // Desktop wallpaper behind icons.
    L"CEF-OSC-WIDGET",                 // Embedded-browser floating overlays.
    L"EdgeUiInputTopWndClass",         // Edge gesture strips at screen edges.
    L"Windows.UI.Core.CoreWindow",     // UWP host surfaces (mostly cloaked shells).
    L"NarratorHelperWindow",
    L"Tao Thread Event Target",        // Helper message windows of toolkits.
};

qint64 rectArea(const QRect &r);

void prepareChildren(DetectedWindow &win);

QRect windowFrame(HWND hwnd);

// Preserve z-order; exclude only the capture overlay, not this application's pinned images.
QString windowProgramName(HWND hwnd);

QRect clampWindowFrame(const QRect &rect, bool maximized);

WindowTargets detectWindowRects(HWND overlay = nullptr, const QStringList &excluded = {});

QRect childAt(const DetectedWindow &win, const QPoint &point, const QRect &previous = {});

QRect accessibleRect(IAccessible *object, const VARIANT &id, const QRect &bounds);

// This toolchain's oleacc import library resolves IID_IAccessible to a thunk, not GUID data.
static const GUID kAccessibleIID = {0x618736e0, 0x3c3d, 0x11cf, {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Minimal UI Automation client. The toolchain ships only a stub UIA header, so these interfaces
// follow the vtable order of the registered UIAutomationCore type library (verified per slot).
struct UiaElement {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const GUID &riid, void **out) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFocus() = 0;                                        // 0x18
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeId(void **runtimeId) = 0;                    // 0x20
    virtual HRESULT STDMETHODCALLTYPE FindFirst(int scope, void *condition, UiaElement **found) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAll(int scope, void *condition, void **found) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindFirstBuildCache(int scope, void *condition, void *cache, UiaElement **found) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindAllBuildCache(int scope, void *condition, void *cache, void **found) = 0;
    virtual HRESULT STDMETHODCALLTYPE BuildUpdatedCache(void *cache, UiaElement **updated) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPropertyValue(int property, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPropertyValueEx(int property, int ignore, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedPropertyValue(int property, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedPropertyValueEx(int property, int ignore, VARIANT *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPatternAs(int pattern, const GUID &riid, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedPatternAs(int pattern, const GUID &riid, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPattern(int pattern, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedPattern(int pattern, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedParent(UiaElement **parent) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCachedChildren(void **children) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentProcessId(int *out) = 0;                        // 0xa0
    virtual HRESULT STDMETHODCALLTYPE CurrentControlType(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentLocalizedControlType(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentName(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentAcceleratorKey(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentAccessKey(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentHasKeyboardFocus(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsKeyboardFocusable(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsEnabled(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentAutomationId(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentClassName(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentHelpText(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentCulture(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsControlElement(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsContentElement(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsPassword(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentNativeWindowHandle(void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentItemType(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsOffscreen(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentOrientation(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentFrameworkId(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentIsRequiredForForm(int *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentItemStatus(BSTR *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentBoundingRectangle(RECT *out) = 0;              // 0x158
};
struct UiaTreeWalker {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const GUID &riid, void **out) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParentElement(UiaElement *element, UiaElement **parent) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFirstChildElement(UiaElement *element, UiaElement **child) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLastChildElement(UiaElement *element, UiaElement **child) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNextSiblingElement(UiaElement *element, UiaElement **next) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPreviousSiblingElement(UiaElement *element, UiaElement **previous) = 0;
    virtual HRESULT STDMETHODCALLTYPE NormalizeElement(UiaElement *element, UiaElement **normalized) = 0;
};
struct UiaAutomation {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const GUID &riid, void **out) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    virtual HRESULT STDMETHODCALLTYPE CompareElements(UiaElement *first, UiaElement *second, int *same) = 0;
    virtual HRESULT STDMETHODCALLTYPE CompareRuntimeIds(void *first, void *second, int *same) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRootElement(UiaElement **root) = 0;
    virtual HRESULT STDMETHODCALLTYPE ElementFromHandle(HWND hwnd, UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE ElementFromPoint(POINT point, UiaElement **element) = 0;   // 0x38
    virtual HRESULT STDMETHODCALLTYPE GetFocusedElement(UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRootElementBuildCache(void *cache, UiaElement **root) = 0;
    virtual HRESULT STDMETHODCALLTYPE ElementFromHandleBuildCache(HWND hwnd, void *cache, UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE ElementFromPointBuildCache(POINT point, void *cache, UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFocusedElementBuildCache(void *cache, UiaElement **element) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTreeWalker(void *condition, UiaTreeWalker **walker) = 0;
    virtual HRESULT STDMETHODCALLTYPE ControlViewWalker(UiaTreeWalker **walker) = 0;         // 0x70
};
static const GUID kUiaClassId = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const GUID kUiaIid = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};

bool windowHung(HWND hwnd);

// All COM objects and VARIANTs remain on the querying thread.
QRect accessibleAt(HWND hwnd, const QPoint &point, const QRect &bounds, const QRect &previous = {});

// Control-view rectangles recorded during a scoped UIA walk. Later hover points are
// local hit tests, so sweeping across sibling controls does not repeat the COM walk.
struct UiaCacheEntry {
    QRect native;
    int parent = -1;
    bool expanded = false;   // Its control-view children were enumerated by a walk.
};
struct UiaCache {
    HWND hwnd = nullptr;
    QRect bounds;
    QVector<UiaCacheEntry> entries;
};

// Scoped UIA hit test for one window. The walk starts at the target window's own element and
// descends, so fullscreen capture overlays never hijack the result (a global ElementFromPoint
// returns the topmost overlay instead). Chromium engines expose their element tree only after
// an accessibility nudge, so wake them before walking; the walk is budget-capped and a slow
// provider is retried on later queries instead of being disabled for the session.
QRect uiaRectAt(UiaAutomation *automation, HWND hwnd, const QPoint &point, const QRect &bounds,
                       const QRect &previous, QStringList *downgraded, const QString &program,
                       UiaCache *cache = nullptr);

class WindowDetector : public QThread
{
    Q_OBJECT
public:
    WindowDetector(HWND overlay, const DetectionOptions &options,
                   const WindowTargets &initialTargets = {});
    ~WindowDetector() override;
    static WindowDetector *create(HWND overlay, const DetectionOptions &options,
                                  const WindowTargets &initialTargets = {});
    static int testCapacity();
    quint64 query(const QPoint &point, const QRect &previous = {});
    void cancel();
    void stop();
signals:
    void gotTargets(const WindowTargets &targets, quint64 id);
    void gotRect(const QRect &rect, quint64 id);
protected:
    void run() override;
private:
    struct Request { QPoint point; QRect previous; quint64 id; };
    inline static QVector<QPointer<WindowDetector>> s_workers;
    HWND m_overlay;
    DetectionOptions m_options;
    WindowTargets m_initialTargets;                  // Captured before the overlay takes focus; transient popups close after that.
    UiaAutomation *m_uia = nullptr;
    UiaCache m_uiaCache;
    QStringList m_uiaSlow;
    QMutex m_mutex;
    QWaitCondition m_wake;
    QVector<Request> m_queue;
    quint64 m_id = 0;
    bool m_stop = false;
};

// Map a native-pixel rect into logical coordinates via the frozen frames.
QRect logicalFromNative(const QVector<ScreenFrame> &frames, const QRect &native);

// Find the first strong pixel step inside hitNative; removes a layered window's
// self-drawn shadow after OS hit-test trimming.
QRect pixelContentRect(const QImage &image, const QRect &imageNative, const QRect &hitNative,
                       int maxInset = 64);
