// Iconos de teclado y ratón (vk_prompts.h). Adaptado de button_prompts.c de Puddz
// (https://github.com/Puddz/AstralChain-PC, MIT): la tabla de huecos, el lienzo de cobertura, el
// ratón y el texto recortado con GDI son de su código; aquí las asignaciones salen del snapshot de
// input_map.c y la identificación se hace al subir la textura en vk_texture.c.
#include "vk_prompts.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"      // rt_log
#include "input_map.h"

// input.c: la copia de la tabla de binds bajo su cerrojo (la misma que usa ui/binds_page.cpp) y el
// estilo de prompts en efecto (AC_PROMPTS resuelto: auto sin mando = teclado; parche en 04).
void input_map_snapshot(InputMap *m);
int input_prompts_effective(void);   // PROMPTS_* de input_map.h

#define ATLAS_HASH 0x4afe0dd389f44dc9ull   // el hash de contenido del atlas original, de Puddz
#define ATLAS_W 256
#define ATLAS_H 512
#define ATLAS_FORMAT 0x25                  // RGBA8 en la numeración de formatos NVN de Puddz; si en
                                           // Sparrow el desc trae otro número, la identificación por
                                           // tamaño + tamaño de almacenamiento + hash sigue mandando
                                           // y el log dice el formato real de la candidata.
#define SS 4                               // supersampling del lienzo de cobertura

enum Kind { K_KEY, K_LSTICK, K_RSTICK, K_DPAD };
enum { D_UP = 1, D_DOWN = 2, D_LEFT = 4, D_RIGHT = 8, D_HORZ = 12, D_VERT = 3, D_ALL = 15, D_ROTATE = 16 };
typedef struct { int x0, y0, x1, y1; int kind, arg; } Slot;  // caja del icono original (inclusive);
// para K_KEY, arg es la acción B_* de Sparrow (en Puddz era su IB_* equivalente)
static const Slot slots[] = {
    {9, 8, 48, 32, K_RSTICK, D_HORZ},     {69, 7, 108, 32, K_LSTICK, D_HORZ},   {129, 7, 163, 32, K_LSTICK, D_UP},
    {185, 7, 217, 32, K_LSTICK, D_LEFT},  {4, 52, 52, 76, K_RSTICK, D_ROTATE},  {69, 51, 103, 76, K_RSTICK, D_VERT},
    {125, 51, 159, 76, K_LSTICK, D_DOWN}, {182, 51, 214, 76, K_LSTICK, D_RIGHT}, {9, 95, 43, 120, K_LSTICK, D_VERT},
    {69, 97, 90, 118, K_KEY, B_MINUS},    {125, 97, 146, 118, K_KEY, B_B},      {181, 97, 202, 118, K_KEY, B_A},
    {224, 93, 243, 120, K_KEY, B_STICK_L}, {224, 137, 243, 164, K_KEY, B_STICK_R},
    {9, 141, 30, 162, K_KEY, B_Y},        {69, 141, 90, 162, K_KEY, B_X},       {124, 142, 147, 161, K_KEY, B_R},
    {180, 140, 203, 163, K_KEY, B_ZL},    {8, 184, 31, 207, K_KEY, B_ZR},       {69, 185, 90, 206, K_KEY, B_PLUS},
    {124, 184, 147, 208, K_LSTICK, D_ALL}, {8, 228, 31, 252, K_RSTICK, D_ALL},   {68, 230, 91, 249, K_KEY, B_L},
    {125, 229, 146, 250, K_DPAD, D_ALL},  {181, 229, 202, 250, K_DPAD, D_UP},
    {9, 273, 30, 294, K_DPAD, D_DOWN},    {69, 273, 90, 294, K_DPAD, D_LEFT},   {125, 273, 146, 294, K_DPAD, D_RIGHT},
    {181, 273, 202, 294, K_DPAD, D_VERT}, {9, 317, 30, 338, K_DPAD, D_HORZ},    {69, 317, 90, 338, K_DPAD, D_UP},
    {125, 317, 146, 338, K_DPAD, D_DOWN}, {181, 317, 202, 338, K_DPAD, D_LEFT}, {9, 361, 30, 382, K_DPAD, D_RIGHT},
    // los diagramas de cuatro círculos de los botones de acción: arriba X, abajo B, izquierda Y, derecha A
    {63, 354, 96, 388, K_KEY, B_X},       {119, 355, 152, 389, K_KEY, B_B},     {174, 355, 208, 388, K_KEY, B_Y},
    {3, 399, 37, 432, K_KEY, B_A},
};

/* ------------------------------------------------------- las asignaciones, desde input_map.c */

static int key_is_mouse(int key) {   // lo que se dibuja como ratón: botones y rueda
    return (key >= KEY_MOUSE_L && key <= KEY_MOUSE_X2) || key == KEY_WHEEL_UP || key == KEY_WHEEL_DOWN;
}

// La tecla que se muestra para una acción: si tiene algún botón de ratón o rueda asignado, ese (por
// eso L y R muestran los laterales X1/X2 y no Q y E, como en el lanzador de Puddz); si no, la primera
// tecla de su lista. 0 (KEY_NONE) si la acción no tiene teclas.
static unsigned shown(const InputMap *m, int action) {
    uint8_t keys[INPUT_BINDING_MAX];
    int n = input_map_keys_of(m, action, keys, INPUT_BINDING_MAX);
    for (int i = 0; i < n; i++)
        if (key_is_mouse(keys[i])) return keys[i];
    return n ? keys[0] : KEY_NONE;
}

// La etiqueta corta de una tecla, a partir de su nombre de ini (input_key_name): las letras y los
// dígitos se muestran tal cual; el resto sale de esta tabla o de su forma (f2 -> F2, num3 -> N3).
static const wchar_t *key_label(int key, wchar_t buf[8]) {
    static const struct { const char *name; const wchar_t *label; } special[] = {
        {"space", L"Space"}, {"tab", L"Tab"}, {"lshift", L"Shift"}, {"rshift", L"Shift"},
        {"lctrl", L"Ctrl"}, {"rctrl", L"Ctrl"}, {"lalt", L"Alt"}, {"enter", L"Enter"},
        {"backspace", L"Bksp"}, {"capslock", L"Caps"}, {"insert", L"Ins"}, {"delete", L"Del"},
        {"home", L"Home"}, {"end", L"End"}, {"pageup", L"PgUp"}, {"pagedown", L"PgDn"}, {"esc", L"Esc"},
        {"minus", L"-"}, {"equals", L"="}, {"lbracket", L"["}, {"rbracket", L"]"}, {"semicolon", L";"},
        {"apostrophe", L"'"}, {"comma", L","}, {"period", L"."}, {"slash", L"/"}, {"backslash", L"\\"},
        {"grave", L"`"}, {"num_add", L"+"}, {"num_sub", L"-"}, {"num_mul", L"*"}, {"num_div", L"/"},
        {"num_decimal", L"."},
    };
    const char *name = input_key_name(key);
    if (!name) { swprintf(buf, 8, L"%d", key); return buf; }
    for (unsigned i = 0; i < sizeof special / sizeof *special; i++)
        if (!strcmp(name, special[i].name)) return special[i].label;
    if (name[0] && !name[1]) { buf[0] = (wchar_t)(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 'a' + 'A' : name[0]); buf[1] = 0; return buf; }
    if (name[0] == 'f' && name[1] >= '0' && name[1] <= '9') {   // f2..f12 -> F2..F12
        buf[0] = L'F'; int i = 1; for (; name[i] && i < 7; i++) buf[i] = (wchar_t)name[i]; buf[i] = 0; return buf;
    }
    if (name[0] == 'n' && name[1] == 'u' && name[2] == 'm' && name[3] >= '0' && name[3] <= '9' && !name[4]) {
        buf[0] = L'N'; buf[1] = (wchar_t)name[3]; buf[2] = 0; return buf;   // num3 -> N3
    }
    int i = 0; for (; name[i] && i < 7; i++) buf[i] = (wchar_t)name[i]; buf[i] = 0; return buf;
}

/* ------------------------------------------------------------------ lienzo de cobertura */
// Un hueco se dibuja a SS veces su tamaño en un lienzo de cobertura (0 vacío, 255 la forma blanca)
// y luego se filtra por caja al alfa del atlas. Las etiquetas se recortan de la forma, como en los
// iconos originales. (De Puddz, sin cambios de fondo.)
typedef struct { int w, h; uint8_t *c; } Canvas;

static void fill_rrect(Canvas *cv, float x0, float y0, float x1, float y1, float r, uint8_t v) {
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); y++) {
        if (y < 0 || y >= cv->h) continue;
        for (int x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
            if (x < 0 || x >= cv->w) continue;
            float px = x + 0.5f, py = y + 0.5f;
            float dx = px < x0 + r ? x0 + r - px : px > x1 - r ? px - (x1 - r) : 0;
            float dy = py < y0 + r ? y0 + r - py : py > y1 - r ? py - (y1 - r) : 0;
            if (dx * dx + dy * dy <= r * r) cv->c[y * cv->w + x] = v;
        }
    }
}

static void fill_ellipse(Canvas *cv, float cx, float cy, float rx, float ry, uint8_t v) {
    for (int y = (int)(cy - ry); y <= (int)(cy + ry); y++)
        for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++) {
            if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy <= 1) cv->c[y * cv->w + x] = v;
        }
}

// Triángulo que apunta en la dirección d (D_UP...), centrado en (cx, cy), semitamaño s.
static void fill_arrow(Canvas *cv, float cx, float cy, float s, int d, uint8_t v) {
    for (int y = (int)(cy - s) - 1; y <= (int)(cy + s) + 1; y++)
        for (int x = (int)(cx - s) - 1; x <= (int)(cx + s) + 1; x++) {
            if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
            float u = (x + 0.5f - cx) / s, w = (y + 0.5f - cy) / s;  // (girado para que la flecha apunte arriba)
            float a, b;
            if (d == D_UP) { a = u; b = w; } else if (d == D_DOWN) { a = u; b = -w; }
            else if (d == D_LEFT) { a = w; b = u; } else { a = w; b = -u; }
            if (b >= -0.8f && b <= 0.6f && fabsf(a) <= (b + 0.8f) * 0.75f) cv->c[y * cv->w + x] = v;
        }
}

/* ---------------------------------------------------------------------- texto (GDI, Windows) */
// El texto se rasteriza con GDI cargado en tiempo de ejecución, como hace Puddz (la línea de enlace
// del juego no lleva gdi32). Fuera de Windows no hay recorte de texto: las teclas se dibujan como
// formas sin letra (degradación conocida; el port se juega en Windows).
#ifdef _WIN32
#include <windows.h>

static struct {
    HDC (WINAPI *CreateCompatibleDC)(HDC);
    HBITMAP (WINAPI *CreateDIBSection)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
    HGDIOBJ (WINAPI *SelectObject)(HDC, HGDIOBJ);
    HFONT (WINAPI *CreateFontW)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR);
    COLORREF (WINAPI *SetTextColor)(HDC, COLORREF);
    int (WINAPI *SetBkMode)(HDC, int);
    BOOL (WINAPI *GetTextExtentPoint32W)(HDC, LPCWSTR, int, LPSIZE);
    BOOL (WINAPI *TextOutW)(HDC, int, int, LPCWSTR, int);
    BOOL (WINAPI *DeleteObject)(HGDIOBJ);
    BOOL (WINAPI *DeleteDC)(HDC);
    BOOL (WINAPI *GdiFlush)(void);
} gdi;

static bool gdi_load(void) {
    static int state;
    if (state) return state > 0;
    state = -1;
    HMODULE m = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
#define GDI_FN(name) if (!(*(FARPROC *)&gdi.name = GetProcAddress(m, #name))) return false
    GDI_FN(CreateCompatibleDC); GDI_FN(CreateDIBSection); GDI_FN(SelectObject); GDI_FN(CreateFontW); GDI_FN(SetTextColor);
    GDI_FN(SetBkMode);
    GDI_FN(GetTextExtentPoint32W); GDI_FN(TextOutW); GDI_FN(DeleteObject); GDI_FN(DeleteDC); GDI_FN(GdiFlush);
#undef GDI_FN
    state = 1;
    return true;
}

// Recorta el texto de la forma (cobertura 0 donde van los glifos), centrado en la caja, tan grande
// como quepa. (De Puddz.)
static void cut_text(Canvas *cv, const wchar_t *text, float bx0, float by0, float bx1, float by1) {
    if (!gdi_load()) return;
    int n = (int)wcslen(text), bw = (int)(bx1 - bx0), bh = (int)(by1 - by0);
    if (n == 0 || bw < 4 || bh < 4) return;
    HDC dc = gdi.CreateCompatibleDC(NULL);
    if (!dc) return;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = cv->w; bi.bmiHeader.biHeight = -cv->h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    uint32_t *px = NULL;
    HBITMAP bmp = gdi.CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp || !px) { gdi.DeleteDC(dc); return; }
    memset(px, 0, (size_t)cv->w * cv->h * 4);
    HGDIOBJ old_bmp = gdi.SelectObject(dc, bmp);
    // la mayor altura de fuente cuyo texto cabe en la caja
    int height = bh, fitted = 0;
    SIZE size = {0};
    HFONT font = NULL;
    for (; height >= 6; height = height * 9 / 10) {
        font = gdi.CreateFontW(-height, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) break;
        HGDIOBJ old = gdi.SelectObject(dc, font);
        gdi.GetTextExtentPoint32W(dc, text, n, &size);
        gdi.SelectObject(dc, old);
        if (size.cx <= bw && size.cy <= bh * 1.45f) { fitted = 1; break; }
        gdi.DeleteObject(font);
        font = NULL;
    }
    if (fitted) {
        HGDIOBJ old = gdi.SelectObject(dc, font);
        gdi.SetBkMode(dc, TRANSPARENT);
        gdi.SetTextColor(dc, RGB(255, 255, 255));
        // (la celda de Segoe UI deja sitio sobre las mayúsculas: se centra en la celda, un poco arriba)
        int tx = (int)(bx0 + (bw - size.cx) / 2.0f), ty = (int)(by0 + (bh - size.cy) / 2.0f + size.cy * 0.02f);
        gdi.TextOutW(dc, tx, ty, text, n);
        gdi.GdiFlush();
        gdi.SelectObject(dc, old);
        for (int i = 0; i < cv->w * cv->h; i++) if (px[i] & 0xFFFFFF) cv->c[i] = 0;
    }
    if (font) gdi.DeleteObject(font);
    gdi.SelectObject(dc, old_bmp);
    gdi.DeleteObject(bmp);
    gdi.DeleteDC(dc);
}
#else
static void cut_text(Canvas *cv, const wchar_t *text, float bx0, float by0, float bx1, float by1) {
    (void)cv; (void)text; (void)bx0; (void)by0; (void)bx1; (void)by1;   // sin GDI: formas sin letra
}
#endif

/* ----------------------------------------------------------------------------------- dibujo */
// Un ratón: cuerpo redondeado, botones separados arriba; `which` se rellena macizo y el resto del
// cuerpo queda en contorno. dirs añade flechas de movimiento a los lados (prompts de cámara): a
// izquierda y derecha, arriba y abajo apiladas a su derecha, o las cuatro en cruz a su derecha.
// (De Puddz; `which` es aquí el KEY_* de Sparrow.)
static void draw_mouse(Canvas *cv, float x0, float y0, float x1, float y1, unsigned which, int dirs) {
    float w = x1 - x0, h = y1 - y0;
    // ancho del ratón y sus flechas, en alturas de ratón
    float aw = dirs == D_HORZ ? 0.66f + 2 * 0.42f : dirs == D_VERT ? 0.66f + 0.5f : dirs ? 0.66f + 0.95f : 0.66f;
    float mh = fminf(h * 0.96f, w / aw), mw = mh * 0.66f, arrow = mh * 0.17f;
    float left = x0 + (w - mh * aw) / 2 + (dirs == D_HORZ ? mh * 0.42f : 0);  // borde izquierdo del ratón
    float cx = left + mw / 2, cy = y0 + h / 2;
    float mx0 = cx - mw / 2, my0 = cy - mh / 2, mx1 = cx + mw / 2, my1 = cy + mh / 2;
    float line = fmaxf(mw * 0.13f, SS * 1.3f), split = my0 + mh * 0.42f;
    fill_rrect(cv, mx0, my0, mx1, my1, mw * 0.48f, 255);
    fill_rrect(cv, mx0 + line, my0 + line, mx1 - line, my1 - line, mw * 0.48f - line, 0);  // (contorno)
    fill_rrect(cv, mx0, split - line / 2, mx1, split + line / 2, 0, 255);                    // línea de botones
    fill_rrect(cv, cx - line / 2, my0, cx + line / 2, split, 0, 255);                        // entre botones
    if (which == KEY_MOUSE_L || which == KEY_MOUSE_R) {
        float hx0 = which == KEY_MOUSE_L ? mx0 : cx, hx1 = which == KEY_MOUSE_L ? cx : mx1;
        for (int y = (int)my0; y < (int)split; y++)
            for (int x = (int)hx0; x < (int)hx1; x++) {
                if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) continue;
                float dx = (x + 0.5f - cx) / (mw / 2), dy = (y + 0.5f - (my0 + mw * 0.48f)) / (mw * 0.48f);
                if (y + 0.5f > my0 + mw * 0.48f || dx * dx + dy * dy <= 1.0f) cv->c[y * cv->w + x] = 255;
            }
    } else if (which == KEY_MOUSE_M || which == KEY_WHEEL_UP || which == KEY_WHEEL_DOWN) {
        fill_rrect(cv, cx - mw * 0.16f, my0 + mh * 0.10f, cx + mw * 0.16f, split - line, mw * 0.16f, 255);  // rueda
        if (which != KEY_MOUSE_M) fill_arrow(cv, cx, which == KEY_WHEEL_UP ? my0 + mh * 0.66f : my0 + mh * 0.72f,
                                             mw * 0.24f, which == KEY_WHEEL_UP ? D_UP : D_DOWN, 255);
    } else if (which == KEY_MOUSE_X1 || which == KEY_MOUSE_X2) {  // laterales: una pestaña a la izquierda
        float ty = which == KEY_MOUSE_X1 ? my0 + mh * 0.64f : my0 + mh * 0.47f;
        fill_rrect(cv, mx0 - line * 1.2f, ty, mx0 + line * 2.6f, ty + mh * 0.15f, line * 0.6f, 255);
    }
    if (dirs == D_HORZ) {
        fill_arrow(cv, mx0 - mh * 0.22f, cy, arrow, D_LEFT, 255);
        fill_arrow(cv, mx1 + mh * 0.22f, cy, arrow, D_RIGHT, 255);
    } else if (dirs == D_VERT) {
        fill_arrow(cv, mx1 + mh * 0.27f, cy - mh * 0.2f, arrow, D_UP, 255);
        fill_arrow(cv, mx1 + mh * 0.27f, cy + mh * 0.2f, arrow, D_DOWN, 255);
    } else if (dirs) {
        float ax = mx1 + mh * 0.5f, d = mh * 0.27f, a = arrow * 0.9f;
        fill_arrow(cv, ax, cy - d, a, D_UP, 255);
        fill_arrow(cv, ax, cy + d, a, D_DOWN, 255);
        fill_arrow(cv, ax - d, cy, a, D_LEFT, 255);
        fill_arrow(cv, ax + d, cy, a, D_RIGHT, 255);
    }
}

// Una tecla que llena la caja: las flechas de cursor llevan flecha; las demás, su nombre.
static void draw_keycap(Canvas *cv, float x0, float y0, float x1, float y1, unsigned key) {
    if (!key) return;  // (sin asignar: se deja vacío)
    if (key_is_mouse(key)) { draw_mouse(cv, x0, y0, x1, y1, key, 0); return; }
    float w = x1 - x0, h = y1 - y0, r = fminf(w, h) * 0.22f;
    fill_rrect(cv, x0, y0, x1, y1, r, 255);
    int arrow = key == KEY_UP ? D_UP : key == KEY_DOWN ? D_DOWN : key == KEY_LEFT ? D_LEFT : key == KEY_RIGHT ? D_RIGHT : 0;
    if (arrow) { fill_arrow(cv, x0 + w / 2, y0 + h / 2, fminf(w, h) * 0.3f, arrow, 0); return; }
    wchar_t buf[8];
    float pad = fminf(w, h) * 0.14f;
    cut_text(cv, key_label(key, buf), x0 + pad, y0 + pad, x1 - pad, y1 - pad);
}

// Varias teclas lado a lado (dos direcciones) o un grupo de cuatro (todas). act: arriba, abajo,
// izquierda, derecha. (De Puddz.)
static void draw_directions(Canvas *cv, float x0, float y0, float x1, float y1, const int act[4], const InputMap *m,
                            int dirs) {
    float w = x1 - x0, h = y1 - y0;
    int single = dirs == D_UP ? 0 : dirs == D_DOWN ? 1 : dirs == D_LEFT ? 2 : dirs == D_RIGHT ? 3 : -1;
    if (single >= 0) {  // una tecla cuadrada en el centro
        float s = fminf(w, h), cx = x0 + w / 2, cy = y0 + h / 2;
        draw_keycap(cv, cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2, shown(m, act[single]));
        return;
    }
    if (dirs == D_HORZ || dirs == D_VERT) {  // dos teclas lado a lado, con un hueco pequeño
        int a = dirs == D_HORZ ? 2 : 0;
        float g = SS * 1.5f, s = fminf((w - g) / 2, h), cx = x0 + w / 2, cy = y0 + h / 2;
        draw_keycap(cv, cx - g / 2 - s, cy - s / 2, cx - g / 2, cy + s / 2, shown(m, act[a]));
        draw_keycap(cv, cx + g / 2, cy - s / 2, cx + g / 2 + s, cy + s / 2, shown(m, act[a + 1]));
        return;
    }
    // cuatro teclas: arriba sobre el centro, izquierda / abajo / derecha debajo (etiquetas solo si
    // son lo bastante grandes para leerse)
    float g = SS * 1.0f, s = fminf((w - 2 * g) / 3, (h - g) / 2), cx = x0 + w / 2, cy = y0 + h / 2;
    float top = cy - g / 2 - s, bottom = cy + g / 2;
    bool labels = s >= SS * 9;
    float xs[3] = {cx - s * 1.5f - g, cx - s / 2, cx + s / 2 + g};
    struct { int a; float x, y; } caps[4] = {{0, xs[1], top}, {2, xs[0], bottom}, {1, xs[1], bottom}, {3, xs[2], bottom}};
    for (int i = 0; i < 4; i++) {
        if (labels) draw_keycap(cv, caps[i].x, caps[i].y, caps[i].x + s, caps[i].y + s, shown(m, act[caps[i].a]));
        else fill_rrect(cv, caps[i].x, caps[i].y, caps[i].x + s, caps[i].y + s, s * 0.22f, 255);
    }
}

static void draw_slot(Canvas *cv, const Slot *sl, const InputMap *m) {
    float x0 = 0, y0 = 0, x1 = (float)cv->w, y1 = (float)cv->h;
    static const int lstick[4] = {B_SL_UP, B_SL_DOWN, B_SL_LEFT, B_SL_RIGHT},
                     rstick[4] = {B_SR_UP, B_SR_DOWN, B_SR_LEFT, B_SR_RIGHT},
                     dpad[4] = {B_UP, B_DOWN, B_LEFT, B_RIGHT};
    switch (sl->kind) {
    case K_KEY: {
        unsigned key = shown(m, sl->arg);
        if (!key) return;
        float s = fminf(x1, y1), w = x1;
        // (un nombre más largo que una letra lleva una tecla más ancha donde el icono era más ancho)
        if (key_is_mouse(key) || (key >= KEY_A && key <= KEY_Z) || (key >= KEY_0 && key <= KEY_9)) w = s;
        float cx = x1 / 2, cy = y1 / 2;
        draw_keycap(cv, cx - w / 2, cy - s / 2, cx + w / 2, cy + s / 2, key);
        return;
    }
    case K_RSTICK:
        // En Sparrow la cámara con ratón está siempre disponible con el cursor capturado (no hay un
        // ajuste «mouse look» aparte como en Puddz): los huecos del stick derecho dibujan el ratón.
        draw_mouse(cv, x0, y0, x1, y1, 0, sl->arg == D_ALL ? 0 : sl->arg);
        return;
    case K_LSTICK: draw_directions(cv, x0, y0, x1, y1, lstick, m, sl->arg); return;
    case K_DPAD: draw_directions(cv, x0, y0, x1, y1, dpad, m, sl->arg); return;
    }
}

/* ------------------------------------------------------------------ identificación y pintado */
// FNV-1a de 64 bits sobre todos los bytes: la misma mezcla (xor y multiplicar por 0x100000001B3) que
// la caché de muestras de Puddz. Su bc7_hash_full no está en este repo: si el hash completo de la
// candidata no casa con ATLAS_HASH, el log lo imprime para corregir la constante con un arranque.
static uint64_t hash_full(const uint8_t *mem, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ mem[i]) * 0x100000001B3ull;
    return h;
}

int vk_prompts_identify(const void *mem, uint64_t size, uint32_t format, uint32_t w, uint32_t h) {
    if (!mem || w != ATLAS_W || h != ATLAS_H || size != (uint64_t)ATLAS_W * ATLAS_H * 4) return 0;
    if (format != ATLAS_FORMAT)
        rt_log("vk: prompts: candidata al atlas con formato %#x (se esperaba %#x); se decide por el hash", format,
               ATLAS_FORMAT);
    // (una textura del mismo tamaño que no es el atlas se pregunta en cada comprobación: solo se
    //  vuelve a hashear entera cuando una muestra de sus bytes cambió. El veredicto se cachea
    //  con la muestra: sin eso, la segunda subida del atlas bueno devolvía 0 y el repintado se
    //  perdía — al cambiar un bind el atlas se resube tal cual y volvían los iconos de mando,
    //  con la marca de «viejo» puesta y resubidas en cada submit.)
    static struct { const void *mem; uint64_t sample; int verdict; } seen[16];
    const uint8_t *p = mem;
    uint64_t sample = 0;
    for (uint64_t i = 0; i < size; i += size / 16) { uint64_t v; memcpy(&v, p + i, 8); sample = (sample ^ v) * 0x100000001B3ull; }
    unsigned k = (unsigned)(((uintptr_t)mem >> 4) % 16);
    if (seen[k].mem == mem && seen[k].sample == sample) return seen[k].verdict;
    uint64_t hash = hash_full(p, size);
    int verdict = hash == ATLAS_HASH;
    seen[k].mem = mem, seen[k].sample = sample, seen[k].verdict = verdict;
    if (!verdict) {
        static int n_logs;
        if (n_logs++ < 4)
            rt_log("vk: prompts: textura de 256x512 que no es el atlas (hash %llx, se esperaba %llx)",
                   (unsigned long long)hash, (unsigned long long)ATLAS_HASH);
        return 0;
    }
    rt_log("vk: prompts: atlas de iconos encontrado");
    return 1;
}

// La generación: el estilo en efecto y un hash de la tabla de binds vigente. Si cualquiera de los
// dos cambia, el atlas pintado queda viejo y vk_texture.c lo resube (vk_prompts_stale).
uint32_t vk_prompts_generation(void) {
    InputMap m;
    input_map_snapshot(&m);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < m.nkeys; i++) {
        h = (h ^ m.keys[i].key) * 0x100000001B3ull;
        h = (h ^ m.keys[i].action) * 0x100000001B3ull;
    }
    return (uint32_t)(h >> 32) ^ (uint32_t)h ^ (uint32_t)input_prompts_effective() * 2654435761u;
}

static uint64_t painted_addr;
static uint32_t painted_gen;

void vk_prompts_note_painted(uint64_t addr) {
    painted_addr = addr;
    painted_gen = vk_prompts_generation();
}

int vk_prompts_stale(uint64_t addr) {
    return painted_addr && addr == painted_addr && vk_prompts_generation() != painted_gen;
}

int vk_prompts_paint(uint8_t *dst, uint32_t row, const void *mem, uint64_t size, uint32_t format, uint32_t w,
                     uint32_t h) {
    if (!dst || input_prompts_effective() != PROMPTS_KEYBOARD) return 0;
    if (!vk_prompts_identify(mem, size, format, w, h)) return 0;
    InputMap m;
    input_map_snapshot(&m);
    for (unsigned i = 0; i < sizeof slots / sizeof slots[0]; i++) {
        const Slot *sl = &slots[i];
        int sw = sl->x1 - sl->x0 + 1, sh = sl->y1 - sl->y0 + 1;
        Canvas cv = {sw * SS, sh * SS, calloc((size_t)sw * sh, SS * SS)};
        if (!cv.c) return 0;
        draw_slot(&cv, sl, &m);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                unsigned sum = 0;
                for (int j = 0; j < SS; j++)
                    for (int i2 = 0; i2 < SS; i2++) sum += cv.c[(y * SS + j) * cv.w + x * SS + i2];
                uint8_t *px = dst + (size_t)(sl->y0 + y) * row + (size_t)(sl->x0 + x) * 4;
                px[0] = px[1] = px[2] = 255;
                px[3] = (uint8_t)(sum / (SS * SS));
            }
        free(cv.c);
    }
    return 1;
}
