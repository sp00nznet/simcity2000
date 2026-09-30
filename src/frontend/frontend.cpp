/*
 * frontend.cpp - the player's window: the game's picture through a shader
 * chain, with a menu bar above it that never covers the game (after Tachyon's
 * developer bar).
 *
 * The game runs on the headless desktop exactly as in a headless run; frame.c
 * composites its picture, input.c delivers this window's mouse and keys to it.
 * What this file adds:
 *
 *   - Direct3D 11 presentation of that picture: an upscaling filter (nearest,
 *     smooth, sharp-bilinear, Scale2x), an optional glow, an optional CRT
 *     (curvature, scanlines, shadow mask, vignette), aspect-correct or integer
 *     scaling, borderless fullscreen.
 *   - The menu bar: the game's own menus, mirrored from its HMENU with live
 *     check marks (WM_INITMENUPOPUP is sent first, as Windows would), then the
 *     host's own: Graphics, Effects, Display, Debug, Cheats.
 *   - The game's pop-up menus (TrackPopupMenu: the toolbar's variants) shown
 *     as ImGui pop-ups here, since a real one would open on a desktop nobody
 *     can see.
 *
 * See docs/frontend.md.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern "C" {
#include "../runtime/fx.h"
unsigned frame_latest(uint32_t *dst, int *w, int *h, unsigned have);
double   frame_capture_ms(void);
HWND     game_frame_window(void);
void     input_live_start(void);
void     input_live_mouse(UINT msg, WPARAM keys, int x, int y);
void     input_live_key(UINT msg, WPARAM w, LPARAM l);
void     input_live_command(UINT id);
void     input_live_initmenu(HMENU sub, int index);
void     guest_lock(void);
void     guest_unlock(void);
void     frame_start(void);
void     qol_set_turbo(int factor);
int      qol_turbo(void);
int      mods_count(void);
const char *mods_name(int i);
const char *mods_description(int i);
int      mods_on(int i);
void     mods_set(int i, int on);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define GAME_FUNDS   (*(volatile int32_t *)0x004CA444)
#define GAME_DAYS    (*(volatile int32_t *)0x004CAE04)
#define GAME_WEATHER (*(volatile uint8_t *)0x004CB40C)
#define GAME_START   (*(volatile int16_t *)0x004CA5F4)

static const char *kMonth[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---------------------------------------------------------------- settings */

struct Settings {
    int   filter = 2;               /* 0 nearest, 1 smooth, 2 sharp, 3 Scale2x */
    bool  integer = false;
    bool  crt = false;
    float curve = 0.05f, scan = 0.35f, mask = 0.12f, vignette = 0.3f;
    bool  glow = false;
    float glow_k = 0.8f, glow_threshold = 0.45f;
    bool  vsync = true;
    bool  fullscreen = false;
    bool  stats = false;
    bool  wheel_zoom = true;
    int   turbo = 1;
    int   autosave_min = 0;         /* 0 = off */
    bool  hold_funds = false;
    int   held_funds = 0;
};
static Settings S;
static char g_ini[MAX_PATH];

static void load_settings() {
    auto geti = [](const char *k, int d) { return (int)GetPrivateProfileIntA("frontend", k, d, g_ini); };
    auto getf = [](const char *k, float d) {
        char b[32];
        GetPrivateProfileStringA("frontend", k, "", b, sizeof b, g_ini);
        return b[0] ? (float)atof(b) : d;
    };
    S.filter = geti("filter", S.filter);
    S.integer = geti("integer", S.integer);
    S.crt = geti("crt", S.crt);
    S.curve = getf("crt_curve", S.curve);
    S.scan = getf("crt_scanlines", S.scan);
    S.mask = getf("crt_mask", S.mask);
    S.vignette = getf("crt_vignette", S.vignette);
    S.glow = geti("glow", S.glow);
    S.glow_k = getf("glow_strength", S.glow_k);
    S.glow_threshold = getf("glow_threshold", S.glow_threshold);
    S.vsync = geti("vsync", S.vsync);
    S.wheel_zoom = geti("wheel_zoom", S.wheel_zoom);
    S.turbo = geti("turbo", S.turbo);
    S.autosave_min = geti("autosave_minutes", S.autosave_min);
    g_fx.daynight = geti("daynight", g_fx.daynight);
    g_fx.seasons = geti("seasons", g_fx.seasons);
    g_fx.weather = geti("weather", g_fx.weather);
    g_fx.cycle_days = geti("cycle_days", g_fx.cycle_days);
    g_fx.night_depth = getf("night_depth", g_fx.night_depth);
}

static void save_settings() {
    auto puti = [](const char *k, int v) { char b[32]; snprintf(b, sizeof b, "%d", v); WritePrivateProfileStringA("frontend", k, b, g_ini); };
    auto putf = [](const char *k, float v) { char b[32]; snprintf(b, sizeof b, "%.3f", v); WritePrivateProfileStringA("frontend", k, b, g_ini); };
    puti("filter", S.filter); puti("integer", S.integer); puti("crt", S.crt);
    putf("crt_curve", S.curve); putf("crt_scanlines", S.scan); putf("crt_mask", S.mask); putf("crt_vignette", S.vignette);
    puti("glow", S.glow); putf("glow_strength", S.glow_k); putf("glow_threshold", S.glow_threshold);
    puti("vsync", S.vsync);
    puti("wheel_zoom", S.wheel_zoom); puti("turbo", S.turbo); puti("autosave_minutes", S.autosave_min);
    puti("daynight", g_fx.daynight); puti("seasons", g_fx.seasons); puti("weather", g_fx.weather);
    puti("cycle_days", g_fx.cycle_days); putf("night_depth", g_fx.night_depth);
}

/* ---------------------------------------------------------------- D3D */

static const char *kShaders = R"HLSL(
cbuffer P : register(b0) {
    float2 src_size; float2 out_size;
    int filter; int crt; float curve; float scan;
    float mask; float vignette; int glow; float glow_k;
    float threshold; float2 blur_dir; float pad;
};
Texture2D src : register(t0);
Texture2D glow_tex : register(t1);
SamplerState point_s : register(s0);
SamplerState linear_s : register(s1);

struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 at(float2 p) { return src.Load(int3(clamp(p, 0, src_size - 1), 0)).rgb; }
bool same(float3 a, float3 b) { return all(abs(a - b) < 0.004); }

float3 sample_src(float2 uv) {
    if (filter == 0) return src.SampleLevel(point_s, uv, 0).rgb;
    if (filter == 1) return src.SampleLevel(linear_s, uv, 0).rgb;
    float2 tex = uv * src_size;
    if (filter == 2) {                               // sharp bilinear
        float2 scale = max(floor(out_size / src_size), 1.0);
        float2 fl = floor(tex);
        float2 c = frac(tex) - 0.5;
        float2 range = 0.5 - 0.5 / scale;
        float2 f = (c - clamp(c, -range, range)) * scale + 0.5;
        return src.SampleLevel(linear_s, (fl + f) / src_size, 0).rgb;
    }
    // Scale2x (EPX): round the staircase on diagonal edges
    float2 p = floor(tex), s = frac(tex);
    float3 P = at(p), A = at(p + float2(0, -1)), B = at(p + float2(1, 0)),
           C = at(p + float2(-1, 0)), D = at(p + float2(0, 1));
    if (s.y < 0.5) {
        if (s.x < 0.5) return (same(C, A) && !same(C, D) && !same(A, B)) ? A : P;
        return (same(A, B) && !same(A, C) && !same(B, D)) ? B : P;
    }
    if (s.x < 0.5) return (same(D, C) && !same(D, B) && !same(C, A)) ? C : P;
    return (same(B, D) && !same(B, A) && !same(D, C)) ? D : P;
}

float4 ps_final(V i) : SV_Target {
    float2 uv = i.uv;
    if (crt) {
        float2 c = uv * 2 - 1;
        c *= 1 + curve * dot(c, c);
        uv = c * 0.5 + 0.5;
        if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
    }
    float3 col = sample_src(uv);
    if (glow) col += glow_tex.SampleLevel(linear_s, uv, 0).rgb * glow_k;
    if (crt) {
        float scanline = 0.5 + 0.5 * cos(uv.y * src_size.y * 6.2831853);
        col *= lerp(1.0, 0.55 + 0.45 * scanline, scan);
        int m = (int)fmod(i.pos.x, 3.0);
        float3 tint = m == 0 ? float3(1, 1 - mask, 1 - mask) : m == 1 ? float3(1 - mask, 1, 1 - mask) : float3(1 - mask, 1 - mask, 1);
        col *= tint * (1 + mask * 0.6);
        float2 v = uv * (1 - uv.yx);
        col *= lerp(1.0, saturate(pow(v.x * v.y * 16.0, 0.25)), vignette);
    }
    return float4(col, 1);
}

// Coloured light glows; white does not. The game's dialogs, status bar and
// newspaper are white, and a plain brightness threshold bloomed them.
float4 ps_bright(V i) : SV_Target {
    float3 c = src.SampleLevel(linear_s, i.uv, 0).rgb;
    float l = dot(c, float3(0.3, 0.59, 0.11));
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    return float4(c * saturate((l - threshold) / (1 - threshold)) * saturate(sat * 2.5), 1);
}

float4 ps_blur(V i) : SV_Target {
    static const float w[5] = { 0.227, 0.194, 0.122, 0.054, 0.016 };
    float3 c = src.SampleLevel(linear_s, i.uv, 0).rgb * w[0];
    for (int k = 1; k < 5; k++) {
        float2 o = blur_dir * k * 1.5;
        c += (src.SampleLevel(linear_s, i.uv + o, 0).rgb + src.SampleLevel(linear_s, i.uv - o, 0).rgb) * w[k];
    }
    return float4(c, 1);
}
)HLSL";

struct CB {
    float src_w, src_h, out_w, out_h;
    int filter, crt; float curve, scan;
    float mask, vignette; int glow; float glow_k;
    float threshold, blur_x, blur_y, pad;
};

struct Target { ID3D11Texture2D *tex = nullptr; ID3D11RenderTargetView *rtv = nullptr; ID3D11ShaderResourceView *srv = nullptr; int w = 0, h = 0; };

static HWND g_hwnd;
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain *g_swap;
static ID3D11RenderTargetView *g_backbuffer;
static ID3D11Texture2D *g_frame_tex;
static ID3D11ShaderResourceView *g_frame_srv;
static int g_tex_w, g_tex_h;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps_final, *g_ps_bright, *g_ps_blur;
static ID3D11Buffer *g_cb;
static ID3D11SamplerState *g_point, *g_linear;
static Target g_glow_a, g_glow_b;
static std::vector<uint32_t> g_pixels(3840 * 2160);
static int g_fw, g_fh;                /* current picture size */
static RECT g_dest;                   /* where the picture sits in the client area */

static void release_target(Target &t) {
    if (t.srv) t.srv->Release();
    if (t.rtv) t.rtv->Release();
    if (t.tex) t.tex->Release();
    t = Target{};
}

static void make_target(Target &t, int w, int h) {
    if (t.w == w && t.h == h && t.tex) return;
    release_target(t);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &t.tex);
    g_dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
    g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
    t.w = w; t.h = h;
}

static ID3DBlob *compile(const char *entry, const char *profile) {
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShaders, strlen(kShaders), "frontend.hlsl", nullptr, nullptr, entry, profile, 0, 0, &code, &err))) {
        fprintf(stderr, "[frontend] shader %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

static void make_backbuffer() {
    ID3D11Texture2D *bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_backbuffer);
    bb->Release();
}

static bool init_d3d(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) {
        /* No hardware device (a virtual or remote display): WARP renders in software. */
        fprintf(stderr, "[frontend] hardware D3D11 failed (0x%08lX); trying WARP\n", (unsigned long)hr);
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, nullptr, &g_ctx);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[frontend] D3D11 failed (0x%08lX)\n", (unsigned long)hr);
        return false;
    }
    make_backbuffer();
    ID3DBlob *b = compile("vs", "vs_4_0");
    if (!b) return false;
    g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs);
    b->Release();
    struct { const char *e; ID3D11PixelShader **ps; } ps[] = { { "ps_final", &g_ps_final }, { "ps_bright", &g_ps_bright }, { "ps_blur", &g_ps_blur } };
    for (auto &p : ps) {
        if (!(b = compile(p.e, "ps_4_0"))) return false;
        g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, p.ps);
        b->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_SAMPLER_DESC ss = {};
    ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_dev->CreateSamplerState(&ss, &g_point);
    ss.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    g_dev->CreateSamplerState(&ss, &g_linear);
    return true;
}

static void upload_frame() {
    static unsigned serial;
    int w, h;
    unsigned s = frame_latest(g_pixels.data(), &w, &h, serial);
    if (!s) return;
    serial = s;
    if (w != g_tex_w || h != g_tex_h) {
        if (g_frame_srv) g_frame_srv->Release();
        if (g_frame_tex) g_frame_tex->Release();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8X8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateTexture2D(&d, nullptr, &g_frame_tex);
        g_dev->CreateShaderResourceView(g_frame_tex, nullptr, &g_frame_srv);
        g_tex_w = w; g_tex_h = h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (int y = 0; y < h; y++) memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, &g_pixels[(size_t)y * w], (size_t)w * 4);
        g_ctx->Unmap(g_frame_tex, 0);
    }
    g_fw = w;
    g_fh = h;
}

static void set_cb(float ow, float oh, float bx = 0, float by = 0) {
    D3D11_MAPPED_SUBRESOURCE m;
    g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    CB *c = (CB *)m.pData;
    *c = CB{ (float)g_tex_w, (float)g_tex_h, ow, oh, S.filter, S.crt, S.curve, S.scan, S.mask, S.vignette,
             S.glow, S.glow_k, S.glow_threshold, bx, by, 0 };
    g_ctx->Unmap(g_cb, 0);
}

static void pass(ID3D11PixelShader *ps, ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *in, ID3D11ShaderResourceView *in2,
                 float x, float y, float w, float h) {
    D3D11_VIEWPORT vp = { x, y, w, h, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ID3D11ShaderResourceView *srvs[2] = { in, in2 };
    g_ctx->PSSetShaderResources(0, 2, srvs);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none[2] = {};
    g_ctx->PSSetShaderResources(0, 2, none);
}

static void render_picture(int cw, int ch, int top) {
    if (!g_frame_srv) return;
    int aw = cw, ah = ch - top;
    if (aw <= 0 || ah <= 0) return;
    float scale = std::min((float)aw / g_tex_w, (float)ah / g_tex_h);
    if (S.integer && scale >= 1) scale = floorf(scale);
    int dw = (int)(g_tex_w * scale), dh = (int)(g_tex_h * scale);
    g_dest = { (aw - dw) / 2, top + (ah - dh) / 2, (aw - dw) / 2 + dw, top + (ah - dh) / 2 + dh };

    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ID3D11SamplerState *samp[2] = { g_point, g_linear };
    g_ctx->PSSetSamplers(0, 2, samp);

    if (S.glow) {                     /* bright pass at half size, blurred both ways, twice */
        int gw = std::max(1, g_tex_w / 2), gh = std::max(1, g_tex_h / 2);
        make_target(g_glow_a, gw, gh);
        make_target(g_glow_b, gw, gh);
        set_cb((float)gw, (float)gh);
        pass(g_ps_bright, g_glow_a.rtv, g_frame_srv, nullptr, 0, 0, (float)gw, (float)gh);
        for (int k = 0; k < 2; k++) {
            set_cb((float)gw, (float)gh, 1.0f / gw, 0);
            pass(g_ps_blur, g_glow_b.rtv, g_glow_a.srv, nullptr, 0, 0, (float)gw, (float)gh);
            set_cb((float)gw, (float)gh, 0, 1.0f / gh);
            pass(g_ps_blur, g_glow_a.rtv, g_glow_b.srv, nullptr, 0, 0, (float)gw, (float)gh);
        }
    }
    set_cb((float)dw, (float)dh);
    pass(g_ps_final, g_backbuffer, g_frame_srv, S.glow ? g_glow_a.srv : nullptr,
         (float)g_dest.left, (float)g_dest.top, (float)dw, (float)dh);
}

/* ---------------------------------------------------------------- the game's pop-up menus */

static struct {
    HMENU menu;
    volatile LONG state;              /* 0 idle, 1 asked, 2 answered */
    UINT result;
    HANDLE done;
} g_popup;

static volatile HCURSOR g_game_cursor;

static std::string menu_label(const char *raw, std::string *shortcut) {
    std::string s;
    for (const char *p = raw; *p; p++) {
        if (*p == '\t') { if (shortcut) *shortcut = p + 1; break; }
        if (*p == '&') { if (p[1] == '&') { s += '&'; p++; } continue; }
        s += *p;
    }
    return s;
}

/* One ImGui menu from a game HMENU; a click sends the command to the game. */
static void menu_items(HMENU m, bool popup_answer) {
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        char text[256] = "";
        MENUITEMINFOA mi = { sizeof mi };
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = text;
        mi.cch = sizeof text;
        if (!GetMenuItemInfoA(m, i, TRUE, &mi)) continue;
        if (mi.fType & MFT_SEPARATOR) { ImGui::Separator(); continue; }
        std::string sc, label = menu_label(text, &sc);
        label += "##" + std::to_string(i);
        bool enabled = !(mi.fState & (MFS_DISABLED | MFS_GRAYED));
        if (mi.hSubMenu) {
            if (ImGui::BeginMenu(label.c_str(), enabled)) { menu_items(mi.hSubMenu, popup_answer); ImGui::EndMenu(); }
            continue;
        }
        if (ImGui::MenuItem(label.c_str(), sc.empty() ? nullptr : sc.c_str(), (mi.fState & MFS_CHECKED) != 0, enabled)) {
            if (popup_answer) {
                g_popup.result = mi.wID;
                g_popup.state = 2;
                SetEvent(g_popup.done);
            } else {
                input_live_command(mi.wID);
            }
        }
    }
}

static void game_menus() {
    HWND frame = game_frame_window();
    HMENU bar = frame ? GetMenu(frame) : nullptr;
    if (!bar) return;
    static int open_prev = -1;
    int open_now = -1;
    int n = GetMenuItemCount(bar);
    for (int i = 0; i < n; i++) {
        char text[128] = "";
        GetMenuStringA(bar, i, text, sizeof text, MF_BYPOSITION);
        HMENU sub = GetSubMenu(bar, i);
        if (!sub || !text[0]) continue;               /* the maximized child's system-menu icons */
        std::string label = menu_label(text, nullptr) + "##game" + std::to_string(i);
        if (ImGui::BeginMenu(label.c_str())) {
            open_now = i;
            if (open_prev != i) input_live_initmenu(sub, i);
            menu_items(sub, false);
            ImGui::EndMenu();
        }
    }
    open_prev = open_now;
}

static void popup_menu() {
    if (g_popup.state == 1 && !ImGui::IsPopupOpen("##gamepopup")) ImGui::OpenPopup("##gamepopup");
    if (ImGui::BeginPopup("##gamepopup")) {
        menu_items(g_popup.menu, true);
        ImGui::EndPopup();
    } else if (g_popup.state == 1 && !ImGui::IsPopupOpen("##gamepopup")) {
        g_popup.result = 0;                           /* dismissed */
        g_popup.state = 2;
        SetEvent(g_popup.done);
    }
}

/* ---------------------------------------------------------------- the host's menus */

static const char *kWeather[] = { "Cold", "Clear", "Hot", "Foggy", "Chilly", "Overcast", "Snow", "Rain",
                                  "Windy", "Blizzard", "Hurricane", "Tornado" };

static void set_funds(int v) {
    guest_lock();
    GAME_FUNDS = v;
    guest_unlock();
}

static void set_fullscreen(bool on);

/* ---- save slots, through the game's own Save As / Load ---- */

#define SLOTS 5

/* A slot is a folder, CITIES\SLOTS\n, holding one <city>.SC2: the game names
 * a city after the file it is saved as, so a file called SLOT1.SC2 renamed
 * NYC to "SLOT1". */
static std::string slot_dir(int n) {
    char p[MAX_PATH];
    GetModuleFileNameA((HMODULE)0x00400000, p, sizeof p);   /* the game's folder */
    char *e = strrchr(p, '\\');
    if (e) e[1] = 0;
    return std::string(p) + "CITIES\\SLOTS\\" + std::to_string(n) + "\\";
}

static std::string slot_path(int n) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((slot_dir(n) + "*.SC2").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    FindClose(h);
    return slot_dir(n) + fd.cFileName;
}

/* The current city's name, from the game's title bar: "... <NYC> ...". */
static std::string city_name() {
    char t[256] = "";
    HWND frame = game_frame_window();
    if (frame) GetWindowTextA(frame, t, sizeof t);
    const char *a = strchr(t, '<'), *b = a ? strchr(a, '>') : nullptr;
    std::string n = (a && b) ? std::string(a + 1, b) : "City";
    for (char &c : n) if (strchr("\\/:*?\"|", c)) c = '_';
    return n.empty() ? "City" : n;
}

/* Empty the slot, then return where the city goes. */
static std::string slot_save_path(int n) {
    std::string d = slot_dir(n);
    CreateDirectoryA(d.substr(0, d.size() - 1 - std::to_string(n).size()).c_str(), nullptr);   /* ...\SLOTS */
    CreateDirectoryA(d.c_str(), nullptr);
    for (std::string old; !(old = slot_path(n)).empty();) DeleteFileA(old.c_str());
    return d + city_name() + ".SC2";
}

/* "NYC, 29 Sep 21:14", or empty if the slot has no city. The name is the
 * file's CNAM chunk: a big-endian length, then a length-prefixed string. */
static std::string slot_label(int n) {
    std::string path = slot_path(n);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (path.empty() || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return "";
    char name[40] = "?";
    FILE *f = fopen(path.c_str(), "rb");
    if (f) {
        unsigned char buf[4096];
        size_t got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        for (size_t i = 0; i + 9 < got; i++)
            if (!memcmp(buf + i, "CNAM", 4)) {
                int len = buf[i + 8];
                if (len > 0 && len < 32 && i + 9 + len <= got) { memcpy(name, buf + i + 9, len); name[len] = 0; }
                break;
            }
    }
    SYSTEMTIME st;
    FILETIME lt;
    FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
    FileTimeToSystemTime(&lt, &st);
    char out[96];
    snprintf(out, sizeof out, "%s, %d %s %02d:%02d", name, st.wDay, kMonth[st.wMonth - 1], st.wHour, st.wMinute);
    return out;
}

extern "C" void file_dialog_answer(int kind, const char *path);

static void saves_menu() {
    if (!ImGui::BeginMenu("Saves")) return;
    ImGui::TextDisabled("Save to");
    for (int n = 1; n <= SLOTS; n++) {
        std::string l = slot_label(n), item = "Slot " + std::to_string(n) + (l.empty() ? "" : "  (" + l + ")") + "##s" + std::to_string(n);
        if (ImGui::MenuItem(item.c_str())) {
            file_dialog_answer(1, slot_save_path(n).c_str());
            input_live_command(0x8026);                         /* File > Save City As */
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("Load (the game asks about unsaved changes first)");
    for (int n = 1; n <= SLOTS; n++) {
        std::string l = slot_label(n), item = "Slot " + std::to_string(n) + (l.empty() ? "  (empty)" : "  (" + l + ")") + "##l" + std::to_string(n);
        if (ImGui::MenuItem(item.c_str(), nullptr, false, !l.empty())) {
            file_dialog_answer(2, slot_path(n).c_str());
            input_live_command(0x8021);                         /* File > Load City */
        }
    }
    ImGui::EndMenu();
}

/* File > Save City, every so often, for a city that has a file already. An
 * unsaved "New City" would open the Save As dialog instead, so it is skipped. */
static void autosave_tick() {
    static DWORD last = GetTickCount();
    if (!S.autosave_min || GetTickCount() - last < (DWORD)S.autosave_min * 60000) return;
    last = GetTickCount();
    char title[256] = "";
    HWND frame = game_frame_window();
    if (!frame || !GetWindowTextA(frame, title, sizeof title) || strstr(title, "<New City>") || !strchr(title, '<'))
        return;
    input_live_command(0x8025);
    fprintf(stderr, "[frontend] autosave\n");
}

static void host_menus() {
    bool changed = false;
    if (ImGui::BeginMenu("Graphics")) {
        const char *filters[] = { "Nearest (original pixels)", "Smooth", "Sharp bilinear", "Scale2x (smooth edges)" };
        for (int i = 0; i < 4; i++) if (ImGui::MenuItem(filters[i], nullptr, S.filter == i)) { S.filter = i; changed = true; }
        ImGui::Separator();
        changed |= ImGui::MenuItem("Integer scaling", nullptr, &S.integer);
        ImGui::Separator();
        changed |= ImGui::MenuItem("Glow", nullptr, &S.glow);
        if (S.glow) {
            changed |= ImGui::SliderFloat("Glow strength", &S.glow_k, 0.0f, 2.0f);
            changed |= ImGui::SliderFloat("Glow threshold", &S.glow_threshold, 0.2f, 0.95f);
        }
        changed |= ImGui::MenuItem("CRT", nullptr, &S.crt);
        if (S.crt) {
            changed |= ImGui::SliderFloat("Curvature", &S.curve, 0.0f, 0.2f);
            changed |= ImGui::SliderFloat("Scanlines", &S.scan, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Shadow mask", &S.mask, 0.0f, 0.5f);
            changed |= ImGui::SliderFloat("Vignette", &S.vignette, 0.0f, 1.0f);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Effects")) {
        bool dn = g_fx.daynight, se = g_fx.seasons, we = g_fx.weather;
        if (ImGui::MenuItem("Day and night", nullptr, &dn)) { g_fx.daynight = dn; changed = true; }
        if (ImGui::MenuItem("Seasons", nullptr, &se)) { g_fx.seasons = se; changed = true; }
        if (ImGui::MenuItem("Weather", nullptr, &we)) { g_fx.weather = we; changed = true; }
        changed |= ImGui::SliderInt("Days per cycle", &g_fx.cycle_days, 5, 300);
        changed |= ImGui::SliderFloat("Night depth", &g_fx.night_depth, 0.0f, 0.95f);
        ImGui::EndMenu();
    }
    saves_menu();
    if (ImGui::BeginMenu("Play")) {
        ImGui::TextDisabled("Turbo (on top of the game's speed)");
        const int speeds[] = { 1, 2, 4, 8 };
        for (int k : speeds) {
            char lab[16];
            snprintf(lab, sizeof lab, k == 1 ? "Normal" : "%dx", k);
            if (ImGui::MenuItem(lab, nullptr, S.turbo == k)) { S.turbo = k; qol_set_turbo(k); changed = true; }
        }
        ImGui::Separator();
        changed |= ImGui::MenuItem("Mouse wheel zooms", nullptr, &S.wheel_zoom);
        ImGui::TextDisabled("Autosave (named cities only)");
        const int every[] = { 0, 5, 10, 30 };
        for (int m : every) {
            char lab[24];
            snprintf(lab, sizeof lab, m ? "Every %d minutes" : "Off", m);
            if (ImGui::MenuItem(lab, nullptr, S.autosave_min == m)) { S.autosave_min = m; changed = true; }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Display")) {
        if (ImGui::MenuItem("Fullscreen", "F11", S.fullscreen)) set_fullscreen(!S.fullscreen);
        changed |= ImGui::MenuItem("Vsync", nullptr, &S.vsync);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Debug")) {
        ImGui::MenuItem("City stats window", nullptr, &S.stats);
        ImGui::Separator();
        ImGui::Text("Frame capture: %.1f ms", frame_capture_ms());
        ImGui::Text("Presentation: %.0f fps", ImGui::GetIO().Framerate);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Mods")) {
        if (!mods_count()) ImGui::TextDisabled("No mods loaded");
        for (int i = 0; i < mods_count(); i++) {
            bool on = mods_on(i);
            if (ImGui::MenuItem(mods_name(i), nullptr, &on)) {
                mods_set(i, on);
                WritePrivateProfileStringA("mods", mods_name(i), on ? "1" : "0", g_ini);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mods_description(i));
        }
        ImGui::Separator();
        ImGui::TextDisabled("Add mods: DLLs in the mods folder beside sc2k.exe (docs/mods.md)");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Cheats")) {
        if (ImGui::MenuItem("+ $10,000")) set_funds(GAME_FUNDS + 10000);
        if (ImGui::MenuItem("+ $100,000")) set_funds(GAME_FUNDS + 100000);
        if (ImGui::MenuItem("+ $1,000,000")) set_funds(GAME_FUNDS + 1000000);
        ImGui::Separator();
        if (ImGui::MenuItem("Hold funds where they are", nullptr, &S.hold_funds)) S.held_funds = GAME_FUNDS;
        ImGui::EndMenu();
    }
    if (changed) save_settings();
}

static void stats_window() {
    if (!S.stats) return;
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("City stats", &S.stats)) {
        int d = GAME_DAYS;
        fx_state_t st;
        fx_state(&st);
        ImGui::Text("Date     %s %d, day %d", kMonth[(d / 25) % 12], d / 300 + GAME_START, d % 25 + 1);
        ImGui::Text("Weather  %s", GAME_WEATHER < 12 ? kWeather[GAME_WEATHER] : "?");
        ImGui::Text("Funds    $%d", GAME_FUNDS);
        ImGui::Text("Sky      %s (%.0f%% night)", st.phase < 0.25f || st.phase > 0.75f ? "night side" : "day side", st.night * 100);
    }
    ImGui::End();
}

/* ---------------------------------------------------------------- window */

static WINDOWPLACEMENT g_wp = { sizeof g_wp };

static void set_fullscreen(bool on) {
    S.fullscreen = on;
    LONG style = GetWindowLongA(g_hwnd, GWL_STYLE);
    if (on) {
        GetWindowPlacement(g_hwnd, &g_wp);
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_wp);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

/* Window coordinates to the picture's; false outside it. */
static bool to_game(int x, int y, int *gx, int *gy) {
    int dw = g_dest.right - g_dest.left, dh = g_dest.bottom - g_dest.top;
    if (dw <= 0 || dh <= 0 || x < g_dest.left || y < g_dest.top || x >= g_dest.right || y >= g_dest.bottom) return false;
    *gx = (x - g_dest.left) * g_fw / dw;
    *gy = (y - g_dest.top) * g_fh / dh;
    return true;
}

static bool g_ui_scripted;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    /* A UI script posts its own positions; the real cursor is elsewhere, and
     * the leave notices it causes would cancel them. */
    if (g_ui_scripted && (msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE)) return 0;
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
    ImGuiIO *io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
    switch (msg) {
    case WM_SIZE:
        if (g_dev && w != SIZE_MINIMIZED) {
            if (g_backbuffer) { g_backbuffer->Release(); g_backbuffer = nullptr; }
            g_swap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
            make_backbuffer();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && g_game_cursor && !(io && io->WantCaptureMouse)) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            int gx, gy;
            if (to_game(p.x, p.y, &gx, &gy)) { SetCursor(g_game_cursor); return TRUE; }
        }
        break;
    case WM_MOUSEWHEEL:
        if (S.wheel_zoom && !(io && io->WantCaptureMouse)) {
            /* The game's own zoom commands (the toolbar's magnifiers). */
            static int acc;
            acc += GET_WHEEL_DELTA_WPARAM(w);
            while (acc >= WHEEL_DELTA) { input_live_command(0x20); acc -= WHEEL_DELTA; }
            while (acc <= -WHEEL_DELTA) { input_live_command(0x21); acc += WHEEL_DELTA; }
            return 0;
        }
        /* fall through */
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_LBUTTONDBLCLK: {
        if (io && io->WantCaptureMouse && msg != WM_LBUTTONUP && msg != WM_RBUTTONUP) break;
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        if (msg == WM_MOUSEWHEEL) ScreenToClient(h, &p);
        int gx, gy;
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) SetCapture(h);
        if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) ReleaseCapture();
        if (!to_game(p.x, p.y, &gx, &gy)) {
            if (msg != WM_LBUTTONUP && msg != WM_RBUTTONUP) break;
            gx = std::max(0, std::min(g_fw - 1, (int)((p.x - g_dest.left) * (float)g_fw / std::max(1L, g_dest.right - g_dest.left))));
            gy = std::max(0, std::min(g_fh - 1, (int)((p.y - g_dest.top) * (float)g_fh / std::max(1L, g_dest.bottom - g_dest.top))));
        }
        input_live_mouse(msg == WM_LBUTTONDBLCLK ? WM_LBUTTONDOWN : msg, w, gx, gy);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_F11) { set_fullscreen(!S.fullscreen); return 0; }
        /* fall through */
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR:
        if (io && io->WantCaptureKeyboard) break;
        input_live_key(msg, w, l);
        return 0;
    case WM_CLOSE:
        save_settings();
        ExitProcess(0);
    }
    return DefWindowProcA(h, msg, w, l);
}

static DWORD WINAPI ui_script_thread(LPVOID arg);

static DWORD WINAPI frontend_thread(LPVOID) {
    WNDCLASSEXA wc = { sizeof wc };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconA((HINSTANCE)0x00400000, MAKEINTRESOURCEA(128));   /* the game's own icon */
    wc.lpszClassName = "sc2k-frontend";
    RegisterClassExA(&wc);
    RECT r = { 0, 0, 1600, 960 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA(wc.lpszClassName, "SimCity 2000 (recompiled)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!init_d3d(g_hwnd)) {
        MessageBoxA(nullptr, "Direct3D 11 could not be started.", "SimCity 2000", MB_ICONERROR);
        ExitProcess(3);
    }
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    static char script[1024];
    if ((g_ui_scripted = GetEnvironmentVariableA("SC2K_UISCRIPT", script, sizeof script) != 0))
        CreateThread(nullptr, 0, ui_script_thread, script, 0, nullptr);
    DWORD last_log = GetTickCount();

    for (;;) {
        if (GetTickCount() - last_log > 5000) {
            last_log = GetTickCount();
            fprintf(stderr, "[frontend] %.0f fps, frame capture %.1f ms\n", ImGui::GetIO().Framerate, frame_capture_ms());
        }
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        if (S.hold_funds && GAME_FUNDS != S.held_funds) set_funds(S.held_funds);
        autosave_tick();
        upload_frame();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        float bar = 0;
        if (ImGui::BeginMainMenuBar()) {
            game_menus();
            ImGui::Separator();
            host_menus();
            char right[96];
            snprintf(right, sizeof right, "$%d  %s %d", GAME_FUNDS, kMonth[(GAME_DAYS / 25) % 12], GAME_DAYS / 300 + GAME_START);
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - 16);
            ImGui::TextDisabled("%s", right);
            bar = ImGui::GetWindowSize().y;
            ImGui::EndMainMenuBar();
        }
        popup_menu();
        stats_window();
        ImGui::Render();

        RECT cr;
        GetClientRect(g_hwnd, &cr);
        const float black[4] = { 0, 0, 0, 1 };
        g_ctx->ClearRenderTargetView(g_backbuffer, black);
        render_picture(cr.right, cr.bottom, (int)bar);
        D3D11_VIEWPORT vp = { 0, 0, (float)cr.right, (float)cr.bottom, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->OMSetRenderTargets(1, &g_backbuffer, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(S.vsync ? 1 : 0, 0);
        if (!S.vsync) Sleep(1);
    }
    return 0;
}

/* SC2K_UISCRIPT="T:click X,Y; T:move X,Y; T:key VK": test input for the
 * frontend window itself (its menu bar, the picture), posted to it the way
 * --input posts to the game. For checking the bar without a real mouse. */
static DWORD WINAPI ui_script_thread(LPVOID arg) {
    const char *s = (const char *)arg;
    DWORD start = GetTickCount();
    while (*s) {
        double t;
        char verb[16];
        int x = 0, y = 0, used = 0;
        while (*s == ' ' || *s == ';') s++;
        if (sscanf(s, "%lf:%15[a-z] %d,%d%n", &t, verb, &x, &y, &used) < 3) break;
        s += used;
        while (*s && *s != ';') s++;
        DWORD due = start + (DWORD)(t * 1000), now = GetTickCount();
        if ((int)(due - now) > 0) Sleep(due - now);
        LPARAM lp = MAKELPARAM(x, y);
        if (!strcmp(verb, "move")) {
            PostMessageA(g_hwnd, WM_MOUSEMOVE, 0, lp);
        } else if (!strcmp(verb, "click")) {
            PostMessageA(g_hwnd, WM_MOUSEMOVE, 0, lp);
            Sleep(80);
            PostMessageA(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            Sleep(80);
            PostMessageA(g_hwnd, WM_LBUTTONUP, 0, lp);
        } else if (!strcmp(verb, "key")) {
            PostMessageA(g_hwnd, WM_KEYDOWN, x, 1);
            PostMessageA(g_hwnd, WM_KEYUP, x, 0xC0000001);
        }
        fprintf(stderr, "[ui] %.1fs %s %d,%d\n", t, verb, x, y);
    }
    return 0;
}

/* ---------------------------------------------------------------- C API */

extern "C" void frontend_start(const char *ini_path) {
    snprintf(g_ini, sizeof g_ini, "%s", ini_path);
    load_settings();
    qol_set_turbo(S.turbo);
    for (int i = 0; i < mods_count(); i++)
        if (GetPrivateProfileIntA("mods", mods_name(i), 0, g_ini)) mods_set(i, 1);
    g_popup.done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    frame_start();
    input_live_start();
    CreateThread(nullptr, 0, frontend_thread, nullptr, 0, nullptr);
}

static bool g_frontend_on;
extern "C" void frontend_enable(void) { g_frontend_on = true; }

/* The game's pop-up menus open here instead: a real one would be on the
 * headless desktop, where nobody can click it. Blocks the game thread until
 * the player picks (the guest lock is released around every native call). */
static BOOL WINAPI o_TrackPopupMenu(HMENU m, UINT flags, int x, int y, int res, HWND owner, const RECT *rc) {
    if (!g_frontend_on) return TrackPopupMenu(m, flags, x, y, res, owner, rc);
    g_popup.menu = m;
    g_popup.result = 0;
    ResetEvent(g_popup.done);
    g_popup.state = 1;
    WaitForSingleObject(g_popup.done, INFINITE);
    g_popup.state = 0;
    UINT id = g_popup.result;
    if (flags & TPM_RETURNCMD) return (BOOL)id;
    if (id) PostMessageA(owner, WM_COMMAND, id, 0);
    return id != 0;
}

/* Remember the game's cursor so the frontend can show it over the picture. */
static HCURSOR WINAPI o_SetCursor(HCURSOR c) {
    g_game_cursor = c;
    return SetCursor(c);
}

extern "C" void *frontend_override(const char *name) {
    if (!strcmp(name, "TrackPopupMenu")) return (void *)o_TrackPopupMenu;
    if (!strcmp(name, "SetCursor")) return (void *)o_SetCursor;
    return nullptr;
}
