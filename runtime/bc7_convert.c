// bc7_convert.c — el productor GPU de la caché BC7 (fase 2, pieza A, doc 04 §6):
// dos modos del ejecutable que tools/bc7_precompute.py maneja tras enumerar la RomFS.
//
//   --bc7-keys      sin Vulkan: por cada trabajo del fichero de trabajos, calcula su
//                   clave (hash y total) con el layout de ESTE runtime y la imprime.
//                   Es la mitad offline de la validación de claves (doc 04 §5): las
//                   líneas se cruzan contra el log [bc7key] del juego en caliente.
//   --bc7-convert   con Vulkan: además de las claves, codifica cada textura a BC7 con
//                   el codificador GPU del port (vk_astc_record con bc=3, modo 6) y
//                   vuelca los bloques, en orden de trabajos, a data.bin.partial en la
//                   carpeta AC_BC7_OUT. El driver monta después index.bin y el sello.
//                   Los bloques son los mismos que el juego produciría en vivo: esta
//                   variante ahorra la codificación en las subidas, no mejora calidad
//                   (eso es el productor CPU con bc7enc, doc 04 §3).
//
// El fichero de trabajos (lo escribe bc7_precompute.py; formato propio y simple):
//   cabecera: "BC7J", u32 versión=1, u32 cuenta, u32 records_off
//   los bytes huésped de cada textura, concatenados tras la cabecera (el driver los
//   escribe al enumerar, sin guardarlos en RAM), y AL FINAL los registros:
//   por trabajo (40 bytes): u32 format, w, h, levels, layers, packaged;
//                           u64 data_off, u64 data_size (offset dentro del fichero)
// Las rutas llegan por la configuración: AC_BC7_JOBS (trabajos) y AC_BC7_OUT (salida).
// Adaptado del diseño de AstralChain-PC (Puddz, MIT) usando el layout, el deswizzle y
// el codificador de este runtime: la clave solo casa si sale del mismo código que el
// juego usa en caliente.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "bc7_cache.h"
#include "cfg.h"
#include "host.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"

typedef struct {
    uint32_t format, w, h, levels, layers, packaged;
    uint64_t data_off, data_size;
} Bc7Job;
_Static_assert(sizeof(Bc7Job) == 40, "Bc7Job debe medir 40 bytes");

// El nvn_texdesc que el juego construiría desde el XT1 (mapeo de doc 04 §2):
// packaged -> pkg_block_h directo es la hipótesis de partida; la validación de
// claves (§5) es la que la confirma o la corrige. 0 si no es ASTC convertible.
static int desc_of(nvn_texdesc *d, const Bc7Job *j) {
    memset(d, 0, sizeof *d);
    d->target = j->layers == 6 ? NVN_TEXTURE_TARGET_CUBEMAP : NVN_TEXTURE_TARGET_2D;
    d->format = (int)j->format;
    d->width = j->w, d->height = j->h, d->depth = 1;
    d->levels = j->levels;
    d->samples = 1;
    d->pkg_block_h = (signed int)(j->packaged & 0xFF);
    int bw, bh;
    return nvn_fmt_bpb(j->format, &bw, &bh) != 0;
}

// La clave de un trabajo: layout con el código del runtime y hash sobre los bytes
// huésped. 0 si el blob es más corto que el almacenamiento que el juego le dará.
static int key_of(const Bc7Job *j, const uint8_t *data, uint64_t *hash, uint64_t *total) {
    nvn_texdesc d;
    nvn_texlayout L;
    if (!desc_of(&d, j)) return 0;
    nvn_tex_layout(&d, &L);
    *total = L.size;
    if (*total > j->data_size) return 0;
    *hash = ac_bc7_hash(data, *total);
    return 1;
}

// Lee la cabecera y los registros (al final del fichero) de trabajos. NULL si no
// es válido.
static Bc7Job *jobs_load(FILE *f, uint32_t *count) {
    uint32_t head[4];
    if (fread(head, 4, 4, f) != 4 || head[0] != 0x4A374342u /* "BC7J" */ || head[1] != 1 || !head[2])
        return NULL;
#ifdef _WIN32
    if (_fseeki64(f, (long long)head[3], SEEK_SET) != 0) return NULL;
#else
    if (fseeko(f, (off_t)head[3], SEEK_SET) != 0) return NULL;
#endif
    Bc7Job *jobs = malloc((size_t)head[2] * sizeof *jobs);
    if (!jobs || fread(jobs, sizeof *jobs, head[2], f) != head[2]) { free(jobs); return NULL; }
    *count = head[2];
    return jobs;
}

// Los bytes huésped de un trabajo, leídos de su offset. malloc del llamador.
static uint8_t *job_data(FILE *f, const Bc7Job *j) {
    uint8_t *data = malloc(j->data_size ? j->data_size : 1);
    if (!data) return NULL;
#ifdef _WIN32
    if (_fseeki64(f, (long long)j->data_off, SEEK_SET) != 0) { free(data); return NULL; }
#else
    if (fseeko(f, (off_t)j->data_off, SEEK_SET) != 0) { free(data); return NULL; }
#endif
    if (j->data_size && fread(data, 1, j->data_size, f) != j->data_size) { free(data); return NULL; }
    return data;
}

static void print_key(const Bc7Job *j, uint64_t hash, uint64_t total) {
    printf("key %016llx %llu %#x %u %u %u %u\n", (unsigned long long)hash,
           (unsigned long long)total, j->format, j->w, j->h, j->levels, j->layers);
    fflush(stdout);
}

int bc7_keys(void) {
    const char *path = cfg_get("AC_BC7_JOBS");
    if (!path || !*path) { fprintf(stderr, "bc7-keys: falta AC_BC7_JOBS\n"); return 2; }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "bc7-keys: no se abre %s\n", path); return 2; }
    uint32_t count = 0;
    Bc7Job *jobs = jobs_load(f, &count);
    if (!jobs) { fprintf(stderr, "bc7-keys: fichero de trabajos no válido\n"); fclose(f); return 2; }
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *data = job_data(f, &jobs[i]);
        uint64_t hash = 0, total = 0;
        if (data && key_of(&jobs[i], data, &hash, &total)) print_key(&jobs[i], hash, total);
        else { printf("skip %u\n", i); fflush(stdout); skipped++; }
        free(data);
    }
    printf("bc7-keys: %u claves, %u saltados\n", count - skipped, skipped);
    free(jobs);
    fclose(f);
    return 0;
}

// ---- La conversión GPU (solo --bc7-convert) ----

typedef struct { VkImage img; VmaAllocation alloc; } Bc7Img;

// Como el make_image de astc_probe.c, con las capas del trabajo (cubemaps: 6).
static int bc7_make_image(VkCtx *c, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers,
                          VkFormat format, Bc7Img *o) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = format, .extent = {w, h, 1},
                            .mipLevels = levels, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateImage(c->vma, &ci, &ai, &o->img, &o->alloc, NULL) == VK_SUCCESS;
}

static void bc7_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa,
                        VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define BC7_ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static void bc7_to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static uint32_t bc7_mip(uint32_t v, uint32_t lv) { uint32_t m = v >> lv; return m ? m : 1; }

// Convierte un trabajo y deja sus bloques (orden capa→nivel, el de bc7_cache.h) en
// *out (malloc del llamador). 1 bien, 0 fallo, VK_ASTC_NO_SCRATCH si el codificador
// se quedó sin scratch en la GPU (el llamador reintenta con el scratch en el host).
static int bc7_convert_one(VkCtx *c, const Bc7Job *j, const uint8_t *data, int host, uint8_t **out) {
    nvn_texdesc d;
    if (!desc_of(&d, j)) return 0;
    int bw, bh;
    int bpb = nvn_fmt_bpb(j->format, &bw, &bh);
    int srgb = j->format >= 0x87;
    uint32_t nreg = j->levels * j->layers;
    VkAstcRegion *regions = calloc(nreg, sizeof *regions);
    if (!regions) return 0;
    // El staging: cada nivel/capa deswizzleado, en orden capa→nivel, offsets a 16.
    uint64_t raw_total = 0;
    for (uint32_t ly = 0; ly < j->layers; ly++)
        for (uint32_t lv = 0; lv < j->levels; lv++) {
            uint32_t bx = (bc7_mip(j->w, lv) + (uint32_t)bw - 1) / (uint32_t)bw;
            uint32_t by = (bc7_mip(j->h, lv) + (uint32_t)bh - 1) / (uint32_t)bh;
            raw_total += (uint64_t)bx * by * (uint32_t)bpb;
        }
    Bc7Img im;
    VkFormat fmt = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    if (!bc7_make_image(c, j->w, j->h, j->levels, j->layers, fmt, &im)) { free(regions); return 0; }
    uint64_t blocks_size = ac_bc7_size(j->w, j->h, j->levels, j->layers);
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, rb;
    VmaAllocation ra;
    uint8_t *stg = vk_staging(raw_total, &sb, NULL);
    uint8_t *rmap = vk_staging(blocks_size, &rb, &ra);
    uint64_t off = 0;
    uint32_t k = 0;
    int bad = 0;
    for (uint32_t ly = 0; ly < j->layers && !bad; ly++)
        for (uint32_t lv = 0; lv < j->levels; lv++) {
            uint32_t mw = bc7_mip(j->w, lv), mh = bc7_mip(j->h, lv);
            uint32_t rb_bytes = (mw + (uint32_t)bw - 1) / (uint32_t)bw * (uint32_t)bpb;
            if (nvn_deswizzle(&d, lv, ly, data, stg + off, rb_bytes)) bad = 1;
            regions[k++] = (VkAstcRegion){.src = off, .w = mw, .h = mh, .slices = 1, .level = lv, .layer = ly};
            off += (uint64_t)((mw + (uint32_t)bw - 1) / (uint32_t)bw) * ((mh + (uint32_t)bh - 1) / (uint32_t)bh) * (uint32_t)bpb;
        }
    if (bad) {   // sin layout CPU para algún nivel: se abandona el trabajo entero
        vk_upload_end();
        vk_upload_flush(1);
        vmaDestroyBuffer(c->vma, rb, ra);
        vk_defer_image(im.img, im.alloc);
        vk_upload_flush(1);
        free(regions);
        return 0;
    }
    bc7_to_general(cb, im.img);
    bc7_barrier(cb, BC7_ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int rec = (host ? vk_astc_record_host : vk_astc_record)(cb, sb, raw_total, regions, (int)nreg,
                                                            (uint32_t)bw, (uint32_t)bh, srgb, 3, im.img,
                                                            VK_IMAGE_ASPECT_COLOR_BIT, VK_NULL_HANDLE);
    if (rec == VK_ASTC_NO_SCRATCH) {
        vk_upload_end();
        vk_upload_flush(1);
        vmaDestroyBuffer(c->vma, rb, ra);
        vk_defer_image(im.img, im.alloc);
        vk_upload_flush(1);
        free(regions);
        return VK_ASTC_NO_SCRATCH;
    }
    if (rec <= 0) {
        vk_upload_end();
        vk_upload_flush(1);
        vmaDestroyBuffer(c->vma, rb, ra);
        vk_defer_image(im.img, im.alloc);
        vk_upload_flush(1);
        free(regions);
        return 0;
    }
    bc7_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, BC7_ALL_RW);
    // Lectura de vuelta: una copia por nivel/capa, bloques apretados en el mismo orden.
    VkBufferImageCopy *copies = calloc(nreg, sizeof *copies);
    if (!copies) {
        vk_upload_end();
        vk_upload_flush(1);
        vmaDestroyBuffer(c->vma, rb, ra);
        vk_defer_image(im.img, im.alloc);
        vk_upload_flush(1);
        free(regions);
        return 0;
    }
    uint64_t boff = 0;
    k = 0;
    for (uint32_t ly = 0; ly < j->layers; ly++)
        for (uint32_t lv = 0; lv < j->levels; lv++) {
            uint32_t mw = bc7_mip(j->w, lv), mh = bc7_mip(j->h, lv);
            copies[k++] = (VkBufferImageCopy){.bufferOffset = boff,
                                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                              .imageExtent = {mw, mh, 1}};
            boff += (uint64_t)((mw + 3) / 4) * ((mh + 3) / 4) * 16;
        }
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_GENERAL, rb, nreg, copies);
    free(copies);
    bc7_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    vk_upload_flush(1);
    vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
    uint8_t *blocks = malloc(blocks_size);
    if (blocks) memcpy(blocks, rmap, blocks_size);
    vmaDestroyBuffer(c->vma, rb, ra);
    vk_defer_image(im.img, im.alloc);
    vk_upload_flush(1);
    free(regions);
    if (!blocks) return 0;
    *out = blocks;
    return 1;
}

int bc7_convert(void) {
    const char *path = cfg_get("AC_BC7_JOBS");
    const char *out_dir = cfg_get("AC_BC7_OUT");
    if (!path || !*path || !out_dir || !*out_dir) {
        fprintf(stderr, "bc7-convert: faltan AC_BC7_JOBS o AC_BC7_OUT\n");
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "bc7-convert: no se abre %s\n", path); return 2; }
    uint32_t count = 0;
    Bc7Job *jobs = jobs_load(f, &count);
    if (!jobs) { fprintf(stderr, "bc7-convert: fichero de trabajos no válido\n"); fclose(f); return 2; }
    char data_path[1024];
    snprintf(data_path, sizeof data_path, "%s/data.bin.partial", out_dir);
    FILE *out = fopen(data_path, "wb");
    if (!out) { fprintf(stderr, "bc7-convert: no se crea %s\n", data_path); free(jobs); fclose(f); return 2; }
    // El modo BC7 se decide una vez por dispositivo: se fija antes de vk_init para
    // que la decisión construya el codificador BC7 (como hace la sonda ASTC).
    plat_setenv("AC_ASTC_BC", "bc7");
    if (!vk_init() || vk_astc_bc() != 3) {
        fprintf(stderr, "bc7-convert: el dispositivo no muestrea BC7 o Vulkan no arranca\n");
        fclose(out); free(jobs); fclose(f);
        return 1;
    }
    VkCtx *c = vk_ctx();
    uint32_t converted = 0, skipped = 0, failed = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *data = job_data(f, &jobs[i]);
        uint64_t hash = 0, total = 0;
        if (!data || !key_of(&jobs[i], data, &hash, &total)) {
            printf("skip %u\n", i); fflush(stdout);
            skipped++;
            free(data);
            continue;
        }
        print_key(&jobs[i], hash, total);
        uint8_t *blocks = NULL;
        int r = bc7_convert_one(c, &jobs[i], data, 0, &blocks);
        if (r == VK_ASTC_NO_SCRATCH)   // sin scratch en la GPU: otra vez, en el host
            r = bc7_convert_one(c, &jobs[i], data, 1, &blocks);
        free(data);
        if (r != 1) { printf("fail %u\n", i); fflush(stdout); failed++; continue; }
        uint64_t size = ac_bc7_size(jobs[i].w, jobs[i].h, jobs[i].levels, jobs[i].layers);
        if (fwrite(blocks, 1, size, out) != size) {
            fprintf(stderr, "bc7-convert: error escribiendo data.bin.partial\n");
            free(blocks);
            fclose(out); free(jobs); fclose(f);
            return 1;
        }
        free(blocks);
        converted++;
        if (converted % 200 == 0) { printf("bc7-convert: %u convertidas\n", converted); fflush(stdout); }
    }
    fclose(out);
    printf("bc7-convert: %u convertidas, %u saltadas, %u fallidas\n", converted, skipped, failed);
    free(jobs);
    fclose(f);
    return failed ? 1 : 0;
}
