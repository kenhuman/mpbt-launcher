// ddraw.cpp — DirectDraw shim for MPBT Solaris
//
// Drop this DLL as "ddraw.dll" alongside MPBTWIN.EXE.  Windows DLL search
// order means the application directory is checked before System32, so our
// shim is loaded in place of the real DirectDraw.
//
// Strategy:
//   • Export DirectDrawCreate (the only ddraw import MPBTWIN.EXE uses).
//   • Return a fake IDirectDraw COM object that never touches the real display.
//   • SetCooperativeLevel — keep the game's own HWND; restyle it as a normal
//     overlapped window, subclass its WndProc so we can repaint on WM_PAINT.
//   • SetDisplayMode — resize the window to the requested resolution; create
//     a top-down 8-bpp DIB section for rendering.
//   • CreateSurface — primary+backbuffer share one DIB. Offscreen surfaces
//     each get their own private DIB section.
//   • IDirectDrawSurface::Flip / Blt / BltFast — BitBlt the DIB to the window DC.
//   • IDirectDrawPalette::SetEntries — SetDIBColorTable so 8-bpp → 32-bpp
//     conversion is handled automatically by GDI at blit time.
//
// Build: 32-bit DLL targeting x86 (MPBTWIN.EXE is 32-bit).
//        See build.bat.

#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0300
#include <windows.h>
// initguid.h causes GUIDs to be defined inline (no dxguid.lib required)
#include <initguid.h>
#include <ddraw.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>

// ============================================================================
// DibBuf — a GDI DIB section used as a software framebuffer
// ============================================================================

struct DibBuf {
    HDC     hdc   = nullptr;
    HBITMAP hbmp  = nullptr;
    void*   bits  = nullptr;  // raw pixel pointer (top-down, no padding for 8bpp at 640)
    int     pitch = 0;        // row stride in bytes (aligned to 4)
    int     w     = 0;
    int     h     = 0;
    int     bpp   = 0;

    bool create(int width, int height, int bitsPerPixel) {
        w = width; h = height; bpp = bitsPerPixel;
        pitch = (width * bitsPerPixel / 8 + 3) & ~3;

        // BITMAPINFO with room for 256 palette entries
        const int infoSize = sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD);
        BITMAPINFO* bi = static_cast<BITMAPINFO*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, infoSize));
        if (!bi) return false;

        bi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi->bmiHeader.biWidth       = width;
        bi->bmiHeader.biHeight      = -height;  // top-down
        bi->bmiHeader.biPlanes      = 1;
        bi->bmiHeader.biBitCount    = (WORD)bitsPerPixel;
        bi->bmiHeader.biCompression = BI_RGB;
        if (bitsPerPixel == 8) bi->bmiHeader.biClrUsed = 256;

        HDC sdc = GetDC(nullptr);
        hdc  = CreateCompatibleDC(sdc);
        hbmp = CreateDIBSection(sdc, bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, sdc);
        HeapFree(GetProcessHeap(), 0, bi);

        if (!hbmp || !hdc) { destroy(); return false; }
        SelectObject(hdc, hbmp);
        return true;
    }

    void destroy() {
        if (hbmp) { DeleteObject(hbmp); hbmp = nullptr; }
        if (hdc)  { DeleteDC(hdc);      hdc  = nullptr; }
        bits = nullptr; pitch = w = h = bpp = 0;
    }

    void updatePalette(const RGBQUAD* pal, DWORD start, DWORD count) {
        if (hdc) SetDIBColorTable(hdc, start, count,
                                  const_cast<RGBQUAD*>(pal) + start);
    }
};

// ============================================================================
// Debug log — writes to C:\MPBT\ddraw_shim.log + OutputDebugString
// Remove the file target once the rendering issue is resolved.
// ============================================================================

static void DbLog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    OutputDebugStringA("[ddraw_shim] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    FILE* f = fopen("C:\\MPBT\\ddraw_shim.log", "a+");
    if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
}

// ============================================================================
// Globals
// ============================================================================

static HWND    g_hwnd  = nullptr;
static int     g_dispW = 640;
static int     g_dispH = 480;
static int     g_bpp   = 8;

// Target window dimensions read from ddraw.ini (0 = use game resolution)
static int     g_targetW = 0;
static int     g_targetH = 0;
// True when the window should cover the full monitor without chrome
static bool    g_borderlessFullscreen = false;

// Shared back-buffer DIB (primary + back surface both refer here)
static DibBuf  g_backDib;

// Global 8-bpp palette (kept in sync with every FakePalette::SetEntries call)
static RGBQUAD g_pal[256];

// Original WndProc of the game window (set when we subclass it)
static WNDPROC g_origWndProc = nullptr;

// ============================================================================
// Config reader — parses ddraw.ini placed alongside the game EXE
// ============================================================================

static void ReadConfig() {
    // Derive ini path from the game process EXE directory.
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exePath, sizeof(exePath));
    char* lastSep = strrchr(exePath, '\\');
    if (!lastSep) return;
    *(lastSep + 1) = '\0'; // keep trailing backslash

    char cfgPath[MAX_PATH];
    _snprintf(cfgPath, sizeof(cfgPath) - 1, "%sddraw.ini", exePath);
    cfgPath[sizeof(cfgPath) - 1] = '\0';

    char modeBuf[64] = {};
    GetPrivateProfileStringA("display", "mode", "", modeBuf, sizeof(modeBuf), cfgPath);
    if (_stricmp(modeBuf, "fullscreen-window") == 0) {
        g_borderlessFullscreen = true;
        g_targetW = GetSystemMetrics(SM_CXSCREEN);
        g_targetH = GetSystemMetrics(SM_CYSCREEN);
        DbLog("ReadConfig: fullscreen-window %dx%d", g_targetW, g_targetH);
        return;
    }

    char wBuf[32] = {}, hBuf[32] = {};
    GetPrivateProfileStringA("display", "width",  "0", wBuf, sizeof(wBuf), cfgPath);
    GetPrivateProfileStringA("display", "height", "0", hBuf, sizeof(hBuf), cfgPath);
    g_targetW = atoi(wBuf);
    g_targetH = atoi(hBuf);
    if (g_targetW > 0 && g_targetH > 0)
        DbLog("ReadConfig: windowed %dx%d", g_targetW, g_targetH);
    else
        DbLog("ReadConfig: no scaling (game resolution)");
}

// ============================================================================
// Helpers
// ============================================================================

// Render the current back-buffer to an arbitrary DC, scaling to (winW x winH)
// with aspect-correct letterboxing (black bars) if needed.
static void RenderToHDC(HDC wdc, int winW, int winH) {
    if (!g_backDib.hdc) return;
    int srcW = g_backDib.w, srcH = g_backDib.h;
    if (winW <= 0) winW = srcW;
    if (winH <= 0) winH = srcH;

    if (winW == srcW && winH == srcH) {
        // 1:1 — plain fast copy
        BitBlt(wdc, 0, 0, srcW, srcH, g_backDib.hdc, 0, 0, SRCCOPY);
        return;
    }

    // Aspect-correct scale
    double scaleX = (double)winW / srcW;
    double scaleY = (double)winH / srcH;
    double scale  = (scaleX < scaleY) ? scaleX : scaleY;
    int dstW = (int)(srcW * scale);
    int dstH = (int)(srcH * scale);
    int dstX = (winW - dstW) / 2;
    int dstY = (winH - dstH) / 2;

    // Fill letterbox bars with black
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (dstX > 0) {
        RECT r1 = {0, 0, dstX, winH};
        RECT r2 = {dstX + dstW, 0, winW, winH};
        FillRect(wdc, &r1, black);
        FillRect(wdc, &r2, black);
    }
    if (dstY > 0) {
        RECT r1 = {0, 0, winW, dstY};
        RECT r2 = {0, dstY + dstH, winW, winH};
        FillRect(wdc, &r1, black);
        FillRect(wdc, &r2, black);
    }

    SetStretchBltMode(wdc, HALFTONE);
    SetBrushOrgEx(wdc, 0, 0, nullptr);
    StretchBlt(wdc, dstX, dstY, dstW, dstH,
               g_backDib.hdc, 0, 0, srcW, srcH, SRCCOPY);
}

// Known addresses inside MPBTWIN.EXE (.data segment, image base 0x00400000)
#define GAME_FLAGS_ADDR    ((volatile DWORD*)0x0047a7c8)  // bit0=render enabled, bit1=quit
#define GAME_STATE_ADDR    ((volatile DWORD*)0x0047d05c)  // 0-2=no render, 3=lobby, 4=battle
#define GAME_GUARD_ADDR    ((volatile DWORD*)0x0047ef60)  // state-4 guard (bit0 must be 1)
#define GAME_SPRITES_ADDR  ((volatile DWORD*)0x004f669c)  // active sprite count (state 3)

static void BlitToWindow() {
    if (!g_hwnd || !g_backDib.hdc) return;
    static int s_btwCount = 0;
    ++s_btwCount;
    if (s_btwCount % 50 == 0) {
        DWORD flags   = *GAME_FLAGS_ADDR;
        DWORD state   = *GAME_STATE_ADDR;
        DWORD guard   = *GAME_GUARD_ADDR;
        DWORD sprites = *GAME_SPRITES_ADDR;
        BYTE  cpx = 0;
        if (g_backDib.bits && g_backDib.w > 1 && g_backDib.h > 1)
            cpx = ((BYTE*)g_backDib.bits)[(g_backDib.h/2)*g_backDib.pitch + g_backDib.w/2];
        DbLog("BTW #%d  state=%d flags=0x%08x guard=0x%x sprites=%d prim_center=0x%02x",
              s_btwCount, (int)state, (unsigned)flags, (unsigned)guard, (int)sprites, (unsigned)cpx);
    }
    int winW = (g_targetW > 0) ? g_targetW : g_backDib.w;
    int winH = (g_targetH > 0) ? g_targetH : g_backDib.h;
    HDC wdc = GetDC(g_hwnd);
    RenderToHDC(wdc, winW, winH);
    ReleaseDC(g_hwnd, wdc);
}

// Bit 1 of the flags word is the WinMain quit flag.
#define GAME_FLAGS_ADDR_QUIT_BIT 0x02

// Subclass WndProc: handle WM_PAINT from our DIB; suppress WM_ACTIVATEAPP
// deactivation so the game's rendering-enable bit (bit 0) stays set in
// windowed mode; force-quit when the window is destroyed.
static LRESULT CALLBACK ShimWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        int winW = (g_targetW > 0) ? g_targetW : g_backDib.w;
        int winH = (g_targetH > 0) ? g_targetH : g_backDib.h;
        RenderToHDC(hdc, winW, winH);
        EndPaint(hwnd, &ps);
        return 0;
    }
    // WM_ACTIVATEAPP (0x001C): game clears bit 0 of flags on deactivation,
    // which silently blocks all sprite blits.  Convert FALSE -> TRUE so the
    // game always thinks it is the active app in windowed mode.
    if (msg == WM_ACTIVATEAPP && wp == FALSE) {
        DbLog("WM_ACTIVATEAPP(FALSE) suppressed -> TRUE");
        wp = TRUE;
    }
    // WM_ACTIVATE (0x0006): same guard, different message.
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
        DbLog("WM_ACTIVATE(WA_INACTIVE) suppressed -> WA_ACTIVE");
        wp = (wp & 0xFFFF0000) | WA_ACTIVE;
    }
    // WM_DESTROY: force the game's quit flag so the WinMain loop exits.
    if (msg == WM_DESTROY) {
        DbLog("WM_DESTROY: setting game quit flag");
        *GAME_FLAGS_ADDR |= GAME_FLAGS_ADDR_QUIT_BIT;
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

// ============================================================================
// FakePalette — IDirectDrawPalette
// ============================================================================

class FakePalette : public IDirectDrawPalette {
    LONG m_ref = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDirectDrawPalette) {
            *pp = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDWORD lpdw) override {
        if (lpdw) *lpdw = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEntries(DWORD, DWORD start, DWORD count,
                                         LPPALETTEENTRY ppe) override {
        for (DWORD i = 0; i < count && start + i < 256; i++) {
            ppe[i].peRed   = g_pal[start + i].rgbRed;
            ppe[i].peGreen = g_pal[start + i].rgbGreen;
            ppe[i].peBlue  = g_pal[start + i].rgbBlue;
            ppe[i].peFlags = 0;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override {
        return DDERR_ALREADYINITIALIZED;
    }
    HRESULT STDMETHODCALLTYPE SetEntries(DWORD, DWORD start, DWORD count,
                                         LPPALETTEENTRY ppe) override {
        static int s_palCount = 0;
        ++s_palCount;
        for (DWORD i = 0; i < count && start + i < 256; i++) {
            g_pal[start + i].rgbRed      = ppe[i].peRed;
            g_pal[start + i].rgbGreen    = ppe[i].peGreen;
            g_pal[start + i].rgbBlue     = ppe[i].peBlue;
            g_pal[start + i].rgbReserved = 0;
        }
        // Update the primary/back DIB colour table AND log the first few calls
        // so we can confirm palette updates happen after splash.
        g_backDib.updatePalette(g_pal, start, count);
        if (s_palCount <= 3)
            DbLog("SetEntries #%d start=%d count=%d rgb0=(%d,%d,%d) rgb17=(%d,%d,%d)",
                  s_palCount, (int)start, (int)count,
                  g_pal[0].rgbRed, g_pal[0].rgbGreen, g_pal[0].rgbBlue,
                  g_pal[17].rgbRed, g_pal[17].rgbGreen, g_pal[17].rgbBlue);
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return S_OK;
    }
};

// ============================================================================
// FakeSurface — IDirectDrawSurface (v1)
// ============================================================================

class FakeSurface : public IDirectDrawSurface {
    LONG         m_ref       = 1;
    bool         m_isPrimary = false;
    bool         m_isBack    = false;
    FakeSurface* m_attached  = nullptr; // back buffer attached to primary
    DibBuf*      m_dib       = nullptr; // points to g_backDib or m_ownDib
    DibBuf       m_ownDib;             // used only for offscreen surfaces
    FakePalette* m_palette   = nullptr;

    DibBuf& dib() { return *m_dib; }

public:
    FakeSurface(bool isPrimary, bool isBack, int w = 0, int h = 0)
        : m_isPrimary(isPrimary), m_isBack(isBack) {
        if (isPrimary || isBack) {
            m_dib = &g_backDib;
        } else {
            // Offscreen surface: allocate private DIB
            m_ownDib.create(w, h, g_bpp);
            // Apply current palette so sprites blit with the correct colours
            m_ownDib.updatePalette(g_pal, 0, 256);
            m_dib = &m_ownDib;
        }
    }

    ~FakeSurface() {
        if (m_attached) { m_attached->Release(); m_attached = nullptr; }
        if (m_palette)  { m_palette->Release();  m_palette  = nullptr; }
        if (m_dib == &m_ownDib) m_ownDib.destroy();
    }

    void SetAttached(FakeSurface* back) {
        m_attached = back;
        if (back) back->AddRef();
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDirectDrawSurface) {
            *pp = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    // ---- Core rendering ----

    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE, DWORD) override {
        BlitToWindow();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Lock(LPRECT, LPDDSURFACEDESC sd,
                                    DWORD, HANDLE) override {
        sd->dwFlags  |= DDSD_LPSURFACE | DDSD_PITCH | DDSD_WIDTH | DDSD_HEIGHT;
        sd->lpSurface = dib().bits;
        sd->lPitch    = dib().pitch;
        sd->dwWidth   = (DWORD)dib().w;
        sd->dwHeight  = (DWORD)dib().h;
        static int s_lockCount = 0;
        if (++s_lockCount <= 10)
            DbLog("Lock #%d this=%p primary=%d bits=%p",
                  s_lockCount, (void*)this, (int)m_isPrimary, dib().bits);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Unlock(LPVOID) override {
        // The game also writes pixels directly into surface memory via
        // internal fill routines (FUN_00453c28) that bypass our Blt.
        // Flushing on Unlock ensures those writes appear on screen too.
        if (m_isPrimary) BlitToWindow();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDC(HDC* phdc) override {
        *phdc = dib().hdc;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS,
                                                  LPDIRECTDRAWSURFACE* pp) override {
        if (m_attached) { m_attached->AddRef(); *pp = m_attached; return S_OK; }
        return DDERR_NOTFOUND;
    }

    // dst->Blt(dstRect, src, srcRect, ...) — copy from src into this surface.
    HRESULT STDMETHODCALLTYPE Blt(LPRECT dstR, LPDIRECTDRAWSURFACE src,
                                   LPRECT srcR, DWORD flags, LPDDBLTFX fx) override {
        // DDBLT_COLORFILL: fill rectangle with a solid colour (src == NULL)
        if (!src && fx && (flags & DDBLT_COLORFILL)) {
            int dx = dstR ? dstR->left : 0, dy = dstR ? dstR->top  : 0;
            int dw = dstR ? dstR->right - dstR->left : dib().w;
            int dh = dstR ? dstR->bottom - dstR->top  : dib().h;
            BYTE col = (BYTE)fx->dwFillColor;
            BYTE* b = (BYTE*)dib().bits;
            for (int row = dy; row < dy + dh && row < dib().h; ++row)
                memset(b + row * dib().pitch + dx, col, dw);
            if (m_isPrimary) BlitToWindow();
            return S_OK;
        }
        if (!src) return S_OK;
        FakeSurface* fs = static_cast<FakeSurface*>(src);
        int sx = srcR ? srcR->left : 0,          sy = srcR ? srcR->top  : 0;
        int sw = srcR ? srcR->right  - srcR->left : fs->dib().w;
        int sh = srcR ? srcR->bottom - srcR->top  : fs->dib().h;
        int dx = dstR ? dstR->left : 0,          dy = dstR ? dstR->top  : 0;
        // Use raw memcpy for 8-bpp copies to avoid GDI palette-translation
        // artefacts (stale colour tables on offscreen surfaces cause BitBlt
        // to map every pixel to index 0 = black via 8→8 colour matching).
        if (g_bpp == 8 && fs->dib().bits && dib().bits) {
            BYTE* dstP = (BYTE*)dib().bits + dy * dib().pitch + dx;
            BYTE* srcP = (BYTE*)fs->dib().bits + sy * fs->dib().pitch + sx;
            int   rowW = (sw < dib().w - dx) ? sw : (dib().w - dx);
            if (rowW > 0)
                for (int r = 0; r < sh && (dy + r) < dib().h; ++r, dstP += dib().pitch, srcP += fs->dib().pitch)
                    memcpy(dstP, srcP, rowW);
        } else {
            BitBlt(dib().hdc, dx, dy, sw, sh, fs->dib().hdc, sx, sy, SRCCOPY);
        }
        if (m_isPrimary) {
            static int   s_bltCount = 0;
            static void* s_lastSrc  = nullptr;
            ++s_bltCount;
            BYTE px = 0;
            int nonZero = 0;
            BYTE firstNZVal = 0; int firstNZOff = -1;
            if (fs->dib().bits && fs->dib().w > 1 && fs->dib().h > 1) {
                px = ((BYTE*)fs->dib().bits)[(fs->dib().h/2)*fs->dib().pitch + fs->dib().w/2];
                // Scan every 8th pixel to check for any non-zero content
                BYTE* b = (BYTE*)fs->dib().bits;
                int total = fs->dib().h * fs->dib().pitch;
                for (int i = 0; i < total; i += 8) {
                    if (b[i]) {
                        ++nonZero;
                        if (firstNZOff < 0) { firstNZVal = b[i]; firstNZOff = i; }
                        if (nonZero >= 64) break;
                    }
                }
            }
            // Always log when src surface changes, first 10 total, and every 200th.
            bool newSrc = (src != s_lastSrc);
            if (newSrc || s_bltCount <= 10 || s_bltCount % 200 == 0) {
                // Also dump game-internal buffer addresses for diagnostics
                DWORD mainCtx  = *(volatile DWORD*)0x0047a378;
                DWORD pxStruct = mainCtx ? *(volatile DWORD*)(mainCtx + 0x4C) : 0;
                DWORD bitsA    = pxStruct ? *(volatile DWORD*)pxStruct : 0;
                DWORD bitsB    = *(volatile DWORD*)0x004da2f0;
                DWORD surfA    = pxStruct ? *(volatile DWORD*)(pxStruct + 0x14) : 0;
                DWORD surfB    = *(volatile DWORD*)0x004da2f8;
                DWORD dispSurf = *(volatile DWORD*)0x0047a7ec;
                DbLog("Primary Blt #%d src=%p%s dst=(%d,%d %dx%d) center_px=0x%02x "
                      "nonZero=%d firstNZ@%d=0x%02x bitsA=0x%08x bitsB=0x%08x "
                      "srcBits=%p surfA=%08x surfB=%08x disp=%08x",
                      s_bltCount, src, newSrc ? " [NEW]" : "",
                      dx, dy, sw, sh, (unsigned)px,
                      nonZero, firstNZOff, (unsigned)firstNZVal,
                      (unsigned)bitsA, (unsigned)bitsB,
                      fs->dib().bits, (unsigned)surfA, (unsigned)surfB,
                      (unsigned)dispSurf);
                s_lastSrc = src;
            }
            BlitToWindow();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y,
                                       LPDIRECTDRAWSURFACE src,
                                       LPRECT srcR, DWORD) override {
        if (!src) return S_OK;
        FakeSurface* fs = static_cast<FakeSurface*>(src);
        int sx = srcR ? srcR->left : 0,          sy = srcR ? srcR->top  : 0;
        int sw = srcR ? srcR->right  - srcR->left : fs->dib().w;
        int sh = srcR ? srcR->bottom - srcR->top  : fs->dib().h;
        if (g_bpp == 8 && fs->dib().bits && dib().bits) {
            BYTE* dstP = (BYTE*)dib().bits + (int)y * dib().pitch + (int)x;
            BYTE* srcP = (BYTE*)fs->dib().bits + sy * fs->dib().pitch + sx;
            int   rowW = (sw < dib().w - (int)x) ? sw : (dib().w - (int)x);
            if (rowW > 0)
                for (int r = 0; r < sh && ((int)y + r) < dib().h; ++r, dstP += dib().pitch, srcP += fs->dib().pitch)
                    memcpy(dstP, srcP, rowW);
        } else {
            BitBlt(dib().hdc, (int)x, (int)y, sw, sh, fs->dib().hdc, sx, sy, SRCCOPY);
        }
        if (m_isPrimary) BlitToWindow();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE pPal) override {
        if (m_palette) m_palette->Release();
        m_palette = static_cast<FakePalette*>(pPal);
        if (m_palette) m_palette->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC sd) override {
        sd->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
        sd->dwWidth  = (DWORD)dib().w;
        sd->dwHeight = (DWORD)dib().h;
        sd->lPitch   = dib().pitch;
        sd->ddsCaps.dwCaps = m_isPrimary
            ? (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX)
            : (m_isBack ? DDSCAPS_BACKBUFFER : DDSCAPS_OFFSCREENPLAIN);
        sd->ddpfPixelFormat.dwSize        = sizeof(DDPIXELFORMAT);
        sd->ddpfPixelFormat.dwFlags       = (g_bpp == 8) ? DDPF_PALETTEINDEXED8 : DDPF_RGB;
        sd->ddpfPixelFormat.dwRGBBitCount = (DWORD)g_bpp;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT pf) override {
        pf->dwSize        = sizeof(DDPIXELFORMAT);
        pf->dwFlags       = (g_bpp == 8) ? DDPF_PALETTEINDEXED8 : DDPF_RGB;
        pf->dwRGBBitCount = (DWORD)g_bpp;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS c) override {
        c->dwCaps = m_isPrimary ? DDSCAPS_PRIMARYSURFACE
                  : (m_isBack  ? DDSCAPS_BACKBUFFER : DDSCAPS_OFFSCREENPLAIN);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsLost()   override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Restore()  override { return S_OK; }

    // ---- Stubs for methods we don't need to intercept ----
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE)              override { return S_OK; }
    HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT)                          override { return S_OK; }
    HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD)                 override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE)    override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID, LPDDENUMSURFACESCALLBACK) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD)                                  override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER*)                     override { return DDERR_NOCLIPPERATTACHED; }
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD, LPDDCOLORKEY)                     override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD)                                 override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG)                   override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE* pp)                  override {
        if (pp) { *pp = m_palette; if (m_palette) m_palette->AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC)            override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER)                      override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD, LPDDCOLORKEY)                     override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG)                       override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT,
                                             DWORD, LPDDOVERLAYFX)                  override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD)                          override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE)      override { return S_OK; }
};

// ============================================================================
// FakeClipper — IDirectDrawClipper (minimal; game attaches it to primary)
// ============================================================================

class FakeClipper : public IDirectDrawClipper {
    LONG m_ref  = 1;
    HWND m_hwnd = nullptr;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDirectDrawClipper) {
            *pp = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetClipList(LPRECT, LPRGNDATA, LPDWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetHWnd(HWND* pp) override { if (pp) *pp = m_hwnd; return S_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE IsClipListChanged(BOOL* pb) override { if (pb) *pb = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetClipList(LPRGNDATA, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetHWnd(DWORD, HWND hwnd) override { m_hwnd = hwnd; return S_OK; }
};

// ============================================================================
// FakeDD — IDirectDraw
// ============================================================================

class FakeDD : public IDirectDraw {
    LONG m_ref = 1;
public:

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDirectDraw) {
            *pp = this; AddRef(); return S_OK;
        }
        // IDirectDraw2 has SetDisplayMode(w,h,bpp,refresh,flags) — 5 args vs our 3.
        // Returning ourselves here would corrupt the stack on SetDisplayMode calls.
        // Return E_NOINTERFACE; well-behaved games fall back to IDirectDraw v1.
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE Compact() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE CreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* pp,
                                             IUnknown*) override {
        if (!pp) return DDERR_INVALIDPARAMS;
        *pp = new FakeClipper();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreatePalette(DWORD, LPPALETTEENTRY ppe,
                                             LPDIRECTDRAWPALETTE* pp,
                                             IUnknown*) override {
        auto* pal = new FakePalette();
        if (ppe) {
            // Seed both the local store and the DIB colour table
            for (int i = 0; i < 256; i++) {
                g_pal[i] = { ppe[i].peBlue, ppe[i].peGreen, ppe[i].peRed, 0 };
            }
            g_backDib.updatePalette(g_pal, 0, 256);
        }
        *pp = pal;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC desc,
                                             LPDIRECTDRAWSURFACE* pp,
                                             IUnknown*) override {
        bool isPrimary = (desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0;
        bool hasBack   = (desc->dwFlags & DDSD_BACKBUFFERCOUNT) &&
                         desc->dwBackBufferCount > 0;
        int  w = (desc->dwFlags & DDSD_WIDTH)  ? (int)desc->dwWidth  : g_dispW;
        int  h = (desc->dwFlags & DDSD_HEIGHT) ? (int)desc->dwHeight : g_dispH;

        if (isPrimary) {
            if (!g_backDib.hdc) g_backDib.create(g_dispW, g_dispH, g_bpp);
            auto* primary = new FakeSurface(true, false);
            if (hasBack) {
                auto* back = new FakeSurface(false, true);
                primary->SetAttached(back);
                back->Release(); // primary holds the ref
            }
            *pp = primary;
            DbLog("CreateSurface PRIMARY %p (%dx%d)", (void*)primary, g_dispW, g_dispH);
        } else {
            auto* surf = new FakeSurface(false, false, w, h);
            *pp = surf;
            static int s_csc = 0;
            if (++s_csc <= 16)
                DbLog("CreateSurface OFFSCREEN #%d %p (%dx%d) caps=0x%08x",
                      s_csc, (void*)surf, w, h, (unsigned)desc->ddsCaps.dwCaps);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE,
                                                LPDIRECTDRAWSURFACE*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID ctx,
                                                LPDDENUMMODESCALLBACK cb) override {
        if (!cb) return S_OK;
        DDSURFACEDESC sd = {};
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH | DDSD_REFRESHRATE;
        sd.dwWidth = 640; sd.dwHeight = 480; sd.dwRefreshRate = 60; sd.lPitch = 640;
        sd.ddpfPixelFormat.dwSize        = sizeof(DDPIXELFORMAT);
        sd.ddpfPixelFormat.dwFlags       = DDPF_PALETTEINDEXED8;
        sd.ddpfPixelFormat.dwRGBBitCount = 8;
        cb(&sd, ctx);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID,
                                            LPDDENUMSURFACESCALLBACK) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS hal, LPDDCAPS hel) override {
        // Report software-only caps (no hardware acceleration)
        auto fill = [](LPDDCAPS c) {
            if (!c || c->dwSize == 0) return;
            DWORD sz = c->dwSize;   // save before ZeroMemory clobbers it
            ZeroMemory(c, sz);
            c->dwSize = sz;         // restore so callers see a valid struct
            c->dwCaps = DDCAPS_BLT | DDCAPS_PALETTE;
        };
        fill(hal); fill(hel);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC sd) override {
        sd->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT |
                      DDSD_PITCH | DDSD_REFRESHRATE;
        sd->dwWidth = (DWORD)g_dispW; sd->dwHeight = (DWORD)g_dispH;
        sd->dwRefreshRate = 60;
        sd->lPitch = g_dispW * g_bpp / 8;
        sd->ddpfPixelFormat.dwSize        = sizeof(DDPIXELFORMAT);
        sd->ddpfPixelFormat.dwFlags       = DDPF_PALETTEINDEXED8;
        sd->ddpfPixelFormat.dwRGBBitCount = (DWORD)g_bpp;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD c1, LPDWORD c2) override {
        if (c1) *c1 = 0; if (c2) *c2 = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE*) override { return DDERR_NOTFOUND; }
    HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD lpdw) override { if (lpdw) *lpdw = 60; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD lpdw) override { if (lpdw) *lpdw = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL lb) override { if (lb) *lb = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(GUID*) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hwnd, DWORD) override {
        DbLog("SetCooperativeLevel hwnd=0x%p", (void*)hwnd);
        g_hwnd = hwnd;

        int wndW = (g_targetW > 0) ? g_targetW : g_dispW;
        int wndH = (g_targetH > 0) ? g_targetH : g_dispH;

        if (g_borderlessFullscreen) {
            // Borderless fullscreen: no title bar, no chrome, covers the whole monitor
            LONG style = GetWindowLongA(hwnd, GWL_STYLE);
            style &= ~(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_DLGFRAME);
            style |= WS_POPUP;
            SetWindowLongA(hwnd, GWL_STYLE, style);
            SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, wndW, wndH,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            // Normal windowed mode
            LONG style = GetWindowLongA(hwnd, GWL_STYLE);
            style &= ~(WS_POPUP | WS_DLGFRAME);
            style |= WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            SetWindowLongA(hwnd, GWL_STYLE, style);
            SetWindowLongA(hwnd, GWL_EXSTYLE, 0);

            RECT r = { 0, 0, wndW, wndH };
            AdjustWindowRect(&r, (DWORD)style, FALSE);
            SetWindowPos(hwnd, HWND_TOP, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }

        // Bring window to foreground so the game receives WM_ACTIVATEAPP(1).
        // The game's WndProc sets DAT_0047a7c8 bit 0 on that message; all
        // sprite-blit guards check that bit before calling IDirectDrawSurface::Blt.
        // Without this, bit 0 stays 0 and nothing renders after the splash screens.
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);

        // Show the cursor (games in exclusive mode typically hide it)
        ShowCursor(TRUE);
        ClipCursor(nullptr);

        // Subclass the game's WndProc so we can repaint from our DIB
        if (!g_origWndProc) {
            g_origWndProc = (WNDPROC)
                SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)ShimWndProc);
        }

        return S_OK;
    }

    // IDirectDraw v1: SetDisplayMode(width, height, bitsPerPixel)
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD bpp) override {
        DbLog("SetDisplayMode %dx%dx%d", (int)w, (int)h, (int)bpp);
        g_dispW = (int)w; g_dispH = (int)h; g_bpp = (int)bpp;

        // Only resize the window when no target scaling is configured;
        // SetCooperativeLevel already set the window to the target size.
        if (g_hwnd && g_targetW == 0) {
            LONG style = GetWindowLongA(g_hwnd, GWL_STYLE);
            RECT r = { 0, 0, (LONG)w, (LONG)h };
            AdjustWindowRect(&r, (DWORD)style, FALSE);
            SetWindowPos(g_hwnd, nullptr, 0, 0,
                         r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER);
        }

        // Recreate the back-buffer DIB with the new dimensions
        g_backDib.destroy();
        g_backDib.create(g_dispW, g_dispH, g_bpp);
        g_backDib.updatePalette(g_pal, 0, 256);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD dwFlags, HANDLE) override {
        if (dwFlags & DDWAITVB_BLOCKBEGIN) Sleep(1);
        return S_OK;
    }
};

// ============================================================================
// DLL entry point
// ============================================================================

// ============================================================================
// IAT patch — redirect Mpbtwin.exe's DirectDrawCreate import to our version.
//
// Called from DllMain when we are injected into a process that already had
// the system ddraw.dll loaded (so Windows named us something other than
// "ddraw.dll" internally). In the normal search-order load case the IAT
// already points here, so the walk is a no-op.
// ============================================================================

static void PatchGameIAT()
{
    // Identify our own module by address (not by name, which may be ambiguous
    // when both system ddraw.dll and we are both loaded).
    HMODULE hSelf = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&DirectDrawCreate, &hSelf) || !hSelf)
        return;

    // If we are already the module Windows calls "ddraw.dll" the IAT already
    // points to us — nothing to do.
    if (GetModuleHandleA("ddraw.dll") == hSelf) return;

    // Find MPBTWIN.EXE's module base.
    HMODULE hGame = GetModuleHandleA("Mpbtwin.exe");
    if (!hGame) return;

    FARPROC ourDDC = GetProcAddress(hSelf, "DirectDrawCreate");
    if (!ourDDC) return;

    // Walk the PE import table.
    BYTE* base = reinterpret_cast<BYTE*>(hGame);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impDir.VirtualAddress) return;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + impDir.VirtualAddress);

    for (; desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name),
                     "ddraw.dll") != 0)
            continue;

        auto* orig  = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->OriginalFirstThunk);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->FirstThunk);

        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + orig->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name),
                       "DirectDrawCreate") != 0)
                continue;

            // Make the IAT page writable, patch, restore.
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR),
                           PAGE_EXECUTE_READWRITE, &old);
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(ourDDC);
            VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), old, &old);
            return;
        }
        break;
    }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD fdwReason, LPVOID)
{
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DbLog("ddraw_shim loaded");
        ReadConfig();
        PatchGameIAT();
    }
    return TRUE;
}

// ============================================================================
// Exports
// ============================================================================

extern "C" {

HRESULT WINAPI DirectDrawCreate(GUID* lpGUID, LPDIRECTDRAW* lplpDD,
                                 IUnknown* pUnkOuter) {
    if (!lplpDD) return DDERR_INVALIDPARAMS;
    *lplpDD = new FakeDD();
    return S_OK;
}

HRESULT WINAPI DirectDrawCreateEx(GUID* lpGUID, void** lplpDD, REFIID iid,
                                   IUnknown* pUnkOuter) {
    LPDIRECTDRAW lpDD = nullptr;
    HRESULT hr = DirectDrawCreate(lpGUID, &lpDD, pUnkOuter);
    if (FAILED(hr)) return hr;
    hr = lpDD->QueryInterface(iid, lplpDD);
    lpDD->Release();
    return hr;
}

HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, LPDIRECTDRAWCLIPPER* pp,
                                        IUnknown*) {
    if (!pp) return DDERR_INVALIDPARAMS;
    *pp = new FakeClipper();
    return S_OK;
}

HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    if (cb) cb(nullptr, "MPBT Shim", "MPBT Windowed Shim", ctx);
    return S_OK;
}
HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW, LPVOID) { return S_OK; }
HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD) {
    if (cb) cb(nullptr, "MPBT Shim", "MPBT Windowed Shim", ctx, nullptr);
    return S_OK;
}
HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW, LPVOID, DWORD) { return S_OK; }
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, void**) { return CLASS_E_CLASSNOTAVAILABLE; }
HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }

} // extern "C"
