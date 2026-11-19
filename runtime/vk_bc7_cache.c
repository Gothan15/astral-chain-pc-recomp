// vk_bc7_cache.c — consulta de solo lectura de la caché BC7 precómputada.
// Adaptado del cache_open/vk_bc7_cache_upload de AstralChain-PC (Puddz, MIT,
// runtime/las/nvn/vulkan/bc7_gpu.c), a las rutas y al batch de subida de este port.
//
// La caché vive junto al exe (cache/bc7/index.bin + data.bin), como shader-cache/ y
// pipeline-cache.bin. Todo fallo es un miss: sin caché, con la caché corrupta o con
// una textura que no está, el juego codifica en vivo como si esto no existiera.
// Nunca se escribe nada desde aquí: escribir es cosa del productor (doc 04).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc7_cache.h"
#include "cfg.h"
#include "host.h"
#include "vk.h"

// data.bin pasa de 16 GB: el fseek de 32 bits se queda corto a mitad de la caché.
// En Windows se usa _fseeki64 (CRT) y fuera, fseeko con off_t de 64 bits.
#ifdef _WIN32
#define ac_fseek64 _fseeki64
#else
#define ac_fseek64 fseeko
#endif

static AcBc7Record *g_index;      // el índice entero, cargado en RAM una sola vez
static uint32_t g_count;          // 0 = sin caché (o caché inválida): todo es miss
static uint64_t g_hits, g_misses; // contadores de la sesión

// La ruta de un fichero de la caché: la carpeta del exe + cache/bc7/<name>, con el
// mismo patrón que cache_path_init de vk_pipeline.c (plat_exe_path + plat_last_sep).
static int cache_path(char *buf, size_t cap, const char *name) {
    char exe[1024];
    int n = plat_exe_path(exe, sizeof exe);
    if (!n || n >= (int)sizeof exe) return 0;
    char *cut = plat_last_sep(exe);
    if (!cut) return 0;
    *cut = 0;
    return snprintf(buf, cap, "%s/cache/bc7/%s", exe, name) < (int)cap;
}

// ¿Está puesto el log de claves ([bc7key], la herramienta de validación de 04)?
// Se lee una vez: es una clave de diagnóstico sin fila en metas, como las DUMP_*.
static int key_log(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("AC_BC7_CACHE_LOG");
        on = v && *v && strcmp(v, "0") != 0;
    }
    return on;
}

// Apertura perezosa y defensiva del índice: cualquier problema (no hay ficheros, la
// cabecera no casa, el índice no cabe en RAM) deja g_count = 0. data.bin NO se deja
// abierto: cada acierto lo abre, lee su registro y lo cierra, así dos subidas que
// coincidan no se pisan el offset de un FILE* compartido (el coste es despreciable
// frente a una subida de textura).
static void cache_open(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    char path[1024];
    if (!cache_path(path, sizeof path, "index.bin")) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t head[3];
    int ok = fread(head, 4, 3, f) == 3 && head[0] == AC_BC7_CACHE_MAGIC &&
             head[1] == AC_BC7_CACHE_VERSION && head[2] != 0;
    if (ok) {
        g_index = malloc((size_t)head[2] * sizeof *g_index);
        ok = g_index && fread(g_index, sizeof *g_index, head[2], f) == head[2];
        if (ok) g_count = head[2];
    }
    fclose(f);
    if (!g_count) { free(g_index), g_index = NULL; return; }
    // Sin data.bin el índice no sirve de nada: se comprueba que al menos abre.
    if (!cache_path(path, sizeof path, "data.bin")) { free(g_index), g_index = NULL, g_count = 0; return; }
    FILE *d = fopen(path, "rb");
    if (!d) { free(g_index), g_index = NULL, g_count = 0; return; }
    fclose(d);
    rt_log("vk: BC7 cache: %u precomputed textures", g_count);
}

int vk_bc7_cache_log_on(void) { return key_log(); }

// La línea [bc7key] de una subida BC7, acierte o no la caché (y aunque no haya
// caché): es la mitad en caliente de la validación de claves de doc 04 §5, que se
// cruza contra las claves del enumerador. La emite el enganche de vk_texture.c.
void vk_bc7_cache_log_key(const void *guest, uint64_t total, uint32_t format, uint32_t w,
                          uint32_t h, uint32_t layout_levels, uint32_t layers) {
    rt_log("[bc7key] %016llx total %llu fmt %#x %ux%u levels %u layers %u",
           (unsigned long long)ac_bc7_hash(guest, total), (unsigned long long)total,
           format, w, h, layout_levels, layers);
}

int vk_bc7_cache_enabled(void) {
    static int on = -1;
    if (on < 0)
        on = cfg_bool("AC_BC7_CACHE") && vk_astc_bc() == 3;   // solo sirve al modo bc7
    return on;
}

int vk_bc7_cache_fetch(const void *guest, uint64_t total, uint32_t format, uint32_t w,
                       uint32_t h, uint32_t layout_levels, uint32_t layers,
                       uint8_t **blocks, uint64_t *size) {
    cache_open();
    if (!g_count) return 0;
    uint64_t hash = ac_bc7_hash(guest, total);
    int log = key_log();
    // Búsqueda binaria hasta el PRIMER registro con este hash (vienen ordenados),
    // y barrido lineal entre los de igual hash hasta que la geometría case entera.
    const AcBc7Record *r = NULL;
    for (uint32_t lo = 0, hi = g_count; lo < hi;) {
        uint32_t mid = (lo + hi) / 2;
        if (g_index[mid].hash < hash) lo = mid + 1;
        else { if (g_index[mid].hash == hash) r = &g_index[mid]; hi = mid; }
    }
    while (r && r < g_index + g_count && r->hash == hash &&
           (r->total != total || r->format != format || r->width != w || r->height != h ||
            r->levels != layout_levels || r->layers != layers))
        r++;
    if (!r || r >= g_index + g_count || r->hash != hash) {
        g_misses++;
        if (log)
            rt_log("[bc7key] %016llx total %llu fmt %#x %ux%u levels %u layers %u: miss",
                   (unsigned long long)hash, (unsigned long long)total, format, w, h,
                   layout_levels, layers);
        return 0;
    }
    // El tamaño del registro debe ser el que la geometría anuncia: si el data.bin
    // está truncado o el índice es de otro productor, esto (o la lectura) lo caza.
    if (r->size != ac_bc7_size(w, h, layout_levels, layers)) { g_misses++; return 0; }
    char path[1024];
    uint8_t *buf = malloc(r->size);
    FILE *d = NULL;
    int ok = buf && cache_path(path, sizeof path, "data.bin") && (d = fopen(path, "rb")) &&
             ac_fseek64(d, (long long)r->offset, SEEK_SET) == 0 &&
             fread(buf, 1, r->size, d) == r->size;
    if (d) fclose(d);
    if (!ok) { free(buf); g_misses++; return 0; }
    if (log)
        rt_log("[bc7key] %016llx total %llu fmt %#x %ux%u levels %u layers %u: hit",
               (unsigned long long)hash, (unsigned long long)total, format, w, h,
               layout_levels, layers);
    *blocks = buf, *size = r->size;
    g_hits++;
    return 1;
}

void vk_bc7_cache_record(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                         const uint8_t *blocks, uint32_t w, uint32_t h,
                         uint32_t layout_levels, uint32_t layers, uint32_t levels) {
    uint64_t off = 0;
    for (uint32_t ly = 0; ly < layers; ly++)
        for (uint32_t lv = 0; lv < layout_levels; lv++) {
            uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
            uint32_t cx = (lw + 3) / 4, cy = (lh + 3) / 4;
            uint64_t bytes = (uint64_t)cx * cy * 16;
            if (lv < levels) {
                // Un staging por nivel dentro del mismo batch: vk_staging devuelve un
                // buffer mapeado que vive hasta que el batch termina (vk.h).
                VkBuffer sb;
                uint8_t *stg = vk_staging(bytes, &sb, NULL);
                memcpy(stg, blocks + off, bytes);
                VkBufferImageCopy rg = {.bufferOffset = 0, .bufferRowLength = cx * 4,
                                        .bufferImageHeight = cy * 4,
                                        .imageSubresource = {aspect, lv, ly, 1},
                                        .imageExtent = {lw, lh, 1}};
                vkCmdCopyBufferToImage(cb, sb, image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            }
            off += bytes;   // también por los niveles no subidos: el orden es capa→nivel
        }
}
