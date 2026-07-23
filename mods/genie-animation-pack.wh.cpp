// ==WindhawkMod==
// @id              genie-minimize-animation-fork
// @name            Linux Animation Pack (Genie + friends)
// @description     macOS/Compiz-style minimize & restore effects: Genie, Vacuum, Glide, Pop, Slide, Free Fall, Warp, Squash, Roll-Up, Swirl.
// @version         2.1.0
// @author          lolstijl (fork)
// @github          https://github.com/lolstijl
// @include         *
// @compilerOptions -ldwmapi -lgdi32 -ld3d11 -ldxgi -ldcomp
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
- **v2.1 — GPU rendering.** The snapshot is now handed to the GPU compositor
  (DirectComposition) once, and each frame only pushes a transform + opacity. This
  is dramatically smoother on high-refresh displays (120/144/165/180+ Hz) and uses a
  fraction of the CPU the old per-frame `StretchBlt` did. If DirectComposition isn't
  available in a given process, it silently falls back to the original GDI renderer,
  so nothing breaks.
- Windows' own animation API is ancient, so a couple of effects (Warp especially) are
  clever fakes rather than true 3D — but they read great in motion. Genie is currently
  an affine (shrink + slide) approximation of the Magic Lamp; a true mesh-warped bend
  is planned next.
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
#include <d2d1.h>     // D2D_MATRIX_3X2_F used by IDCompositionMatrixTransform
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
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

template <class T> static inline void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// -------------------------------------------------------------------------
// GPU (DirectComposition) backend
//
// The heavy part of the old renderer was re-running a HALFTONE StretchBlt of
// the whole window bitmap on the CPU every single frame, then re-uploading it
// via UpdateLayeredWindow. At 180 Hz the per-frame budget is ~5.5 ms and that
// resample routinely blew past it on big windows, so frames were dropped.
//
// Every effect in this mod is really just scale + translate + fade of a static
// snapshot (even "Genie" here is an affine fake, not a real mesh bend). That
// means we can hand the snapshot to the GPU compositor ONCE as a texture and,
// each frame, only push a 3x2 matrix + an opacity value. The GPU does the
// resample for free, bilinear-filtered, at native refresh. Per-frame CPU cost
// drops to a few floats + a Commit, which never misses the frame budget.
//
// One D3D11 device + DXGI factory are created lazily per process and reused for
// every animation (device creation is tens of ms — far too slow to do per
// minimize). Each animation gets its own lightweight DirectComposition device
// so its hot loop needs no locks. If any of this fails (no GPU, locked-down
// process, older Windows), we fall back to the original GDI renderer.
// -------------------------------------------------------------------------
static std::mutex        g_gpuInitMutex;
static std::mutex        g_gpuCtxMutex;   // serializes shared D3D device/context/factory use
static bool              g_gpuInitTried = false;
static bool              g_gpuAvailable = false;
static ID3D11Device*     g_d3dDevice    = nullptr;  // shared across animation threads
static IDXGIFactory2*    g_dxgiFactory  = nullptr;  // shared

// Create (once) the shared D3D11 device + DXGI factory. Returns false if the
// GPU path can't be used in this process; caller then uses the CPU fallback.
static bool EnsureGpuDevice() {
    std::lock_guard<std::mutex> lock(g_gpuInitMutex);
    if (g_gpuInitTried) return g_gpuAvailable;
    g_gpuInitTried = true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &g_d3dDevice, &fl, nullptr);
    if (FAILED(hr)) {
        // Retry on WARP so software-rendering / GPU-less sessions still work.
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, &g_d3dDevice, &fl, nullptr);
    }
    if (FAILED(hr) || !g_d3dDevice) return false;

    // The shared device + its immediate context are touched from every animation
    // thread only during the brief per-animation setup (create texture/swapchain,
    // copy, present). Those calls are serialized with g_gpuCtxMutex; the hot
    // per-frame loop runs on a private DirectComposition device and takes no lock.

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) &&
        SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&g_dxgiFactory))) {
        g_gpuAvailable = true;
    }
    SafeRelease(adapter);
    SafeRelease(dxgiDev);

    if (!g_gpuAvailable) SafeRelease(g_d3dDevice);
    return g_gpuAvailable;
}

static void ReleaseGpuDevice() {
    std::lock_guard<std::mutex> lock(g_gpuInitMutex);
    SafeRelease(g_dxgiFactory);
    SafeRelease(g_d3dDevice);
    g_gpuAvailable = false;
    g_gpuInitTried = false;
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

// Reveal the real window (on restore) and undo the force-disabled DWM
// transitions. Runs once per animation, after the render loop, while the ghost
// still shows the final full-size frame so there's no gap under the real window.
static void FinalizeRealWindow(GhostAnimData* data) {
    if (data->isRising) {
        SetLayeredWindowAttributes(data->hRealWnd, 0, 255, LWA_ALPHA);
        if (!(data->originalExStyle & WS_EX_LAYERED)) {
            SetWindowLongPtrW(data->hRealWnd, GWL_EXSTYLE, data->originalExStyle);
        }
    }
    SetDwmTransitions(data->hRealWnd, TRUE);
}

// -------------------------------------------------------------------------
// CPU renderer (fallback). Original GDI StretchBlt + UpdateLayeredWindow path,
// used when the GPU/DirectComposition path is unavailable in this process.
// hGhost must be a WS_EX_LAYERED popup sized to the window rect.
// -------------------------------------------------------------------------
static void RunCpuAnim(GhostAnimData* data, HWND hGhost,
                       int screenWidth, int screenHeight, float taskbarY) {
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

    const double totalMs = (double)data->durationMs;
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

        StretchBlt(hScaledDC, 0, 0, newW, newH,
                   hOrigDC, 0, 0, data->width, data->height, SRCCOPY);

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
        DwmFlush(); // block until the next DWM compose cycle - the vsync sync point
    }

    FinalizeRealWindow(data);

    SelectObject(hScaledDC, hOldScaled);
    SelectObject(hOrigDC, hOldOrig);
    DeleteObject(hScaledBitmap);
    DeleteDC(hScaledDC);
    DeleteDC(hOrigDC);
    ReleaseDC(NULL, hScreenDC);
}

// -------------------------------------------------------------------------
// GPU renderer (DirectComposition). Uploads the snapshot to the GPU once, then
// per frame only pushes a 3x2 transform + an opacity value and commits. Returns
// false if any setup step fails, so the caller can fall back to the CPU path.
// hGhost must be a full-screen WS_EX_NOREDIRECTIONBITMAP popup.
// -------------------------------------------------------------------------
static bool RunGpuAnim(GhostAnimData* data, HWND hGhost,
                       int screenWidth, int screenHeight, float taskbarY) {
    const int w = data->width;
    const int h = data->height;

    // 1. Pull the snapshot out as top-down 32-bit BGRA and force it opaque
    //    (the snapshot carries no meaningful alpha; premultiplied-opaque = same RGB).
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;      // negative = top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const size_t stride = (size_t)w * 4;
    BYTE* bits = (BYTE*)malloc(stride * (size_t)h);
    if (!bits) return false;

    HDC hScreenDC = GetDC(NULL);
    int scan = GetDIBits(hScreenDC, data->hBitmap, 0, h, bits, &bmi, DIB_RGB_COLORS);
    ReleaseDC(NULL, hScreenDC);
    if (scan == 0) { free(bits); return false; }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) bits[i * 4 + 3] = 0xFF;

    // 2. Everything that touches the shared D3D device/context/factory is done
    //    under one lock; it runs once per animation and is off the hot path.
    IDXGISwapChain1*             swapChain   = nullptr;
    IDXGIDevice*                 dxgiDev     = nullptr;
    IDCompositionDesktopDevice*  dcompDevice = nullptr;
    IDCompositionTarget*         dcompTarget = nullptr;
    IDCompositionVisual2*        visual      = nullptr;
    IDCompositionEffectGroup*    effectGroup = nullptr;   // carries opacity (pre-Visual3)
    IDCompositionMatrixTransform* xform      = nullptr;
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock(g_gpuCtxMutex);

        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width  = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA srd;
        ZeroMemory(&srd, sizeof(srd));
        srd.pSysMem = bits;
        srd.SysMemPitch = (UINT)stride;

        ID3D11Texture2D* srcTex = nullptr;
        HRESULT hr = g_d3dDevice->CreateTexture2D(&td, &srd, &srcTex);
        if (SUCCEEDED(hr) && srcTex) {
            DXGI_SWAP_CHAIN_DESC1 scd;
            ZeroMemory(&scd, sizeof(scd));
            scd.Width  = w;
            scd.Height = h;
            scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            scd.SampleDesc.Count = 1;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.BufferCount = 2;
            scd.Scaling    = DXGI_SCALING_STRETCH;
            scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            scd.AlphaMode  = DXGI_ALPHA_MODE_PREMULTIPLIED;

            hr = g_dxgiFactory->CreateSwapChainForComposition(g_d3dDevice, &scd, nullptr, &swapChain);
            if (SUCCEEDED(hr) && swapChain) {
                ID3D11Texture2D* backBuf = nullptr;
                if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf)) && backBuf) {
                    ID3D11DeviceContext* ctx = nullptr;
                    g_d3dDevice->GetImmediateContext(&ctx);
                    if (ctx) {
                        ctx->CopyResource(backBuf, srcTex);
                        ctx->Release();
                    }
                    backBuf->Release();
                }
                DXGI_PRESENT_PARAMETERS pp;
                ZeroMemory(&pp, sizeof(pp));
                swapChain->Present1(0, 0, &pp);
            }
        }
        SafeRelease(srcTex);

        if (swapChain &&
            SUCCEEDED(g_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) &&
            SUCCEEDED(DCompositionCreateDevice2(dxgiDev, __uuidof(IDCompositionDesktopDevice), (void**)&dcompDevice)) &&
            SUCCEEDED(dcompDevice->CreateTargetForHwnd(hGhost, TRUE, &dcompTarget)) &&
            SUCCEEDED(dcompDevice->CreateVisual(&visual)) &&
            SUCCEEDED(dcompDevice->CreateMatrixTransform(&xform)) &&
            SUCCEEDED(dcompDevice->CreateEffectGroup(&effectGroup))) {
            visual->SetContent(swapChain);
            visual->SetTransform(xform);
            visual->SetEffect(effectGroup);   // opacity is animated on the effect group
            dcompTarget->SetRoot(visual);
            ok = true;
        }
        SafeRelease(dxgiDev);
    }

    free(bits);

    if (!ok) {
        SafeRelease(xform);
        SafeRelease(effectGroup);
        SafeRelease(visual);
        SafeRelease(dcompTarget);
        SafeRelease(dcompDevice);
        SafeRelease(swapChain);
        return false;
    }

    // 3. Hot loop: compute the affine transform for this frame and commit. The
    //    GPU resamples the snapshot; per-frame CPU cost is a handful of floats.
    const int capW = (int)(w * 1.30f) + 4;
    const int capH = (int)(h * 1.30f) + 4;
    const double totalMs = (double)data->durationMs;
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

        int newW, newH, curX, curY;
        float alphaFloat;
        SolveFrame(data, t, screenWidth, screenHeight, taskbarY, capW, capH,
                   &newW, &newH, &curX, &curY, &alphaFloat);

        // Map the w x h snapshot onto (curX,curY)-(curX+newW,curY+newH). The ghost
        // window spans the whole screen at (0,0), so screen coords == visual coords.
        float sx = (w > 0) ? (float)newW / (float)w : 1.0f;
        float sy = (h > 0) ? (float)newH / (float)h : 1.0f;
        D2D_MATRIX_3X2_F m;
        m._11 = sx;          m._12 = 0.0f;
        m._21 = 0.0f;        m._22 = sy;
        m._31 = (float)curX; m._32 = (float)curY;

        xform->SetMatrix(m);
        effectGroup->SetOpacity(clampf(alphaFloat, 0.0f, 1.0f));
        dcompDevice->Commit();

        if (firstFrame) {
            ShowWindow(hGhost, SW_SHOWNOACTIVATE);
            firstFrame = FALSE;
        }

        if (lastFrame) break;
        DwmFlush(); // pace to the compose cycle; per-frame work is trivial now
    }

    // Reveal the real window while the ghost still shows the final frame, then
    // tear down the composition.
    FinalizeRealWindow(data);

    SafeRelease(xform);
    SafeRelease(effectGroup);
    SafeRelease(visual);
    SafeRelease(dcompTarget);
    SafeRelease(dcompDevice);
    SafeRelease(swapChain);
    return true;
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

    bool useGpu = EnsureGpuDevice();
    HWND hGhost = NULL;

    if (useGpu) {
        // No redirection bitmap: DirectComposition owns every pixel. Full-screen
        // so content can travel anywhere without being clipped to the window rect.
        hGhost = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            L"STATIC", NULL, WS_POPUP,
            0, 0, screenWidth, screenHeight, NULL, NULL, NULL, NULL);

        if (!hGhost || !RunGpuAnim(data, hGhost, screenWidth, screenHeight, taskbarY)) {
            useGpu = false;
            if (hGhost) { DestroyWindow(hGhost); hGhost = NULL; }
        }
    }

    if (!useGpu) {
        // Layered popup sized to the window rect, shown once configured.
        hGhost = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
            L"STATIC", NULL, WS_POPUP,
            data->targetRect.left, data->targetRect.top, data->width, data->height,
            NULL, NULL, NULL, NULL);
        RunCpuAnim(data, hGhost, screenWidth, screenHeight, taskbarY);
    }

    if (hGhost) DestroyWindow(hGhost);
    DeleteObject(data->hBitmap);
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
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        for (auto& pair : g_SnapshotCache) {
            DeleteObject(pair.second);
        }
        g_SnapshotCache.clear();
        g_IconPositions.clear();
    }
    // Any in-flight animation threads own their own DirectComposition devices and
    // will finish shortly; only the shared D3D device + factory live here.
    ReleaseGpuDevice();
}
