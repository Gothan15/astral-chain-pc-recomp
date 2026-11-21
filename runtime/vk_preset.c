// El preset de imagen ELZELDAPA y el CMAA 2 en el present (vk.h): dos efectos tomados del preset de ReShade
// que Gothan usa en el port de Zelda, integrados en el juego para no necesitar ReShade.
//
// El preset (AC_POST_PRESET=elzeldapa, en directo) corre sobre la imagen del juego en el present, después del
// ajuste de color (vk_color.c) y antes del FSR o el blit, en el orden del preset original:
//   1. Colourfulness -0.30 (shaders/preset_colour.comp): porte de Colourfulness.fx de bacondither
//      (licencia BSD: redistribuible con su aviso, que va en el propio shader).
//   2. ExposureFusion (shaders/preset_ef_*.comp): porte de MartysMods_EXPOSUREFUSION.fx de Pascal Gilcher —
//      PROPIETARIO («unauthorized copying strictly prohibited»): está aquí solo para el uso privado de
//      Gothan y este porte no debe salir de su fork privado. Es un mapeo tonal local Retinex: siembra una
//      pirámide de (sesgo, log-luma) a mitad de resolución, la baja con un Kuwahara separable que respeta
//      los bordes, funde los niveles de abajo arriba y aplica el sesgo como factor 2^sesgo sobre el color
//      expandido a HDR por la curva del propio shader.
//   El afilado del preset (Smart Sharp) no va aquí: es la alternativa al RCAS en vk_fsr.c (AC_SHARPENER).
//   El antialias del preset (CMAA 2) tampoco: es la opción AC_ANTI_ALIASING=cmaa2 de aquí abajo.
//
// El CMAA 2 (shaders/cmaa_*.comp): porte a compute del CMAA_2.fx (© Intel, Apache 2.0; porte a ReShade de
// Lord Of Lunacy) — detección de bordes en modo MLAA con el umbral HIGH, formas simples, formas en Z con
// trazado de línea de hasta 128 píxeles y mezcla diferida. Sin la lista de candidatos del original: cada
// píxel candidato hace el mismo trabajo en su hilo, y la mezcla larga se recoge (gather) en vez de
// dibujarse con líneas. Corre cuando ningún upscaler temporal está activo, que es cuando la opción de
// antialiasing manda (como el SMAA del juego), y sobre la imagen tal como llega al present, HUD incluido —
// el mismo sitio donde ReShade lo aplicaba.
//
// Como vk_color.c: cada función devuelve una imagen RGBA16F del tamaño del recorte que toma el lugar de la
// imagen del juego, o VK_NULL_HANDLE si el efecto está apagado o falló algo (y el present sigue sin él).
// Las vistas de origen de la ranura s se destruyen cuando la ranura s graba otra vez.
#include "vk.h"

#include <string.h>

#include "cfg.h"
#include "cmaa_blend_spv.h"
#include "cmaa_edges_spv.h"
#include "cmaa_process_spv.h"
#include "host.h"
#include "preset_colour_spv.h"
#include "preset_ef_down_spv.h"
#include "preset_ef_fuse_spv.h"
#include "preset_ef_init_spv.h"
#include "preset_ef_main_spv.h"
#include "vk_dlaa.h"
#include "vk_rec.h"

#define SLOTS 8
#define EF_LEVELS 8   // los niveles 0..7 de la pirámide (el original llega a 9 solo por encima de 4K)

// Una tubería de cómputo con su propio layout (las pasadas de este módulo atan entre 2 y 5 descriptores)
typedef struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
} Pipe;

static struct {
    int tried, presetOk, cmaaOk;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler sampNear, sampLin;
    Pipe colour, efInit, efDown, efFuse, efMain;   // el preset
    Pipe cmaaEdges, cmaaProcess, cmaaBlend;        // el CMAA 2
    // el preset: trabajo (salida de Colourfulness y color de entrada de ExposureFusion), salida, pirámide
    VkImage work, out;
    VmaAllocation workAlloc, outAlloc;
    VkImageView workView, outView;
    VkImage pyr[EF_LEVELS], tmp[EF_LEVELS], fused;
    VmaAllocation pyrAlloc[EF_LEVELS], tmpAlloc[EF_LEVELS], fusedAlloc;
    VkImageView pyrView[EF_LEVELS], tmpView[EF_LEVELS], fusedView;
    uint32_t pw, ph;   // el tamaño del que cuelgan las imágenes del preset
    // el CMAA 2: bordes (R8), candidatos (RGBA16F), formas Z (RGBA8), salida
    VkImage edges, cand, zsh, aout;
    VmaAllocation edgesAlloc, candAlloc, zshAlloc, aoutAlloc;
    VkImageView edgesView, candView, zshView, aoutView;
    uint32_t aw, ah;
    VkImageView src_view[SLOTS];    // la vista de origen de cada ranura en vk_preset_record
    VkImageView cmaa_view[SLOTS];   // y en vk_cmaa_record
    unsigned seen;                  // la versión de cfg con la que se leyeron los valores de abajo
    int presetOn, cmaaOn;
    int loggedPreset, loggedCmaa;   // la línea de «en marcha» ya salió para este tamaño
} P = {.seen = ~0u};

static int make_pipe(VkCtx *c, Pipe *p, const uint32_t *spv, size_t bytes, const VkDescriptorSetLayoutBinding *b,
                     int nb, uint32_t pushSize) {
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = (uint32_t)nb, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &p->dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &p->dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &p->layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = p->layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p->pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    return r == VK_SUCCESS;
}

static int init(VkCtx *c) {
    P.tried = 1;
    P.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!P.push) return 0;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (vkCreateSampler(c->device, &si, NULL, &P.sampNear) != VK_SUCCESS) return 0;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR, si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(c->device, &si, NULL, &P.sampLin) != VK_SUCCESS) return 0;
    const VkDescriptorSetLayoutBinding inOut[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    const VkDescriptorSetLayoutBinding fuseB[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = EF_LEVELS,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    const VkDescriptorSetLayoutBinding mainB[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    const VkDescriptorSetLayoutBinding procB[4] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    const VkDescriptorSetLayoutBinding blendB[5] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    P.presetOk = make_pipe(c, &P.colour, preset_colour_spv, sizeof preset_colour_spv, inOut, 2, 32) &&
                 make_pipe(c, &P.efInit, preset_ef_init_spv, sizeof preset_ef_init_spv, inOut, 2, 32) &&
                 make_pipe(c, &P.efDown, preset_ef_down_spv, sizeof preset_ef_down_spv, inOut, 2, 24) &&
                 make_pipe(c, &P.efFuse, preset_ef_fuse_spv, sizeof preset_ef_fuse_spv, fuseB, 3, 32) &&
                 make_pipe(c, &P.efMain, preset_ef_main_spv, sizeof preset_ef_main_spv, mainB, 3, 32);
    if (!P.presetOk) rt_log("vk: image preset off: its pipelines could not be created");
    P.cmaaOk = make_pipe(c, &P.cmaaEdges, cmaa_edges_spv, sizeof cmaa_edges_spv, inOut, 2, 32) &&
               make_pipe(c, &P.cmaaProcess, cmaa_process_spv, sizeof cmaa_process_spv, procB, 4, 32) &&
               make_pipe(c, &P.cmaaBlend, cmaa_blend_spv, sizeof cmaa_blend_spv, blendB, 5, 32);
    if (!P.cmaaOk) rt_log("vk: CMAA 2 off: its pipelines could not be created");
    return P.presetOk || P.cmaaOk;
}

// Una imagen de trabajo con su vista, en GENERAL para todo (muestreo y almacenamiento)
static int make_image(VkCtx *c, VkFormat fmt, uint32_t w, uint32_t h, VkImage *img, VmaAllocation *alloc,
                      VkImageView *view) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, alloc, NULL) != VK_SUCCESS) return *img = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, view) != VK_SUCCESS) return *view = VK_NULL_HANDLE, 0;
    return 1;
}

static void drop_image(VkCtx *c, VkImage *img, VmaAllocation *alloc, VkImageView *view) {
    if (*view) vkDestroyImageView(c->device, *view, NULL);
    if (*img) vmaDestroyImage(c->vma, *img, *alloc);
    *img = VK_NULL_HANDLE, *view = VK_NULL_HANDLE, *alloc = NULL;
}

static void free_preset_images(VkCtx *c) {
    drop_image(c, &P.work, &P.workAlloc, &P.workView);
    drop_image(c, &P.out, &P.outAlloc, &P.outView);
    drop_image(c, &P.fused, &P.fusedAlloc, &P.fusedView);
    for (int i = 0; i < EF_LEVELS; i++) {
        drop_image(c, &P.pyr[i], &P.pyrAlloc[i], &P.pyrView[i]);
        drop_image(c, &P.tmp[i], &P.tmpAlloc[i], &P.tmpView[i]);
    }
    P.pw = P.ph = 0;
    P.loggedPreset = 0;
}

// Los tamaños de la pirámide: el nivel i guarda (ancho >> (i+1), alto >> (i+1)) y su temporal de la pasada
// horizontal (ancho >> (i+1), alto >> i); el nivel 0 es la mitad de la imagen
static uint32_t lvl(uint32_t v, int shift) {
    uint32_t r = v >> shift;
    return r ? r : 1;
}

static int ensure_preset(VkCtx *c, uint32_t w, uint32_t h) {
    if (P.work && P.pw == w && P.ph == h) return 1;
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    free_preset_images(c);
    const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT, RG = VK_FORMAT_R16G16_SFLOAT;
    int ok = make_image(c, F16, w, h, &P.work, &P.workAlloc, &P.workView) &&
             make_image(c, F16, w, h, &P.out, &P.outAlloc, &P.outView) &&
             make_image(c, RG, lvl(w, 1), lvl(h, 1), &P.fused, &P.fusedAlloc, &P.fusedView);
    for (int i = 0; ok && i < EF_LEVELS; i++) {
        ok = make_image(c, RG, lvl(w, i + 1), lvl(h, i + 1), &P.pyr[i], &P.pyrAlloc[i], &P.pyrView[i]);
        if (ok && i >= 1)
            ok = make_image(c, RG, lvl(w, i + 1), lvl(h, i), &P.tmp[i], &P.tmpAlloc[i], &P.tmpView[i]);
    }
    if (!ok) {
        free_preset_images(c);
        return 0;
    }
    P.pw = w, P.ph = h;
    return 1;
}

static void free_cmaa_images(VkCtx *c) {
    drop_image(c, &P.edges, &P.edgesAlloc, &P.edgesView);
    drop_image(c, &P.cand, &P.candAlloc, &P.candView);
    drop_image(c, &P.zsh, &P.zshAlloc, &P.zshView);
    drop_image(c, &P.aout, &P.aoutAlloc, &P.aoutView);
    P.aw = P.ah = 0;
    P.loggedCmaa = 0;
}

static int ensure_cmaa(VkCtx *c, uint32_t w, uint32_t h) {
    if (P.edges && P.aw == w && P.ah == h) return 1;
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    free_cmaa_images(c);
    int ok = make_image(c, VK_FORMAT_R8_UNORM, w, h, &P.edges, &P.edgesAlloc, &P.edgesView) &&
             make_image(c, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, &P.cand, &P.candAlloc, &P.candView) &&
             make_image(c, VK_FORMAT_R8G8B8A8_UNORM, w, h, &P.zsh, &P.zshAlloc, &P.zshView) &&
             make_image(c, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, &P.aout, &P.aoutAlloc, &P.aoutView);
    if (!ok) {
        free_cmaa_images(c);
        return 0;
    }
    P.aw = w, P.ah = h;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

// La vista de origen de la ranura, como en vk_color.c: se destruye cuando la ranura graba otra vez
static VkImageView src_view(VkCtx *c, VkImageView *slots, unsigned slot, VkImage src, VkFormat fmt) {
    slot %= SLOTS;
    if (slots[slot]) vkDestroyImageView(c->device, slots[slot], NULL);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = src,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &slots[slot]) != VK_SUCCESS) slots[slot] = VK_NULL_HANDLE;
    return slots[slot];
}

// Ata una pasada de (muestreador → imagen de almacenamiento) y empuja sus constantes
static void pass2(VkCommandBuffer cb, Pipe *p, VkSampler samp, VkImageView in, VkImageView out, const void *pc,
                  uint32_t pcSize, uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii = {samp, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 2, wr);
    vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
}

// Los dos interruptores, releídos cuando la configuración cambia (ambos son LIVE). El CMAA solo manda
// cuando ningún upscaler temporal está en marcha, como el SMAA del juego al que sustituye en la opción.
static void read_cfg(void) {
    unsigned v = cfg_version();
    if (v == P.seen) return;
    P.seen = v;
    P.presetOn = !strcmp(cfg_value("AC_POST_PRESET"), "elzeldapa");
    P.cmaaOn = !strcmp(cfg_value("AC_ANTI_ALIASING"), "cmaa2") && !vk_dlaa_available();
}

// El nivel más grueso de la pirámide según la tabla LOWEST_LEVEL del original (sobre la mitad de la altura),
// acotado a los 8 niveles que guarda este porte
static int ef_levels(uint32_t h) {
    uint32_t H = h / 2;
    int k = H < 128 ? 3 : H < 256 ? 4 : H < 512 ? 5 : H < 1024 ? 6 : H < 2048 ? 7 : 8;
    return k > EF_LEVELS - 1 ? EF_LEVELS - 1 : k;
}

VkImage vk_preset_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]) {
    read_cfg();
    if (!P.presetOn) return VK_NULL_HANDLE;
    VkCtx *c = vk_ctx();
    if (!P.tried) init(c);
    if (!P.presetOk || !ensure_preset(c, (uint32_t)crop[2], (uint32_t)crop[3])) return VK_NULL_HANDLE;
    VkImageView sv = src_view(c, P.src_view, slot, src, fmt);
    if (!sv) return VK_NULL_HANDLE;
    const uint32_t w = (uint32_t)crop[2], h = (uint32_t)crop[3];
    const uint32_t l0w = lvl(w, 1), l0h = lvl(h, 1);
    const int k = ef_levels(h);
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // las escrituras del juego en src se hacen visibles; el contenido anterior de las imágenes propias se descarta
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, P.work, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // 1. Colourfulness (-0.30, lim_luma 0.7: los valores del preset) sobre el recorte, a la imagen de trabajo
    struct { int32_t off[2], size[2]; float par[4]; } pcC = {{crop[0], crop[1]}, {crop[2], crop[3]}, {-0.30f, 0.7f, 0, 0}};
    pass2(cb, &P.colour, P.sampNear, sv, P.workView, &pcC, sizeof pcC, w, h);
    barrier(cb, P.work, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 2. ExposureFusion siembra el nivel 0 desde la imagen de trabajo
    barrier(cb, P.pyr[0], VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    struct { int32_t off[2], src[2], dst[2]; float intensity, pad; } pcI = {
        {0, 0}, {crop[2], crop[3]}, {(int)l0w, (int)l0h}, 0.5f, 0};
    pass2(cb, &P.efInit, P.sampLin, P.workView, P.pyrView[0], &pcI, sizeof pcI, l0w, l0h);
    barrier(cb, P.pyr[0], VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 3. la pirámide baja nivel a nivel (horizontal al temporal, vertical al nivel)
    for (int i = 1; i <= k; i++) {
        struct { int32_t src[2], dst[2], dir[2]; } pcD = {
            {(int)lvl(w, i), (int)lvl(h, i)}, {(int)lvl(w, i + 1), (int)lvl(h, i)}, {1, 0}};
        barrier(cb, P.tmp[i], VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        pass2(cb, &P.efDown, P.sampLin, P.pyrView[i - 1], P.tmpView[i], &pcD, sizeof pcD, lvl(w, i + 1), lvl(h, i));
        barrier(cb, P.tmp[i], VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        pcD.src[0] = (int)lvl(w, i + 1), pcD.src[1] = (int)lvl(h, i);
        pcD.dst[0] = (int)lvl(w, i + 1), pcD.dst[1] = (int)lvl(h, i + 1);
        pcD.dir[0] = 0, pcD.dir[1] = 1;
        barrier(cb, P.pyr[i], VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        pass2(cb, &P.efDown, P.sampLin, P.tmpView[i], P.pyrView[i], &pcD, sizeof pcD, lvl(w, i + 1), lvl(h, i + 1));
        barrier(cb, P.pyr[i], VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    // 4. la fusión de los niveles, a tamaño del nivel 0
    barrier(cb, P.fused, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        VkDescriptorImageInfo lv[EF_LEVELS], ci2 = {P.sampLin, P.workView, VK_IMAGE_LAYOUT_GENERAL},
                              oi = {VK_NULL_HANDLE, P.fusedView, VK_IMAGE_LAYOUT_GENERAL};
        for (int i = 0; i < EF_LEVELS; i++)
            lv[i] = (VkDescriptorImageInfo){P.sampLin, P.pyrView[i], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = EF_LEVELS,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = lv},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ci2},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
        struct { int32_t dst[2], off[2], src[2]; float eq; int32_t levels; } pcF = {
            {(int)l0w, (int)l0h}, {0, 0}, {crop[2], crop[3]}, 0.5f, k};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.efFuse.pipe);
        rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.efFuse.layout, 0, 3, wr);
        vkCmdPushConstants(cb, P.efFuse.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcF, &pcF);
        vkCmdDispatch(cb, (l0w + 7) / 8, (l0h + 7) / 8, 1);
    }
    barrier(cb, P.fused, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 5. el paso principal: el sesgo fundido sobre el color de la imagen de trabajo, a la salida
    barrier(cb, P.out, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        VkDescriptorImageInfo fi = {P.sampLin, P.fusedView, VK_IMAGE_LAYOUT_GENERAL},
                              ci2 = {P.sampLin, P.workView, VK_IMAGE_LAYOUT_GENERAL},
                              oi = {VK_NULL_HANDLE, P.outView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &fi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ci2},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
        struct { int32_t off[2], size[2], fused[2]; float intensity, target; } pcM = {
            {0, 0}, {crop[2], crop[3]}, {(int)l0w, (int)l0h}, 0.5f, 0.3f};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.efMain.pipe);
        rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.efMain.layout, 0, 3, wr);
        vkCmdPushConstants(cb, P.efMain.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcM, &pcM);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    }
    // lo que venga (FSR o el blit) lee la salida; las lecturas de src terminan aquí
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, CS, 0, ALL, 0);
    barrier(cb, P.out, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
    if (!P.loggedPreset) {
        P.loggedPreset = 1;
        rt_log("vk: image preset ELZELDAPA on a %ux%u picture (Colourfulness + ExposureFusion)", w, h);
    }
    return P.out;
}

VkImage vk_cmaa_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]) {
    read_cfg();
    if (!P.cmaaOn) return VK_NULL_HANDLE;
    VkCtx *c = vk_ctx();
    if (!P.tried) init(c);
    if (!P.cmaaOk || !ensure_cmaa(c, (uint32_t)crop[2], (uint32_t)crop[3])) return VK_NULL_HANDLE;
    VkImageView sv = src_view(c, P.cmaa_view, slot, src, fmt);
    if (!sv) return VK_NULL_HANDLE;
    const uint32_t w = (uint32_t)crop[2], h = (uint32_t)crop[3];
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    struct { int32_t off[2], size[2]; float pad[4]; } pc = {{crop[0], crop[1]}, {crop[2], crop[3]}, {0, 0, 0, 0}};
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 1. la detección de bordes
    barrier(cb, P.edges, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    pass2(cb, &P.cmaaEdges, P.sampNear, sv, P.edgesView, &pc, sizeof pc, w, h);
    barrier(cb, P.edges, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 2. las formas simples y la detección de las Z
    barrier(cb, P.cand, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    barrier(cb, P.zsh, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        VkDescriptorImageInfo si = {P.sampNear, sv, VK_IMAGE_LAYOUT_GENERAL},
                              ei = {P.sampNear, P.edgesView, VK_IMAGE_LAYOUT_GENERAL},
                              co = {VK_NULL_HANDLE, P.candView, VK_IMAGE_LAYOUT_GENERAL},
                              zo = {VK_NULL_HANDLE, P.zshView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[4] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ei},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &co},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &zo}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.cmaaProcess.pipe);
        rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.cmaaProcess.layout, 0, 4, wr);
        vkCmdPushConstants(cb, P.cmaaProcess.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    }
    barrier(cb, P.cand, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, P.zsh, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 3. la mezcla: las líneas Z recogidas sobre los candidatos, a la salida
    barrier(cb, P.aout, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        VkDescriptorImageInfo si = {P.sampLin, sv, VK_IMAGE_LAYOUT_GENERAL},
                              ci2 = {P.sampNear, P.candView, VK_IMAGE_LAYOUT_GENERAL},
                              zi = {P.sampNear, P.zshView, VK_IMAGE_LAYOUT_GENERAL},
                              oi = {VK_NULL_HANDLE, P.aoutView, VK_IMAGE_LAYOUT_GENERAL},
                              ei = {P.sampNear, P.edgesView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[5] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ci2},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ei}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.cmaaBlend.pipe);
        rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.cmaaBlend.layout, 0, 5, wr);
        vkCmdPushConstants(cb, P.cmaaBlend.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    }
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, CS, 0, ALL, 0);
    barrier(cb, P.aout, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL,
            VK_ACCESS_2_MEMORY_READ_BIT);
    if (!P.loggedCmaa) {
        P.loggedCmaa = 1;
        rt_log("vk: CMAA 2 on a %ux%u picture", w, h);
    }
    return P.aout;
}

static void drop_pipe(VkCtx *c, Pipe *p) {
    if (p->pipe) vkDestroyPipeline(c->device, p->pipe, NULL);
    if (p->layout) vkDestroyPipelineLayout(c->device, p->layout, NULL);
    if (p->dsl) vkDestroyDescriptorSetLayout(c->device, p->dsl, NULL);
    p->pipe = VK_NULL_HANDLE, p->layout = VK_NULL_HANDLE, p->dsl = VK_NULL_HANDLE;
}

void vk_preset_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !P.tried) return;
    for (int i = 0; i < SLOTS; i++) {
        if (P.src_view[i]) vkDestroyImageView(c->device, P.src_view[i], NULL);
        if (P.cmaa_view[i]) vkDestroyImageView(c->device, P.cmaa_view[i], NULL);
    }
    free_preset_images(c);
    free_cmaa_images(c);
    drop_pipe(c, &P.colour);
    drop_pipe(c, &P.efInit);
    drop_pipe(c, &P.efDown);
    drop_pipe(c, &P.efFuse);
    drop_pipe(c, &P.efMain);
    drop_pipe(c, &P.cmaaEdges);
    drop_pipe(c, &P.cmaaProcess);
    drop_pipe(c, &P.cmaaBlend);
    if (P.sampNear) vkDestroySampler(c->device, P.sampNear, NULL);
    if (P.sampLin) vkDestroySampler(c->device, P.sampLin, NULL);
    memset(&P, 0, sizeof P);
    P.seen = ~0u;
}
