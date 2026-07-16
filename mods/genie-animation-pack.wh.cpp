// ==WindhawkMod==
// @id              genie-minimize-animation-fork
// @name            Linux Animation Pack (Genie + friends)
// @description     macOS/Compiz-style minimize & restore effects: Genie, Vacuum, Glide, Pop, Slide, Free Fall, Warp, Squash, Roll-Up, Swirl.
// @version         2.0.0
// @author          lolstijl (fork)
// @github          https://github.com/lolstijl
// @include         *
// @compilerOptions -ldwmapi -lgdi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Linux Animation Pack

A fork of the Genie Animation Mod that turns Windows 11's boring minimize/restore
into a whole menu of Compiz/macOS-flavored effects. Pick one from the **Animation
style** dropdown in the mod's Settings tab — that dropdown is your toggle between the
classic Genie and all the new ones. Each style has its own **Duration** slider so you
can tune the feel of every effect independently, and there's a master **Enable
animations** switch to fall back to stock Windows without disabling the mod.

## The effects

- **Genie — Magic Lamp**: the classic. The window stretches and pours down into the
  taskbar like a genie into a lamp.
- **Vacuum**: the whole window shrinks and accelerates as it gets sucked into the
  taskbar icon.
- **Glide**: subtle GNOME-style shrink + fade in place. Understated and clean.
- **Pop**: the window swells slightly and vanishes. Snappy and modern.
- **Slide**: KDE-style straight drop off the bottom edge.
- **Free Fall**: gravity takes over — the window accelerates, stretches, sways, and
  tumbles off the bottom of the screen.
- **Warp**: Star Trek transporter. The window squeezes into a thin vertical beam of
  light, then the beam shoots up and dematerializes.
- **Squash**: the window is flattened like a pancake onto the taskbar.
- **Roll-Up**: the window rolls up into its own title bar like a window blind.
- **Swirl**: a whirlpool. The window spins side-to-side while shrinking down into
  the taskbar like water down a drain.

Restore plays every effect in reverse, so windows *un-genie*, *un-roll*, drop back
in, etc.

## Notes
- These animations can spike CPU if you spam minimize/restore very fast, because each
  one runs a short real-time render thread. Normal use is fine.
- Windows' own animation API is ancient, so a couple of effects (Warp especially) are
  clever GDI fakes rather than true 3D — but they read great in motion.
- Originally written with the help of Gemini & Claude; this multi-effect pack was
  extended by Claude.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enable animations
  $description: >-
    Master switch. Turn this off to fall back to Windows' default minimize/restore
    behavior without having to disable the whole mod.

- style: genie
  $name: Animation style
  $description: >-
    Which effect plays when you minimize or restore a window. This is your toggle
    between the classic Genie and all the new Linux-flavored effects. Restore always
    plays the same effect in reverse.
  $options:
  - genie: Genie — Magic Lamp (pours into the taskbar)
  - vacuum: Vacuum — sucked into the taskbar icon
  - glide: Glide — GNOME-style shrink & fade
  - pop: Pop — swell and vanish
  - slide: Slide — straight drop off the bottom (KDE)
  - fall: Free Fall — gravity tumble off screen
  - warp: Warp — Star Trek transporter beam
  - squash: Squash — flattened onto the taskbar
  - rollup: Roll-Up — rolls up like a window blind
  - swirl: Swirl — whirlpool down the drain

- duration_genie: 450
  $name: Duration — Genie (ms)
  $description: Clamped to 50-3000. Lower is snappier, higher is more deliberate.

- duration_vacuum: 380
  $name: Duration — Vacuum (ms)
  $description: Clamped to 50-3000.

- duration_glide: 300
  $name: Duration — Glide (ms)
  $description: Clamped to 50-3000.

- duration_pop: 260
  $name: Duration — Pop (ms)
  $description: Clamped to 50-3000.

- duration_slide: 340
  $name: Duration — Slide (ms)
  $description: Clamped to 50-3000.

- duration_fall: 620
  $name: Duration — Free Fall (ms)
  $description: Clamped to 50-3000.

- duration_warp: 520
  $name: Duration — Warp (ms)
  $description: Clamped to 50-3000.

- duration_squash: 400
  $name: Duration — Squash (ms)
  $description: Clamped to 50-3000.

- duration_rollup: 380
  $name: Duration — Roll-Up (ms)
  $description: Clamped to 50-3000.

- duration_swirl: 700
  $name: Duration — Swirl (ms)
  $description: Clamped to 50-3000.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <math.h>
#include <wchar.h>
#include <atomic>
#include <unordered_map>
#include <mutex>

typedef LRESULT (WINAPI *DefWindowProcW_t)(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
DefWindowProcW_t DefWindowProcW_Original;

typedef BOOL (WINAPI *ShowWindow_t)(HWND hWnd, int nCmdShow);
ShowWindow_t ShowWindow_Original;

// -------------------------------------------------------------------------
// Animation modes
// -------------------------------------------------------------------------
enum AnimMode {
    MODE_GENIE = 0,
    MODE_VACUUM,
    MODE_GLIDE,
    MODE_POP,
    MODE_SLIDE,
    MODE_FALL,
    MODE_WARP,
    MODE_SQUASH,
    MODE_ROLLUP,
    MODE_SWIRL,
    MODE_COUNT
};

// Keys must match the $options keys in the settings block above, in enum order.
static const wchar_t* kModeKeys[MODE_COUNT] = {
    L"genie", L"vacuum", L"glide", L"pop", L"slide",
    L"fall",  L"warp",   L"squash", L"rollup", L"swirl"
};
static const wchar_t* kDurKeys[MODE_COUNT] = {
    L"duration_genie", L"duration_vacuum", L"duration_glide", L"duration_pop",
    L"duration_slide", L"duration_fall",   L"duration_warp",  L"duration_squash",
    L"duration_rollup", L"duration_swirl"
};

struct GhostAnimData {
    HWND hRealWnd;
    HBITMAP hBitmap;
    RECT targetRect;
    int width;
    int height;
    int targetDockX; // The dynamically learned icon position
    BOOL isRising;
    int mode;
    int durationMs;
    LONG_PTR originalExStyle;
};

// --- THE VAULTS ---
std::unordered_map<HWND, HBITMAP> g_SnapshotCache;
std::unordered_map<HWND, int> g_IconPositions; // Remembers where icons live
std::mutex g_CacheMutex;

// --- SETTINGS ---
std::atomic<bool> g_enabled{true};
std::atomic<int>  g_mode{MODE_GENIE};
std::atomic<int>  g_durations[MODE_COUNT];

void LoadSettings() {
    g_enabled.store(Wh_GetIntSetting(L"enabled") != 0, std::memory_order_relaxed);

    for (int i = 0; i < MODE_COUNT; i++) {
        int ms = Wh_GetIntSetting(kDurKeys[i]);
        if (ms < 50) ms = 50;
        if (ms > 3000) ms = 3000;
        g_durations[i].store(ms, std::memory_order_relaxed);
    }

    int mode = MODE_GENIE;
    PCWSTR style = Wh_GetStringSetting(L"style");
    if (style) {
        for (int i = 0; i < MODE_COUNT; i++) {
            if (wcscmp(style, kModeKeys[i]) == 0) { mode = i; break; }
        }
        Wh_FreeStringSetting(style);
    }
    g_mode.store(mode, std::memory_order_relaxed);
}

void SetDwmTransitions(HWND hWnd, BOOL enable) {
    BOOL disable = !enable;
    DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
}

// -------------------------------------------------------------------------
// Easing helpers
// -------------------------------------------------------------------------
static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float easeInCubic(float t)  { return t * t * t; }
static inline float easeOutCubic(float t) { float u = 1.0f - t; return 1.0f - u * u * u; }
static inline float easeInOutCubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

// -------------------------------------------------------------------------
// Per-frame effect solver.
// t runs 0 (full window) -> 1 (fully minimized/gone). The animation thread
// reverses t for restore, so every effect is authored once and plays backwards
// for free. Fills newW/newH (size), currentX/currentY (top-left) and alphaFloat.
// -------------------------------------------------------------------------
static void SolveFrame(const GhostAnimData* d, float t,
                       int SW, int SH, float taskbarY,
                       int capW, int capH,
                       int* outW, int* outH, int* outX, int* outY, float* outAlpha) {
    const int   w = d->width;
    const int   h = d->height;
    const int   L = d->targetRect.left;
    const int   T = d->targetRect.top;
    const float cx = L + w * 0.5f;
    const float cy = T + h * 0.5f;
    const int   dockX = d->targetDockX;

    float scaleX = 1.0f, scaleY = 1.0f;
    float fx = cx, fy = cy;   // desired CENTER of the frame (default: in place)
    float alpha = 1.0f;
    bool  anchorTopLeft = false; // if true, fx/fy are treated as the TOP-LEFT instead

    switch (d->mode) {

    case MODE_GENIE: {
        // Verbatim math from the original mod — the proven Magic Lamp look.
        float invT  = 1.0f - t;
        float moveX = 1.0f - (invT * invT * invT * invT * invT * invT);
        float moveY = (0.70f * (t * t)) + (0.10f * t);
        scaleX = 1.0f - (0.95f * (1.8f * t));
        if (scaleX < 0.05f) scaleX = 0.05f;
        scaleY = 1.0f - (0.70f * (t * t));

        float startCenterX = cx;
        float startY       = (float)T;
        float targetDockY  = (float)(SH + h);
        float curCenterX   = startCenterX + ((float)dockX - startCenterX) * moveX;
        float curTopY      = startY + (targetDockY - startY) * moveY;
        // Genie positions by top (not center); express that via anchorTopLeft.
        anchorTopLeft = true;
        fx = curCenterX;   // still a center for X; handled below
        fy = curTopY;      // top for Y
        if (t > 0.6f) alpha = 1.0f - ((t - 0.6f) / 0.4f);
        break;
    }

    case MODE_VACUUM: {
        float e = easeInCubic(t);
        float s = 1.0f - 0.97f * e;
        if (s < 0.03f) s = 0.03f;
        scaleX = scaleY = s;
        fx = cx + (dockX - cx) * e;
        fy = cy + (taskbarY - cy) * e;
        if (t > 0.75f) alpha = 1.0f - ((t - 0.75f) / 0.25f);
        break;
    }

    case MODE_GLIDE: {
        float e = easeOutCubic(t);
        scaleX = scaleY = 1.0f - 0.12f * e;
        alpha = 1.0f - e;
        break;
    }

    case MODE_POP: {
        float e = easeOutCubic(t);
        scaleX = scaleY = 1.0f + 0.18f * e;
        alpha = 1.0f - e;
        break;
    }

    case MODE_SLIDE: {
        float e = easeInCubic(t);
        anchorTopLeft = true;
        fx = (float)L;
        fy = T + (SH - T + 5) * e;   // top travels to just past the bottom edge
        if (t > 0.70f) alpha = 1.0f - ((t - 0.70f) / 0.30f);
        break;
    }

    case MODE_FALL: {
        float e = t * t;             // gravitational acceleration
        scaleX = 1.0f - 0.10f * t;
        scaleY = 1.0f + 0.20f * t;   // stretches as it speeds up
        float drift = (w * 0.15f) * sinf(t * 3.0f);
        anchorTopLeft = true;
        fx = L + drift;
        fy = T + (SH - T + h) * e;
        if (t > 0.60f) alpha = 1.0f - ((t - 0.60f) / 0.40f);
        break;
    }

    case MODE_WARP: {
        // Phase 1 (0..0.6): squeeze horizontally into a thin vertical beam.
        // Phase 2 (0.6..1): the beam shoots up off the top and fades.
        float ramp = t / 0.6f; if (ramp > 1.0f) ramp = 1.0f;
        scaleX = 1.0f - 0.96f * ramp;
        scaleY = 1.0f;
        anchorTopLeft = true;
        fx = cx;             // X center, resolved to left below
        fy = (float)T;       // top
        if (t > 0.6f) {
            float up  = (t - 0.6f) / 0.4f;
            float upE = easeInCubic(up);
            scaleY = 1.0f - 0.40f * up;
            fy = T - (cy + h) * upE;
            alpha = 1.0f - up;
        }
        break;
    }

    case MODE_SQUASH: {
        float e = easeInCubic(t);
        scaleY = 1.0f - 0.97f * e;
        scaleX = 1.0f + 0.10f * e;
        anchorTopLeft = true;
        fx = cx;                       // X center, resolved below
        fy = T + (taskbarY - T) * e;   // top sinks toward the taskbar
        if (t > 0.75f) alpha = 1.0f - ((t - 0.75f) / 0.25f);
        break;
    }

    case MODE_ROLLUP: {
        float e = easeInOutCubic(t);
        scaleY = 1.0f - e;             // height collapses, top stays anchored
        scaleX = 1.0f;
        anchorTopLeft = true;
        fx = (float)L;
        fy = (float)T;
        if (t > 0.85f) alpha = 1.0f - ((t - 0.85f) / 0.15f);
        break;
    }

    case MODE_SWIRL: {
        float e = easeInCubic(t);
        scaleX = scaleY = 1.0f - 0.95f * e;
        float baseCX = cx + (dockX - cx) * e;
        float baseCY = cy + (taskbarY - cy) * e;
        float amp = (w * 0.40f) * (1.0f - t);
        fx = baseCX + amp * sinf(t * 18.0f);
        fy = baseCY + amp * 0.5f * cosf(t * 18.0f);
        if (t > 0.70f) alpha = 1.0f - ((t - 0.70f) / 0.30f);
        break;
    }

    default:
        break;
    }

    int newW = (int)(w * scaleX);
    int newH = (int)(h * scaleY);
    if (newW < 2) newW = 2;
    if (newH < 1) newH = 1;
    if (newW > capW) newW = capW;
    if (newH > capH) newH = capH;

    int px, py;
    if (anchorTopLeft) {
        // fy is a top; fx is a center for genie/warp/squash, a left for slide/fall/rollup.
        if (d->mode == MODE_GENIE || d->mode == MODE_WARP || d->mode == MODE_SQUASH)
            px = (int)(fx - newW / 2.0f);
        else
            px = (int)fx;
        py = (int)fy;
    } else {
        px = (int)(fx - newW / 2.0f);
        py = (int)(fy - newH / 2.0f);
    }

    *outW = newW;
    *outH = newH;
    *outX = px;
    *outY = py;
    *outAlpha = clampf(alpha, 0.0f, 1.0f);
}

// -------------------------------------------------------------------------
// Animation Thread
// -------------------------------------------------------------------------
DWORD WINAPI GhostAnimationThread(LPVOID lpParam) {
    GhostAnimData* data = (GhostAnimData*)lpParam;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int screenWidth  = GetSystemMetrics(SM_CXSCREEN);

    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    float taskbarY = (float)workArea.bottom;

    // Create the ghost without WS_VISIBLE so it isn't briefly shown as an
    // uninitialized layered window. UpdateLayeredWindow below configures it
    // before we make it visible.
    HWND hGhost = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        L"STATIC", NULL, WS_POPUP,
        data->targetRect.left, data->targetRect.top, data->width, data->height,
        NULL, NULL, NULL, NULL
    );

    // Two memory DCs:
    //   hOrigDC   - holds the captured window snapshot at original size.
    //   hScaledDC - holds a per-frame transformed copy, fed to UpdateLayeredWindow.
    // The scaled bitmap is allocated a bit LARGER than the window so effects that
    // temporarily scale up (Pop, Free Fall, Squash) don't get clipped.
    int capW = (int)(data->width  * 1.30f) + 4;
    int capH = (int)(data->height * 1.30f) + 4;

    HDC hScreenDC = GetDC(NULL);
    HDC hOrigDC   = CreateCompatibleDC(hScreenDC);
    HDC hScaledDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hScaledBitmap = CreateCompatibleBitmap(hScreenDC, capW, capH);
    HBITMAP hOldOrig   = (HBITMAP)SelectObject(hOrigDC, data->hBitmap);
    HBITMAP hOldScaled = (HBITMAP)SelectObject(hScaledDC, hScaledBitmap);
    // HALFTONE averages source pixels into each destination pixel instead of
    // picking one (COLORONCOLOR), so extreme downscales come out smooth. HALFTONE
    // requires SetBrushOrgEx per GDI docs or the resampling brush can misalign.
    SetStretchBltMode(hScaledDC, HALFTONE);
    SetBrushOrgEx(hScaledDC, 0, 0, NULL);

    // Snapshot the cached duration once. The cache is refreshed by
    // Wh_ModSettingsChanged, so user edits take effect on the next animation.
    const double totalMs = (double)data->durationMs;

    // Time-based progress synced to the DWM compose cycle via DwmFlush. Driving
    // progress by real elapsed time and gating each frame on the next compose
    // cycle means every frame we render is exactly one frame the user sees.
    LARGE_INTEGER qpcFreq, qpcStart, qpcNow;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);

    BOOL firstFrame = TRUE;
    for (;;) {
        QueryPerformanceCounter(&qpcNow);
        double elapsedMs = (qpcNow.QuadPart - qpcStart.QuadPart) * 1000.0 / qpcFreq.QuadPart;
        BOOL lastFrame = (elapsedMs >= totalMs);
        float progress = lastFrame ? 1.0f : (float)(elapsedMs / totalMs);
        float t = data->isRising ? (1.0f - progress) : progress;

        int newW, newH, currentX, currentY;
        float alphaFloat;
        SolveFrame(data, t, screenWidth, screenHeight, taskbarY, capW, capH,
                   &newW, &newH, &currentX, &currentY, &alphaFloat);
        BYTE alpha = (BYTE)(255.0f * alphaFloat);

        // Stretch the snapshot into the top-left sub-region of the scaled bitmap.
        // UpdateLayeredWindow only reads exactly (0,0)-(newW,newH) via the SIZE we
        // pass, so pixels outside that region never leak on screen.
        StretchBlt(hScaledDC, 0, 0, newW, newH,
                   hOrigDC, 0, 0, data->width, data->height, SRCCOPY);

        // Single atomic update: position + size + bitmap + per-window alpha, all
        // committed in one composition pass.
        POINT ptDst = { currentX, currentY };
        SIZE  sz    = { newW, newH };
        POINT ptSrc = { 0, 0 };
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = alpha;
        bf.AlphaFormat = 0; // source bitmap is opaque BGR, not premultiplied BGRA
        UpdateLayeredWindow(hGhost, NULL, &ptDst, &sz, hScaledDC, &ptSrc, 0, &bf, ULW_ALPHA);

        if (firstFrame) {
            ShowWindow(hGhost, SW_SHOWNOACTIVATE);
            firstFrame = FALSE;
        }

        if (lastFrame) break;

        // Block until the next DWM compose cycle - the vsync sync point.
        DwmFlush();
    }

    if (data->isRising) {
        SetLayeredWindowAttributes(data->hRealWnd, 0, 255, LWA_ALPHA);
        if (!(data->originalExStyle & WS_EX_LAYERED)) {
            SetWindowLongPtrW(data->hRealWnd, GWL_EXSTYLE, data->originalExStyle);
        }
    }

    // Re-enable DWM transitions on the real window now that our animation is done,
    // otherwise transitions stay force-disabled permanently for any window we touch.
    SetDwmTransitions(data->hRealWnd, TRUE);

    SelectObject(hScaledDC, hOldScaled);
    SelectObject(hOrigDC, hOldOrig);
    DeleteObject(hScaledBitmap);
    DeleteObject(data->hBitmap);
    DeleteDC(hScaledDC);
    DeleteDC(hOrigDC);
    ReleaseDC(NULL, hScreenDC);
    DestroyWindow(hGhost);
    delete data;
    return 0;
}

// -------------------------------------------------------------------------
// Core Setup Engine & Smart Tracking Logic
// -------------------------------------------------------------------------
void StartGenieAnim(HWND hWnd, BOOL rising) {
    RECT rect;
    GetWindowRect(hWnd, &rect);
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;

    if (w <= 0 || h <= 0) return;

    // --- SMART ICON TRACKING ---
    POINT pt;
    GetCursorPos(&pt);
    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int learnedTargetX = screenWidth / 2; // Default to center

    // If the mouse is outside the main desktop area (e.g. hovering over the taskbar)
    if (!PtInRect(&workArea, pt)) {
        learnedTargetX = pt.x; // Steal the mouse X coordinate!
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        g_IconPositions[hWnd] = learnedTargetX; // Save it to the vault
    } else {
        // Mouse is on the desktop (clicking the [-] title bar button)
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        if (g_IconPositions.count(hWnd)) {
            learnedTargetX = g_IconPositions[hWnd];
        }
    }

    GhostAnimData* data = new GhostAnimData();
    data->hRealWnd = hWnd;
    data->targetRect = rect;
    data->width = w;
    data->height = h;
    data->isRising = rising;
    data->targetDockX = learnedTargetX; // Assign the learned coordinate
    data->mode = g_mode.load(std::memory_order_relaxed);
    data->durationMs = g_durations[data->mode].load(std::memory_order_relaxed);
    data->originalExStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);

    HDC hScreenDC = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    data->hBitmap = CreateCompatibleBitmap(hScreenDC, w, h);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, data->hBitmap);

    if (rising) {
        BOOL fromCache = FALSE;
        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            if (g_SnapshotCache.count(hWnd)) {
                HDC hCacheDC = CreateCompatibleDC(hScreenDC);
                HBITMAP hOldCacheBmp = (HBITMAP)SelectObject(hCacheDC, g_SnapshotCache[hWnd]);
                BitBlt(hMemDC, 0, 0, w, h, hCacheDC, 0, 0, SRCCOPY);
                SelectObject(hCacheDC, hOldCacheBmp);
                DeleteDC(hCacheDC);

                DeleteObject(g_SnapshotCache[hWnd]);
                g_SnapshotCache.erase(hWnd);
                fromCache = TRUE;
            }
        }
        if (!fromCache) {
            PrintWindow(hWnd, hMemDC, PW_CLIENTONLY | 0x00000002);
        }
    } else {
        BitBlt(hMemDC, 0, 0, w, h, hScreenDC, rect.left, rect.top, SRCCOPY);

        std::lock_guard<std::mutex> lock(g_CacheMutex);
        if (g_SnapshotCache.count(hWnd)) {
            DeleteObject(g_SnapshotCache[hWnd]);
        }
        g_SnapshotCache[hWnd] = CreateCompatibleBitmap(hScreenDC, w, h);
        HDC hCacheDC = CreateCompatibleDC(hScreenDC);
        HBITMAP hOldCacheBmp = (HBITMAP)SelectObject(hCacheDC, g_SnapshotCache[hWnd]);
        BitBlt(hCacheDC, 0, 0, w, h, hMemDC, 0, 0, SRCCOPY);
        SelectObject(hCacheDC, hOldCacheBmp);
        DeleteDC(hCacheDC);
    }

    SelectObject(hMemDC, hOldBmp);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);
    CreateThread(NULL, 0, GhostAnimationThread, data, 0, NULL);
}

// -------------------------------------------------------------------------
// Hooks
// -------------------------------------------------------------------------
BOOL WINAPI ShowWindow_Hook(HWND hWnd, int nCmdShow) {
    if (g_enabled.load(std::memory_order_relaxed)) {
        if (nCmdShow == SW_MINIMIZE || nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_SHOWMINNOACTIVE) {
            SetDwmTransitions(hWnd, FALSE);
            StartGenieAnim(hWnd, FALSE);
            return ShowWindow_Original(hWnd, nCmdShow);
        }
        else if (nCmdShow == SW_RESTORE || nCmdShow == SW_SHOWNORMAL) {
            if (IsIconic(hWnd)) {
                SetDwmTransitions(hWnd, FALSE);
                LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
                SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
                SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
                BOOL res = ShowWindow_Original(hWnd, nCmdShow);
                StartGenieAnim(hWnd, TRUE);
                return res;
            }
        }
    }
    return ShowWindow_Original(hWnd, nCmdShow);
}

LRESULT WINAPI DefWindowProcW_Hook(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (Msg == WM_DESTROY) {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        if (g_SnapshotCache.count(hWnd)) {
            DeleteObject(g_SnapshotCache[hWnd]);
            g_SnapshotCache.erase(hWnd);
        }
        if (g_IconPositions.count(hWnd)) {
            g_IconPositions.erase(hWnd);
        }
    }

    if (g_enabled.load(std::memory_order_relaxed) && Msg == WM_SYSCOMMAND) {
        UINT cmd = wParam & 0xFFF0;
        if (cmd == SC_MINIMIZE) {
            SetDwmTransitions(hWnd, FALSE);
            StartGenieAnim(hWnd, FALSE);
            return DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
        }
        else if (cmd == SC_RESTORE && IsIconic(hWnd)) {
            SetDwmTransitions(hWnd, FALSE);
            LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
            SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
            LRESULT res = DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
            StartGenieAnim(hWnd, TRUE);
            return res;
        }
    }
    return DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
}

BOOL Wh_ModInit() {
    LoadSettings();
    Wh_SetFunctionHook((void*)DefWindowProcW, (void*)DefWindowProcW_Hook, (void**)&DefWindowProcW_Original);
    Wh_SetFunctionHook((void*)ShowWindow, (void*)ShowWindow_Hook, (void**)&ShowWindow_Original);
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    std::lock_guard<std::mutex> lock(g_CacheMutex);
    for (auto& pair : g_SnapshotCache) {
        DeleteObject(pair.second);
    }
    g_SnapshotCache.clear();
    g_IconPositions.clear();
}
