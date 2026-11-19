// El transporte del contrato con el addon de ReShade (vk_reshade_contract.h): memoria compartida con
// nombre. Es la parte de Windows del módulo; como todo archivo *_win.c del runtime, es Win32 por diseño
// y no pasa por la capa de plataforma. Fuera de Windows no hay contrato: el addon es cosa de Windows, y
// sin contrato la opción queda inerte (vk_reshade_contract_addon_present devuelve siempre 0).
#include <stdint.h>

#include "vk_reshade_contract.h"

#ifdef _WIN32

#include <windows.h>
#include <string.h>

static HANDLE g_map;              // el bloque creado (o abierto) por este proceso
static AcReshadeContract *g_sh;   // su vista mapeada

// El bloque compartido, creado a la primera: si el addon ya lo creó, se abre el suyo; si no, se crea
// aquí con la marca del fork. Devuelve NULL si algo falla (la opción queda inerte, sin romper nada).
static AcReshadeContract *contract(void) {
    if (g_sh) return g_sh;
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(AcReshadeContract),
                               AC_RESHADE_CONTRACT_NAME);
    if (!g_map) return NULL;
    int fresh = GetLastError() != ERROR_ALREADY_EXISTS;
    g_sh = (AcReshadeContract *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AcReshadeContract));
    if (!g_sh) return NULL;
    if (fresh) {
        memset(g_sh, 0, sizeof *g_sh);
        g_sh->fork_magic = AC_RESHADE_FORK_MAGIC;
    }
    return g_sh;
}

void vk_reshade_contract_publish(uint64_t hud_image, uint32_t w, uint32_t h, uint32_t format, uint64_t frame) {
    AcReshadeContract *s = contract();
    if (!s) return;
    s->hud_image = hud_image;
    s->width = w;
    s->height = h;
    s->format = format;
    s->frame = frame;
}

// El addon está presente si escribió su marca y sigue dando señales: en cada present de ReShade anota el
// frame que ha visto (addon_frame), así que si el addon se descarga o muere, su marca caduca en unos
// frames y el fork deja de redirigir en vez de quedarse sin HUD para siempre.
int vk_reshade_contract_addon_present(void) {
    AcReshadeContract *s = contract();
    if (!s || s->addon_magic != AC_RESHADE_ADDON_MAGIC) return 0;
    return s->frame - s->addon_frame <= 3;
}

#else

void vk_reshade_contract_publish(uint64_t hud_image, uint32_t w, uint32_t h, uint32_t format, uint64_t frame) {
    (void)hud_image, (void)w, (void)h, (void)format, (void)frame;
}

int vk_reshade_contract_addon_present(void) { return 0; }

#endif
