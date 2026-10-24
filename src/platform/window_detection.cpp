#include "platform/window_detection.h"
#include <QGuiApplication>
#include <QImage>
#include <QVector>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QScreen>
#include <QtMath>
#include <QElapsedTimer>
#include <QPointer>
#include <QFileInfo>
#include <qt_windows.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <functional>
#include <algorithm>

qint64 rectArea(const QRect &r) { return qint64(r.width()) * r.height(); }

namespace {
int colorDelta(const QColor &a, const QColor &b)
{
    return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
}
}

QRect pixelContentRect(const QImage &image, const QRect &imageNative, const QRect &hitNative,
                       int maxInset)
{
    if (image.isNull() || image.size() != imageNative.size() || hitNative.isEmpty())
        return hitNative;
    const int maxScan = qMin(maxInset, qMin(hitNative.width() / 2, hitNative.height() / 2));
    if (maxScan < 2) return hitNative;
    const auto pixel = [&](int x, int y) {
        return image.pixelColor(x - imageNative.x(), y - imageNative.y());
    };
    const int ys[] = {hitNative.top() + hitNative.height() / 2,
                      hitNative.top() + hitNative.height() / 4,
                      hitNative.top() + hitNative.height() * 3 / 4};
    const int xs[] = {hitNative.left() + hitNative.width() / 2,
                      hitNative.left() + hitNative.width() / 4,
                      hitNative.left() + hitNative.width() * 3 / 4};
    QVector<int> leftScore(maxScan, 0), rightScore(maxScan, 0);
    QVector<int> topScore(maxScan, 0), bottomScore(maxScan, 0);
    QVector<int> leftCount(maxScan, 0), rightCount(maxScan, 0);
    QVector<int> topCount(maxScan, 0), bottomCount(maxScan, 0);
    // shortcut: an image edge that matches the shadow/background color has no
    // step to find; use compositor alpha if that case becomes common.
    for (int y : ys) {
        for (int i = 0; i < maxScan; ++i) {
            const int x = hitNative.left() + i;
            if (imageNative.contains(x, y) && imageNative.contains(x + 1, y)) {
                leftScore[i] += colorDelta(pixel(x, y), pixel(x + 1, y));
                ++leftCount[i];
            }
            const int xr = hitNative.right() - i;
            if (imageNative.contains(xr - 1, y) && imageNative.contains(xr, y)) {
                rightScore[i] += colorDelta(pixel(xr - 1, y), pixel(xr, y));
                ++rightCount[i];
            }
        }
    }
    for (int x : xs) {
        for (int i = 0; i < maxScan; ++i) {
            const int y = hitNative.top() + i;
            if (imageNative.contains(x, y) && imageNative.contains(x, y + 1)) {
                topScore[i] += colorDelta(pixel(x, y), pixel(x, y + 1));
                ++topCount[i];
            }
            const int yb = hitNative.bottom() - i;
            if (imageNative.contains(x, yb - 1) && imageNative.contains(x, yb)) {
                bottomScore[i] += colorDelta(pixel(x, yb - 1), pixel(x, yb));
                ++bottomCount[i];
            }
        }
    }
    const auto firstEdge = [](const QVector<int> &score, const QVector<int> &count) {
        // A shadow fades in gradually; the image starts where the step jumps
        // sharply above that gradient. Requiring a relative jump skips both the
        // smooth rim and later texture edges inside the image.
        for (int i = 1; i < score.size(); ++i) {
            if (count[i] < 2) continue;
            const int avg = score[i] / count[i];
            const int previous = count[i - 1] ? score[i - 1] / count[i - 1] : 0;
            if (avg >= 24 && avg >= 2 * qMax(1, previous)) return i;
        }
        return -1;
    };
    int left = hitNative.left(), top = hitNative.top();
    int right = hitNative.right(), bottom = hitNative.bottom();
    const int li = firstEdge(leftScore, leftCount);
    const int ri = firstEdge(rightScore, rightCount);
    const int ti = firstEdge(topScore, topCount);
    const int bi = firstEdge(bottomScore, bottomCount);
    if (li >= 0) left = hitNative.left() + li + 1;
    if (ri >= 0) right = hitNative.right() - ri - 1;
    if (ti >= 0) top = hitNative.top() + ti + 1;
    if (bi >= 0) bottom = hitNative.bottom() - bi - 1;
    const QRect refined(left, top, right - left + 1, bottom - top + 1);
    if (refined.width() < 8 || refined.height() < 8 || !hitNative.contains(refined))
        return hitNative;
    return refined;
}

void prepareChildren(DetectedWindow &win)
{
    QVector<QRect> rects;
    for (const QRect &child : win.nativeChildren) {
        const QRect clipped = child.intersected(win.native);
        if (!clipped.isEmpty() && !rects.contains(clipped)) rects.append(clipped);
    }
    const auto smaller = [](const QRect &a, const QRect &b) { return rectArea(a) < rectArea(b); };
    std::stable_sort(rects.begin(), rects.end(), smaller);
    QVector<QRect> strips;
    for (int i = rects.size() - 1; i >= 0; --i)
        for (int j = i - 1; j >= 0; --j)
            if (rects[i].contains(rects[j])
                && (rects[i].width() == rects[j].width() || rects[i].height() == rects[j].height()))
                for (const QRect &strip : QRegion(rects[i]).subtracted(QRegion(rects[j])))
                    if (!rects.contains(strip) && !strips.contains(strip)) strips.append(strip);
    rects += strips;
    std::stable_sort(rects.begin(), rects.end(), smaller);
    rects.removeAll(win.native);
    win.nativeChildren = rects;
}

QRect windowFrame(HWND hwnd)
{
    RECT r{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &r, sizeof(r)))) {
        UINT thickness = 0;
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(hwnd, &placement) && placement.showCmd != SW_SHOWMAXIMIZED
            && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_BORDER))
            DwmGetWindowAttribute(hwnd, 37 /* DWMWA_VISIBLE_FRAME_BORDER_THICKNESS */, &thickness, sizeof(thickness));
        return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top)
            .adjusted(thickness, thickness, -int(thickness), -int(thickness));
    }
    return GetWindowRect(hwnd, &r) ? QRect(r.left, r.top, r.right - r.left, r.bottom - r.top) : QRect();
}

QString windowProgramName(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768];
    DWORD length = sizeof(path) / sizeof(path[0]);
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    return ok ? QFileInfo(QString::fromWCharArray(path, length)).fileName().toLower() : QString();
}

QRect clampWindowFrame(const QRect &rect, bool maximized)
{
    struct Monitors { HMONITOR center; QRect rect; bool crosses; };
    Monitors monitors{MonitorFromPoint(POINT{rect.center().x(), rect.center().y()}, MONITOR_DEFAULTTONEAREST), rect, false};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT bounds, LPARAM lp) -> BOOL {
        Monitors *m = reinterpret_cast<Monitors *>(lp);
        if (monitor != m->center && m->rect.intersects(QRect(bounds->left, bounds->top,
            bounds->right - bounds->left, bounds->bottom - bounds->top))) m->crosses = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if ((!maximized && monitors.crosses) || !GetMonitorInfoW(monitors.center, &info)) return rect;
    const RECT &r = info.rcMonitor;
    return rect.intersected(QRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
}

WindowTargets detectWindowRects(HWND overlay, const QStringList &excluded)
{
    struct Ctx { WindowTargets out; HWND overlay; QStringList excluded; } ctx{{}, overlay, excluded};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx *ctx = reinterpret_cast<Ctx *>(lp);
        if (hwnd == ctx->overlay || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
            return TRUE;
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TRANSPARENT)                  // Click-through overlay: never a capture target.
            return TRUE;
        if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_DISABLED)
            return TRUE;
        int cloaked = 0;
        if (DwmGetWindowAttribute(hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked)) >= 0 && cloaked)
            return TRUE;                             // Hidden on another virtual desktop.
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        for (const wchar_t *bad : kExcludedWindowClasses)
            if (!_wcsicmp(cls, bad))
                return TRUE;
        DetectedWindow win;
        win.hwnd = hwnd;
        win.program = windowProgramName(hwnd);
        if (!win.program.isEmpty() && ctx->excluded.contains(win.program, Qt::CaseInsensitive)) return TRUE;
        win.native = windowFrame(hwnd);
        if (win.native.isEmpty()) return TRUE;
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        win.maximized = GetWindowPlacement(hwnd, &placement) && placement.showCmd == SW_SHOWMAXIMIZED;
        win.native = clampWindowFrame(win.native, win.maximized);
        if (win.native.isEmpty()) return TRUE;
        struct ChildCtx { QVector<QRect> *out; } childCtx { &win.nativeChildren };
        EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
            ChildCtx *c = reinterpret_cast<ChildCtx *>(lp);
            if (!IsWindow(child) || !IsWindowVisible(child) || IsIconic(child))
                return TRUE;
            RECT r {};
            if (GetWindowRect(child, &r) && r.left < r.right && r.top < r.bottom)
                c->out->append(QRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&childCtx));
        prepareChildren(win);
        ctx->out.append(win);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

QRect childAt(const DetectedWindow &win, const QPoint &point, const QRect &previous)
{
    for (const QRect &child : win.nativeChildren)
        if (previous.contains(point) ? (child != previous && child.contains(previous)) : child.contains(point))
            return child;
    return win.native.contains(point) ? win.native : QRect();
}

QRect accessibleRect(IAccessible *object, const VARIANT &id, const QRect &bounds)
{
    LONG x = 0, y = 0, w = 0, h = 0;
    if (FAILED(object->accLocation(&x, &y, &w, &h, id)) || w <= 0 || h <= 0) return {};
    return QRect(x, y, w, h).intersected(bounds);
}

bool windowHung(HWND hwnd)
{
    DWORD_PTR reply = 0;
    return IsHungAppWindow(hwnd)
        || (!SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 60, &reply)
            && GetLastError() == ERROR_TIMEOUT);
}

namespace {
int uiaCacheAdd(UiaCache *cache, const QRect &rect, int parent)
{
    if (!cache || rect.isEmpty()) return -1;
    for (int i = 0; i < cache->entries.size(); ++i)
        if (cache->entries.at(i).native == rect && cache->entries.at(i).parent == parent)
            return i;
    // shortcut: cap virtualized trees; raise the cap if a deep list loses elements.
    if (cache->entries.size() >= 1500) return -1;
    cache->entries.append({rect, parent, false});
    return cache->entries.size() - 1;
}

int uiaCacheHit(const UiaCache &cache, const QPoint &point)
{
    int best = -1;
    qint64 bestArea = 0;
    for (int i = 0; i < cache.entries.size(); ++i) {
        const QRect &rect = cache.entries.at(i).native;
        if (!rect.contains(point)) continue;
        const qint64 area = rectArea(rect);
        if (best < 0 || area < bestArea) { best = i; bestArea = area; }
    }
    return best;
}

// Semantic controls whose own rectangle is the useful capture target. Returning
// the control instead of walking into its label keeps a sweep responsive; real
// containers (Pane, Group, Document, List, ToolBar...) stay unexpanded so the
// first hover inside them still refines.
bool uiaCacheLeaf(int controlType)
{
    switch (controlType) {
    case 50000:  // Button
    case 50002:  // CheckBox
    case 50003:  // ComboBox
    case 50004:  // Edit
    case 50005:  // Hyperlink
    case 50006:  // Image
    case 50007:  // ListItem
    case 50011:  // MenuItem
    case 50012:  // ProgressBar
    case 50013:  // RadioButton
    case 50014:  // ScrollBar
    case 50015:  // Slider
    case 50016:  // Spinner
    case 50019:  // TabItem
    case 50020:  // Text
    case 50022:  // ToolTip
    case 50024:  // TreeItem
    case 50027:  // Thumb
    case 50031:  // SplitButton
    case 50035:  // HeaderItem
    case 50038:  // Separator
        return true;
    default:
        return false;
    }
}

QRect uiaCacheAncestor(const UiaCache &cache, const QRect &previous)
{
    if (previous.isEmpty()) return {};
    for (int i = 0; i < cache.entries.size(); ++i) {
        if (cache.entries.at(i).native != previous) continue;
        for (int parent = cache.entries.at(i).parent; parent >= 0;
             parent = cache.entries.at(parent).parent) {
            const QRect &rect = cache.entries.at(parent).native;
            if (rect != previous && rect.contains(previous)) return rect;
        }
    }
    // A native or MSAA child may be finer than the cached UIA level; the
    // smallest cached rectangle containing it is the next expandable level.
    int best = -1;
    qint64 bestArea = 0;
    for (int i = 0; i < cache.entries.size(); ++i) {
        const QRect &rect = cache.entries.at(i).native;
        if (!rect.contains(previous) || rect == previous) continue;
        const qint64 area = rectArea(rect);
        if (best < 0 || area < bestArea) { best = i; bestArea = area; }
    }
    return best >= 0 ? cache.entries.at(best).native : QRect();
}
}

QRect accessibleAt(HWND hwnd, const QPoint &point, const QRect &bounds, const QRect &previous)
{
    if (windowHung(hwnd)) return {};
    WINDOWINFO info{};
    info.cbSize = sizeof(info);
    if (!GetWindowInfo(hwnd, &info) || (info.dwWindowStatus == WS_ACTIVECAPTION
        && (info.dwStyle & (WS_THICKFRAME | WS_CAPTION) || info.dwExStyle & WS_EX_WINDOWEDGE))) return {};
    IAccessible *object = nullptr;
    const HRESULT objectResult = AccessibleObjectFromWindow(hwnd, OBJID_WINDOW, kAccessibleIID,
                                        reinterpret_cast<void **>(&object));
    if (FAILED(objectResult) || !object) return {};
    QElapsedTimer timer;
    timer.start();
    QRect best;
    const auto accepts = [&point, &previous](const QRect &r) {
        return r.contains(point) && (!previous.contains(point) || (r != previous && r.contains(previous)));
    };
    for (int depth = 0; object && depth < 60; ++depth) {
        VARIANT self{};
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        const QRect current = accessibleRect(object, self, bounds);
        if (accepts(current)) best = current;
        VARIANT state{};
        const bool invisible = SUCCEEDED(object->get_accState(self, &state)) && state.vt == VT_I4
                               && (state.lVal & STATE_SYSTEM_INVISIBLE);
        VariantClear(&state);
        VARIANT hit{};
        // shortcut: The budget bounds traversal, not a single stalled provider call; isolate COM in a helper process if this occurs.
        if (invisible || timer.elapsed() >= 100 || FAILED(object->accHitTest(point.x(), point.y(), &hit))) {
            VariantClear(&hit);
            break;
        }
        if (hit.vt == VT_DISPATCH && hit.pdispVal) {
            IAccessible *next = nullptr;
            hit.pdispVal->QueryInterface(kAccessibleIID, reinterpret_cast<void **>(&next));
            VariantClear(&hit);
            object->Release();
            object = next;
            continue;
        }
        if (hit.vt == VT_EMPTY) { hit.vt = VT_I4; hit.lVal = CHILDID_SELF; }
        if (hit.vt == VT_I4) {
            const QRect leaf = accessibleRect(object, hit, bounds);
            if (accepts(leaf) && (best.isEmpty() || best.contains(leaf))) best = leaf;
            if (previous.contains(point)) {
                for (int up = depth; object && up < 60 && timer.elapsed() < 100; ++up) {
                    const QRect parentRect = accessibleRect(object, self, bounds);
                    if (accepts(parentRect)) { best = parentRect; break; }
                    IDispatch *parent = nullptr;
                    IAccessible *next = nullptr;
                    if (SUCCEEDED(object->get_accParent(&parent)) && parent) {
                        parent->QueryInterface(kAccessibleIID, reinterpret_cast<void **>(&next));
                        parent->Release();
                    }
                    object->Release();
                    object = next;
                }
            }
        }
        VariantClear(&hit);
        break;
    }
    if (object) object->Release();
    return best;
}

QRect uiaRectAt(UiaAutomation *automation, HWND hwnd, const QPoint &point, const QRect &bounds,
                       const QRect &previous, QStringList *downgraded, const QString &program,
                       UiaCache *cache)
{
    const bool cached = cache && cache->hwnd == hwnd && cache->bounds == bounds;
    if (cached) {
        // Warm path: a hover sweep over recorded siblings is a local rectangle
        // hit test, not another COM walk.
        if (!previous.isEmpty()) {
            const QRect parent = uiaCacheAncestor(*cache, previous);
            if (!parent.isEmpty()) return parent;
        } else {
            const int hit = uiaCacheHit(*cache, point);
            if (hit >= 0) {
                const UiaCacheEntry &entry = cache->entries.at(hit);
                // A hit whose children were never enumerated must still walk:
                // returning the container would hide every element inside it.
                if (entry.expanded && entry.native != bounds && entry.native.contains(point))
                    return entry.native;
            }
        }
    }
    if (!automation || windowHung(hwnd)) return QRect();
    if (cache && !cached) {
        cache->hwnd = hwnd;
        cache->bounds = bounds;
        cache->entries.clear();
    }
    const auto rectOf = [bounds](UiaElement *element) {
        RECT r{};
        if (!element || FAILED(element->CurrentBoundingRectangle(&r)) || r.right <= r.left || r.bottom <= r.top)
            return QRect();
        return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top).intersected(bounds);
    };
    // Wake sleeping Chromium engines: they activate their accessibility provider when a client
    // queries WM_GETOBJECT with the UIA root object id; the tree then builds asynchronously.
    const auto nudge = [&]() {
        POINT local{point.x(), point.y()};
        ScreenToClient(hwnd, &local);
        const HWND child = ChildWindowFromPointEx(hwnd, local, CWP_SKIPINVISIBLE);
        DWORD_PTR reply = 0;
        SendMessageTimeoutW(child ? child : hwnd, WM_GETOBJECT, 0, -25 /* UiaRootObjectId */,
                            SMTO_ABORTIFHUNG, 30, &reply);
    };
    nudge();
    UiaElement *root = nullptr;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) return QRect();
    UiaTreeWalker *walker = nullptr;
    const bool walking = SUCCEEDED(automation->ControlViewWalker(&walker)) && walker;
    // The walk budget keeps hover queries interactive; a slow first tree build
    // still returns the best rectangle found so far. It must not disable later
    // queries: Chromium builds its accessibility tree asynchronously, so one
    // over-budget walk is normal and a permanent downgrade loses real elements.
    QElapsedTimer timer;
    timer.start();
    const int rootIndex = uiaCacheAdd(cache, rectOf(root), -1);
    // Single-path descent with decoy skipping: some providers expose empty containers with the
    // same rectangle as the real branch (e.g. browser frames), so recurse only into the first
    // sibling that shrinks the rectangle. Remaining siblings are recorded for the warm cache so
    // a sweep across a toolbar does not repeat this walk.
    std::function<UiaElement *(UiaElement *, int, int)> search;
    search = [&](UiaElement *element, int depth, int selfIndex) -> UiaElement * {
        element->AddRef();                               // Owned reference for the caller.
        if (depth >= 60 || timer.elapsed() >= kUiaQueryBudgetMs) return element;
        const QRect selfRect = rectOf(element);
        const int selfArea = rectArea(selfRect);
        UiaElement *chosen = nullptr;
        UiaElement *child = nullptr;
        bool expanded = false;
        int afterChosen = 0;
        int typed = 0;
        if (SUCCEEDED(walker->GetFirstChildElement(element, &child))) {
            expanded = true;
            while (child) {
                if (timer.elapsed() >= kUiaQueryBudgetMs) { expanded = false; break; }
                const QRect childRect = rectOf(child);
                const int childIndex = uiaCacheAdd(cache, childRect, selfIndex);
                if (!chosen && childRect.contains(point)) {
                    UiaElement *found = search(child, depth + 1, childIndex);
                    if (rectArea(rectOf(found)) < selfArea) chosen = found;
                    else found->Release();
                } else if (cache && typed < 64 && childIndex >= 0
                           && !cache->entries.at(childIndex).expanded) {
                    ++typed;
                    int type = 0;
                    if (SUCCEEDED(child->CurrentControlType(&type)) && uiaCacheLeaf(type))
                        cache->entries[childIndex].expanded = true;
                }
                // Enough siblings to cover a toolbar or list row without letting
                // a huge virtualized list turn the first hover into a full scan.
                // Without a cache the old stop-at-first-shrink behavior is kept.
                if (chosen) {
                    if (!cache) break;
                    if (++afterChosen >= 48) { expanded = false; break; }
                }
                UiaElement *sibling = nullptr;
                const HRESULT hr = walker->GetNextSiblingElement(child, &sibling);
                child->Release();
                child = SUCCEEDED(hr) ? sibling : nullptr;
            }
            if (child) child->Release();
        }
        if (expanded && cache && selfIndex >= 0) cache->entries[selfIndex].expanded = true;
        if (chosen) {
            element->Release();
            return chosen;                               // Shrunk: the finer element wins.
        }
        return element;
    };
    UiaElement *held = walking ? search(root, 0, rootIndex) : nullptr;
    QRect deepest = rectOf(held ? held : root);
    QRect result;
    if (previous.isEmpty()) {
        result = deepest.contains(point) ? deepest : QRect();
    } else if (walking) {
        // Expansion: first control-view ancestor strictly containing the current region.
        while (held && timer.elapsed() < kUiaQueryBudgetMs) {
            const QRect r = rectOf(held);
            if (!r.isEmpty() && r != previous && r.contains(previous)) { result = r; break; }
            UiaElement *parent = nullptr;
            if (FAILED(walker->GetParentElement(held, &parent)) || !parent) break;
            held->Release();
            held = parent;
        }
    }
    if (held) held->Release();
    if (walking) walker->Release();
    root->Release();
    // Diagnostic only: a slow provider is retried instead of being disabled for
    // the rest of the capture session.
    if (timer.elapsed() > kUiaQueryBudgetMs && !program.isEmpty() && !downgraded->contains(program))
        downgraded->append(program);
    return result;
}

QRect logicalFromNative(const QVector<ScreenFrame> &frames, const QRect &native)
{
    if (native.isEmpty()) return {};
    QRect result;
    for (const ScreenFrame &f : frames)
        if (f.pixelRect.intersects(native)) {
            const QRect part = f.pixelRect.intersected(native);
            const double sx = double(f.logicalRect.width()) / f.pixelRect.width();
            const double sy = double(f.logicalRect.height()) / f.pixelRect.height();
            const int x = qRound(f.logicalRect.x() + (part.x() - f.pixelRect.x()) * sx);
            const int y = qRound(f.logicalRect.y() + (part.y() - f.pixelRect.y()) * sy);
            const int right = qRound(f.logicalRect.x() + (part.x() + part.width() - f.pixelRect.x()) * sx);
            const int bottom = qRound(f.logicalRect.y() + (part.y() + part.height() - f.pixelRect.y()) * sy);
            result |= QRect(x, y, right - x, bottom - y);
        }
    return result.isEmpty() ? native : result;
}

WindowDetector::WindowDetector(HWND overlay, const DetectionOptions &options,
                               const WindowTargets &initialTargets)
    : m_overlay(overlay), m_options(options), m_initialTargets(initialTargets) {}

WindowDetector::~WindowDetector()  { stop(); wait(); }

WindowDetector *WindowDetector::create(HWND overlay, const DetectionOptions &options,
                                       const WindowTargets &initialTargets)
{
    s_workers.erase(std::remove_if(s_workers.begin(), s_workers.end(),
        [](const QPointer<WindowDetector> &worker) { return !worker || worker->isFinished(); }), s_workers.end());
    // shortcut: Two stalled in-process providers exhaust detection capacity; use process isolation for restartable providers.
    if (s_workers.size() >= 2) return nullptr;
    auto *worker = new WindowDetector(overlay, options, initialTargets);
    s_workers.append(worker);
    return worker;
}

quint64 WindowDetector::query(const QPoint &point, const QRect &previous) {
    QMutexLocker lock(&m_mutex);
    const quint64 id = ++m_id;
    m_queue.append({point, previous, id});
    while (m_queue.size() > 4) m_queue.removeFirst();
    m_wake.wakeOne();
    return id;
}

void WindowDetector::cancel() { QMutexLocker lock(&m_mutex); ++m_id; m_queue.clear(); }

void WindowDetector::stop() { QMutexLocker lock(&m_mutex); m_stop = true; m_queue.clear(); m_wake.wakeOne(); }

void WindowDetector::run()  {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com))
        CoCreateInstance(kUiaClassId, nullptr, CLSCTX_INPROC_SERVER, kUiaIid,
                         reinterpret_cast<void **>(&m_uia));
    // Snapshotting before the overlay activates keeps transient browser bubbles
    // (for example the page QR code) that close as soon as they lose focus.
    WindowTargets cachedTargets = m_initialTargets;
    QElapsedTimer targetsAge;
    bool haveTargets = !cachedTargets.isEmpty();
    const bool fixedTargets = haveTargets;
    if (haveTargets) targetsAge.start();
    while (true) {
        Request request;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_stop && m_queue.isEmpty()) m_wake.wait(&m_mutex);
            if (m_stop) break;
            request = m_queue.takeFirst();
            if (request.id != m_id) continue;
        }
        if (m_options.windows && !fixedTargets && (!haveTargets || targetsAge.elapsed() >= 300)) {
            cachedTargets = detectWindowRects(m_overlay, m_options.excludedPrograms);
            targetsAge.restart();
            haveTargets = true;
        }
        WindowTargets targets = m_options.windows ? cachedTargets : WindowTargets();
        {
            QMutexLocker lock(&m_mutex);
            if (m_stop) break;
        }
        emit gotTargets(targets, request.id);
        QRect result;
        for (const DetectedWindow &win : targets) {
            if (!win.native.contains(request.point)) continue;
            result = win.native;
            if (m_options.elements) {
                const bool blocked = !m_options.disabledUiPrograms.contains(win.program, Qt::CaseInsensitive);
                const QRect accessible = SUCCEEDED(com) && blocked
                    ? accessibleAt(win.hwnd, request.point, win.native, request.previous) : QRect();
                const QRect child = childAt(win, request.point, request.previous);
                // Snipaste is MSAA-first. Keep a semantic MSAA/native control,
                // but let UIA refine container-sized results (Chromium exposes
                // its page pane to MSAA while the input lives in the UIA tree).
                QRect best;
                if (!accessible.isEmpty()) {
                    best = accessible;
                    if (!child.isEmpty() && rectArea(child) < rectArea(accessible)) best = child;
                } else {
                    best = child;
                }
                const bool container = best.isEmpty() || !request.previous.isEmpty()
                    || rectArea(best) * 4 >= rectArea(win.native) * 3;
                if (container) {
                    const QRect uia = m_uia && blocked
                        ? uiaRectAt(m_uia, win.hwnd, request.point, win.native, request.previous,
                                    &m_uiaSlow, win.program, &m_uiaCache) : QRect();
                    if (!uia.isEmpty() && (best.isEmpty() || rectArea(uia) < rectArea(best)))
                        best = uia;
                }
                if (!best.isEmpty()) result = best;
            }
            break;
        }
        {
            QMutexLocker lock(&m_mutex);
            if (m_stop) break;
        }
        // Deliver every processed query even when a newer one is already queued: during a
        // fast slide each result is superseded before it finishes, and dropping it here
        // would freeze the hover border until the mouse stops. One worker emits in query
        // order, and the overlay rejects results that arrive out of order.
        emit gotRect(result, request.id);
    }
    if (m_uia) { m_uia->Release(); m_uia = nullptr; }
    if (SUCCEEDED(com)) CoUninitialize();
}
