// Cámara directa (1:1) del ratón: en vez de emular el stick derecho, el movimiento del ratón se
// suma al yaw y al pitch del estado de la cámara del juego, escritos en su propia memoria, como
// hace un juego de PC. Es el plan de docs/camara-directa-plan.md, y este módulo trae sus dos
// primeras fases:
//
// Fase 1, localizar el estado (AC_CAM_SCAN=1): vuelca a disco las regiones escribibles del montón
// del huésped cada 8 segundos (6 volcados), para comparar fuera del juego una postura quieta con
// una girada y encontrar los dos flotantes que siguen a la cámara.
//
// Fase 2, escribir: las direcciones del estado se leen del archivo cam-direct.txt de la carpeta
// del juego («yaw 0x...» y «pitch 0x...», direcciones del huésped), que es lo que la fase 1
// produce. Sin archivo, o con valores que no validan, el módulo queda inactivo e input.c sigue
// con la emulación del stick de siempre: el modo direct sin direcciones no rompe nada.
//
// La escritura va en el frame presentado (boot.c), el mismo punto donde corre la máquina de
// cheats: lo escrito lo lee la actualización de cámara del frame siguiente.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cam_direct.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

// Conteos de ratón pendientes de aplicar: los suma el hilo de entrada y los toma el frame.
static volatile int32_t pend_dx, pend_dy;

void cam_direct_push(int32_t dx, int32_t dy) {
    if (dx) __atomic_add_fetch(&pend_dx, dx, __ATOMIC_RELAXED);
    if (dy) __atomic_add_fetch(&pend_dy, dy, __ATOMIC_RELAXED);
}

void cam_direct_reset(void) {
    __atomic_store_n(&pend_dx, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pend_dy, 0, __ATOMIC_RELAXED);
}

static int32_t take(volatile int32_t *p) { return __atomic_exchange_n(p, 0, __ATOMIC_RELAXED); }

// ---- el estado de la cámara, cuando se conoce -------------------------------------------------

static uint64_t yaw_addr, pitch_addr;   // direcciones del huésped; 0 = sin localizar
static int addr_bad;                    // la validación falló: no reintentar hasta otro archivo

// ¿Se puede escribir un flotante en esta dirección del huésped? mem_query da la región que la
// contiene: tiene que existir, estar viva y permitir lectura y escritura.
static int addr_ok(uint64_t a) {
    RtMemoryInfo mi;
    if (mem_query(a, &mi) || !mi.state || (mi.perm & 3) != 3) return 0;
    return a + 4 <= mi.base + mi.size;
}

// Lee cam-direct.txt de la carpeta del juego: dos líneas, «yaw 0x...» y «pitch 0x...».
static void try_load_addrs(void) {
    FILE *f = fopen("cam-direct.txt", "r");
    if (!f) return;
    char line[128];
    uint64_t y = 0, p = 0;
    while (fgets(line, sizeof line, f)) {
        char name[16];
        unsigned long long v;
        if (sscanf(line, "%15s %llx", name, &v) == 2) {
            if (!strcmp(name, "yaw")) y = v;
            else if (!strcmp(name, "pitch")) p = v;
        }
    }
    fclose(f);
    if (y && p && addr_ok(y) && addr_ok(p)) {
        yaw_addr = y;
        pitch_addr = p;
        rt_log("cam-direct: estado de la cámara en yaw %#llx, pitch %#llx",
               (unsigned long long)y, (unsigned long long)p);
    } else if (y || p) {
        addr_bad = 1;
        rt_log("cam-direct: cam-direct.txt no trae dos direcciones válidas; el modo directo queda inactivo");
    }
}

int cam_direct_active(void) { return yaw_addr != 0; }

// La sensibilidad en conteos por radián: como en el Bloodborne de referencia, 900 conteos por
// radián con AC_MOUSE_SENS a 1; la clave escala esa base.
static double counts_per_rad(void) {
    static double sens = -1;
    static unsigned seen;
    unsigned ver = cfg_version();
    if (sens < 0 || ver != seen) {
        seen = ver;
        sens = cfg_double("AC_MOUSE_SENS");
        if (sens < 0) sens = 0;
        if (sens > 10) sens = 10;
    }
    return 900.0 * sens;
}

static void apply(int32_t dx, int32_t dy) {
    double scale = counts_per_rad();
    if (scale <= 0) return;   // sensibilidad 0: sin giro, como en el stick
    if (!addr_ok(yaw_addr) || !addr_ok(pitch_addr)) {
        rt_log("cam-direct: el estado de la cámara dejó de validar; el modo directo queda inactivo");
        yaw_addr = pitch_addr = 0;
        addr_bad = 1;
        return;
    }
    float *yaw = (float *)(uintptr_t)yaw_addr;
    float *pitch = (float *)(uintptr_t)pitch_addr;
    float y = *yaw, p = *pitch;
    if (!isfinite(y) || !isfinite(p) || fabsf(y) > 10000.0f || fabsf(p) > 10000.0f) {
        rt_log("cam-direct: valores imposibles en el estado (yaw %f, pitch %f); el modo directo queda inactivo", y, p);
        yaw_addr = pitch_addr = 0;
        addr_bad = 1;
        return;
    }
    double np = (double)p - (double)dy / scale;   // ratón hacia arriba (dy negativo) = mirar arriba
    if (np > 1.45) np = 1.45;                     // el pitch se limita a unos ±83 grados
    if (np < -1.45) np = -1.45;
    *yaw = (float)((double)y + (double)dx / scale);
    *pitch = (float)np;
}

// ---- fase 1: el escáner de volcados -----------------------------------------------------------

static int scan_on(void) {
    static int on = -1;
    static unsigned seen;
    unsigned ver = cfg_version();
    if (on < 0 || ver != seen) {
        seen = ver;
        on = cfg_bool("AC_CAM_SCAN");
        if (on) rt_log("cam-scan: escáner activo: un volcado del montón cada 8 s (6 en total) en la carpeta cam-scan");
    }
    return on;
}

#define SCAN_MAX_DUMPS 6
#define SCAN_CAP_BYTES (768ull << 20)   // tope por volcado: el montón usado no debería acercarse

static void do_dump(int n, uint64_t frame) {
    const RtGuestLayout *L = mem_layout();
    if (!L->heap.size) return;
    plat_file_mkdir_p("cam-scan");
    char bin_path[64], txt_path[64];
    snprintf(bin_path, sizeof bin_path, "cam-scan/dump-%d.bin", n);
    snprintf(txt_path, sizeof txt_path, "cam-scan/dump-%d.txt", n);
    FILE *bin = fopen(bin_path, "wb");
    FILE *txt = fopen(txt_path, "w");
    if (!bin || !txt) {
        if (bin) fclose(bin);
        if (txt) fclose(txt);
        rt_log("cam-scan: no pude abrir %s para escribir", bin_path);
        return;
    }
    fprintf(txt, "dump %d\nframe %llu\nheap %#llx %llu\n", n, (unsigned long long)frame,
            (unsigned long long)L->heap.base, (unsigned long long)L->heap.size);
    uint64_t end = L->heap.base + L->heap.size, a = L->heap.base, total = 0;
    unsigned chunks = 0;
    while (a < end && total < SCAN_CAP_BYTES) {
        RtMemoryInfo mi;
        if (mem_query(a, &mi) || !mi.size) { a += 0x1000; continue; }
        uint64_t re = mi.base + mi.size;
        if (mi.state && (mi.perm & 2)) {   // región viva y escribible: ahí vive el estado del juego
            uint64_t s = mi.base < a ? a : mi.base, e = re > end ? end : re;
            if (e > s && total + (e - s) <= SCAN_CAP_BYTES) {
                fprintf(txt, "chunk %#llx %llu %llu\n", (unsigned long long)s,
                        (unsigned long long)(e - s), (unsigned long long)total);
                fwrite((const void *)(uintptr_t)s, 1, (size_t)(e - s), bin);
                total += e - s;
                chunks++;
            }
        }
        a = re > a ? re : a + 0x1000;
    }
    fclose(bin);
    fclose(txt);
    rt_log("cam-scan: volcado %d listo: %llu MB en %u trozos (%s)", n,
           (unsigned long long)(total >> 20), chunks, bin_path);
}

// ---- el frame ----------------------------------------------------------------------------------

void cam_direct_frame(uint64_t frame) {
    uint64_t now = plat_now_ns();

    if (scan_on()) {
        static uint64_t next_dump;   // 0 = armar con el primer frame
        static int dumps;
        if (!next_dump) next_dump = now + 10000000000ull;   // el primero, a los 10 s de arrancar
        if (dumps < SCAN_MAX_DUMPS && now >= next_dump) {
            do_dump(dumps, frame);
            dumps++;
            next_dump = now + 8000000000ull;
            if (dumps == SCAN_MAX_DUMPS) rt_log("cam-scan: sesión completa (%d volcados)", dumps);
        }
    }

    const char *mode = cfg_value("AC_MOUSE_CAMERA");
    if (!mode || strcmp(mode, "direct")) {   // sin el modo directo no hay nada que aplicar
        take(&pend_dx);
        take(&pend_dy);
        return;
    }
    if (!yaw_addr && !addr_bad) {
        static uint64_t next_try;
        if (now >= next_try) {   // el archivo puede aparecer con el juego en marcha (tras el escaneo)
            try_load_addrs();
            next_try = now + 5000000000ull;
        }
    }
    int32_t dx = take(&pend_dx), dy = take(&pend_dy);
    if (yaw_addr && (dx || dy)) apply(dx, dy);
}
