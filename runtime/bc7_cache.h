// Caché en disco de texturas BC7 precómputadas (pieza A de la fase 2): el productor
// (tools/bc7_precompute.py + su herramienta C, o el modo --bc7-convert del exe) la escribe
// tras la instalación; el runtime la consulta de solo lectura en vk_texture.c antes de
// codificar en vivo. Formato e identificación adaptados de AstralChain-PC (Puddz, MIT):
// su bc7_cache.h, con la misma función de hash y el mismo registro de 56 bytes.
//
// index.bin: cabecera "BC7C", versión, cuenta; después `cuenta` registros ordenados por
// hash. data.bin: los bloques de cada textura, capa por capa y nivel por nivel dentro de
// cada capa, 16 bytes por bloque de 4x4 téxeles.
// La identidad de una textura es el hash de sus bytes huésped TAL CUAL están en la
// memoria de textura (layout Tegra, sin deswizzle), con la longitud que el layout
// calcula (nvn_texlayout.size); el registro guarda además ese total y la geometría,
// y la consulta los verifica todos antes de servir un solo bloque.
#pragma once
#include <stdint.h>
#include <string.h>

#define AC_BC7_CACHE_MAGIC 0x43374342u   // "BC7C"
#define AC_BC7_CACHE_VERSION 1u

typedef struct {
    uint64_t hash;      // ac_bc7_hash(bytes huésped, total)
    uint64_t total;     // bytes huésped de la textura (nvn_texlayout.size)
    uint64_t offset;    // offset de sus bloques dentro de data.bin
    uint64_t size;      // bytes BC7 de la textura (suma de niveles x capas, 16 B/bloque)
    uint32_t format;    // formato NVN ASTC (0x79..0x94)
    uint32_t width;     // ancho del nivel 0
    uint32_t height;    // alto del nivel 0
    uint32_t levels;    // niveles DEL LAYOUT (los que el hash cubre)
    uint32_t layers;    // capas (1, o 6 en cubemaps)
    uint32_t pad;       // siempre 0
} AcBc7Record;
_Static_assert(sizeof(AcBc7Record) == 56, "AcBc7Record debe medir 56 bytes");

// El hash de la caché: cuatro carriles FNV-1a de 64 bits sobre los bytes huésped.
// Es el bc7_hash_full de Puddz (MIT) con otro nombre: DEBE ser bit a bit el mismo
// cálculo en el productor y en el runtime, o ningún registro acertará jamás.
// Lee u64 por memcpy (endian del host, x86-64): la caché es local a cada PC.
static inline uint64_t ac_bc7_hash(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full,
             h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^
                 (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

// Bytes BC7 de una textura: por capa, la suma de bloques de cada nivel; 16 B/bloque.
// La usan el productor (para `size` y para escribir data.bin en orden) y la consulta
// (para avanzar offsets por los niveles que no se suben).
static inline uint64_t ac_bc7_size(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers) {
    uint64_t n = 0;
    for (uint32_t lv = 0; lv < levels; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        n += (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * 16;
    }
    return n * layers;
}
