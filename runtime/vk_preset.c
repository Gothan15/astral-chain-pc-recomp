// El preset de imagen ELZELDAPA antes del HUD (vk_exec.h): dos efectos del preset de ReShade que Gothan
// usa en el port de Zelda, integrados como una capa nativa sobre la imagen de la escena ya terminada,
// antes de que el juego dibuje su HUD encima: sin ReShade y sin tocar el HUD, que queda tal como el
// juego lo dibuja. Es el mismo sitio de la cadena donde vk_dlaa.c afila la escena con RCAS «antes del
// HUD»; el ajuste de color de vk_color.c, en cambio, corre en el present sobre el cuadro entero, HUD
// incluido, y esto no lo sustituye: son dos capas distintas.
//
// El cuadro de Astral Chain, medido por vk_dlaa.c: prepass de profundidad, pasadas sobre el objetivo de
// luz (B10G11R11), la cadena de bloom (el primer nivel a un cuarto de ancho termina la escena), el
// composite (tonemap y grado, otro objetivo HDR con la profundidad de la escena) y las pasadas del SMAA,
// cuya mezcla escribe la imagen RGBA8 sobre la que se dibuja el HUD. El seguidor de abajo reconoce esa
// frontera sin depender de que haya un upscaler temporal en marcha: tras el composite, cada pasada de
// un solo objetivo, sin profundidad y con formato de imagen terminada es candidata a ser la mezcla, y
// la última candidata es la imagen final; cuando el pase siguiente dibuja sobre ella (el HUD), el preset
// se graba primero en el mismo flujo de comandos. Las pasadas del SMAA de bordes y pesos también son
// candidatas y se van relevando: solo la última antes del HUD es la buena.
//
// El preset, en el orden del original:
//   1. Colourfulness -0.30 (shaders/preset_colour.comp): porte de Colourfulness.fx de bacondither
//      (licencia BSD: redistribuible con su aviso, que va en el propio shader).
//   2. ExposureFusion (shaders/preset_ef_*.comp): porte de MartysMods_EXPOSUREFUSION.fx de Pascal
//      Gilcher — PROPIETARIO («unauthorized copying strictly prohibited»): está aquí solo para el uso
//      privado de Gothan y este porte no debe salir de su fork privado. Es un mapeo tonal local
//      Retinex: siembra una pirámide de (sesgo, log-luma) a mitad de resolución, la baja con un
//      Kuwahara separable que respeta los bordes, funde los niveles de abajo arriba y aplica el sesgo
//      como factor 2^sesgo sobre el color expandido a HDR por la curva del propio shader.
// La salida (RGBA16F) se dibuja sobre la imagen del juego con un triángulo de pantalla completa
// (shaders/preset_copy.frag, el vértice de hdr_fullscreen.vert), como la aplicación del AO de vk_ao.c:
// la imagen del juego no admite una copia directa desde otro formato ni escritura por almacenamiento
// garantizada, y como objetivo de color sí admite que se le dibuje encima.
//
// Límites conocidos: con el HUD apagado (AC_HUD=0) ningún pase dibuja sobre la imagen tras la mezcla,
// así que en esos cuadros el preset no se aplica; y lo que queda pendiente al presentarse el cuadro se
// descarta (un menú sobre el cuadro congelado no recibe el preset tarde). La clave es AC_POST_PRESET
// (off|elzeldapa, en directo), solo en el ini por ahora: el menú no la muestra.
#include "vk.h"

#include <string.h>

#include "cfg.h"
#include "hdr_fullscreen_spv.h"
#include "host.h"
#include "preset_colour_spv.h"
#include "preset_copy_spv.h"
#include "preset_ef_down_spv.h"
#include "preset_ef_fuse_spv.h"
#include "preset_ef_init_spv.h"
#include "preset_ef_main_spv.h"
#include "vk_exec.h"
#include "vk_rec.h"

#define EF_LEVELS 8   // los niveles 0..7 de la pirámide (el original llega a 9 solo por encima de 4K)
#define MAX_COPY 4    // tuberías de la copia final, una por formato de imagen del juego
#define RETIRE_N 2    // juegos de imágenes retirados que esperan unos presents antes de destruirse

// Una tubería de cómputo con su propio layout (las pasadas de este módulo atan entre 2 y 5 descriptores)
typedef struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
} Pipe;

// Las imágenes de trabajo de un tamaño: la de Colourfulness, la salida, el nivel fundido y la pirámide
typedef struct {
    VkImage work, out, fused;
    VmaAllocation workAlloc, outAlloc, fusedAlloc;
    VkImageView workView, outView, fusedView;
    VkImage pyr[EF_LEVELS], tmp[EF_LEVELS];
    VmaAllocation pyrAlloc[EF_LEVELS], tmpAlloc[EF_LEVELS];
    VkImageView pyrView[EF_LEVELS], tmpView[EF_LEVELS];
    uint32_t w, h;
    int logged, age;   // age: presents desde que se retiró (solo en los retirados)
} Set;

static struct {
    int tried, presetOk, copyOk;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler sampNear, sampLin;
    Pipe colour, efInit, efDown, efFuse, efMain;   // el preset
    VkDescriptorSetLayout copyDsl;                 // la copia final: un muestreador en el fragmento
    VkPipelineLayout copyLayout;
    struct { VkFormat fmt; VkPipeline pipe; } copy[MAX_COPY];
    int ncopy;
    Set cur, retired[RETIRE_N];
    unsigned seen;      // la versión de cfg con la que se leyó el interruptor
    int presetOn;
    int nrec;           // los comandos grabados por la aplicación en curso (ex->recorded)
    // el seguidor de la frontera escena/HUD
    int stage;          // 0 nada, 1 prepass visto, 2 escena armada, 3 escena terminada, 4 composite
                        // visto, 5 imagen final lista y preset pendiente, 6 preset aplicado
    VkExecTarget light; // el objetivo de luz armado (su tamaño y su profundidad mandan)
    const VkTexImage *depthImg;
    VkExecTarget pic;   // la imagen final del cuadro, la que el HUD dibuja encima
    uint64_t serial;    // sube con cada imagen final nueva
    uint64_t picSerial; // el serial de la imagen final pendiente
    uint64_t presentedSerial;   // el serial que el último present ya cubrió (vk_preset_present)
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

static VkShaderModule module(VkDevice d, const uint32_t *spv, size_t bytes) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = spv};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(d, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// La tubería gráfica de la copia final para un formato de imagen del juego (sin mezcla: sustituye)
static VkPipeline copy_pipeline(VkFormat fmt) {
    for (int i = 0; i < P.ncopy; i++)
        if (P.copy[i].fmt == fmt) return P.copy[i].pipe;
    if (P.ncopy == MAX_COPY || !P.copyOk) return VK_NULL_HANDLE;
    VkCtx *c = vk_ctx();
    VkShaderModule vs = module(c->device, hdr_fullscreen_spv, sizeof hdr_fullscreen_spv),
                   fs = module(c->device, preset_copy_spv, sizeof preset_copy_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                       .layout = P.copyLayout};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &p) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    if (r != VK_SUCCESS) {
        rt_log("vk: image preset: no copy pipeline for format %d: skipped", (int)fmt);
        return VK_NULL_HANDLE;
    }
    P.copy[P.ncopy].fmt = fmt, P.copy[P.ncopy].pipe = p, P.ncopy++;
    return p;
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
    P.presetOk = make_pipe(c, &P.colour, preset_colour_spv, sizeof preset_colour_spv, inOut, 2, 32) &&
                 make_pipe(c, &P.efInit, preset_ef_init_spv, sizeof preset_ef_init_spv, inOut, 2, 32) &&
                 make_pipe(c, &P.efDown, preset_ef_down_spv, sizeof preset_ef_down_spv, inOut, 2, 24) &&
                 make_pipe(c, &P.efFuse, preset_ef_fuse_spv, sizeof preset_ef_fuse_spv, fuseB, 3, 32) &&
                 make_pipe(c, &P.efMain, preset_ef_main_spv, sizeof preset_ef_main_spv, mainB, 3, 32);
    if (!P.presetOk) rt_log("vk: image preset off: its pipelines could not be created");
    // la copia final: un muestreador en el fragmento, sin constantes
    const VkDescriptorSetLayoutBinding copyB = {.binding = 0,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 1, .pBindings = &copyB};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &P.copyDsl};
    P.copyOk = vkCreateDescriptorSetLayout(c->device, &dci, NULL, &P.copyDsl) == VK_SUCCESS &&
               vkCreatePipelineLayout(c->device, &li, NULL, &P.copyLayout) == VK_SUCCESS;
    if (!P.copyOk) rt_log("vk: image preset off: its copy layout could not be created");
    return P.presetOk && P.copyOk;
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

static void free_set(VkCtx *c, Set *s) {
    drop_image(c, &s->work, &s->workAlloc, &s->workView);
    drop_image(c, &s->out, &s->outAlloc, &s->outView);
    drop_image(c, &s->fused, &s->fusedAlloc, &s->fusedView);
    for (int i = 0; i < EF_LEVELS; i++) {
        drop_image(c, &s->pyr[i], &s->pyrAlloc[i], &s->pyrView[i]);
        drop_image(c, &s->tmp[i], &s->tmpAlloc[i], &s->tmpView[i]);
    }
    s->w = s->h = 0, s->logged = 0;
}

// Los tamaños de la pirámide: el nivel i guarda (ancho >> (i+1), alto >> (i+1)) y su temporal de la pasada
// horizontal (ancho >> (i+1), alto >> i); el nivel 0 es la mitad de la imagen
static uint32_t lvl(uint32_t v, int shift) {
    uint32_t r = v >> shift;
    return r ? r : 1;
}

static int build_set(VkCtx *c, Set *s, uint32_t w, uint32_t h) {
    const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT, RG = VK_FORMAT_R16G16_SFLOAT;
    int ok = make_image(c, F16, w, h, &s->work, &s->workAlloc, &s->workView) &&
             make_image(c, F16, w, h, &s->out, &s->outAlloc, &s->outView) &&
             make_image(c, RG, lvl(w, 1), lvl(h, 1), &s->fused, &s->fusedAlloc, &s->fusedView);
    for (int i = 0; ok && i < EF_LEVELS; i++) {
        ok = make_image(c, RG, lvl(w, i + 1), lvl(h, i + 1), &s->pyr[i], &s->pyrAlloc[i], &s->pyrView[i]);
        if (ok && i >= 1)
            ok = make_image(c, RG, lvl(w, i + 1), lvl(h, i), &s->tmp[i], &s->tmpAlloc[i], &s->tmpView[i]);
    }
    if (!ok) {
        free_set(c, s);
        return 0;
    }
    s->w = w, s->h = h;
    return 1;
}

// El juego de imágenes del tamaño pedido. Al cambiar de tamaño el juego viejo se retira y se destruye
// unos presents después (vk_preset_present), no en mitad del flujo: sus comandos pueden seguir en vuelo.
static Set *ensure_set(VkCtx *c, uint32_t w, uint32_t h) {
    if (P.cur.w == w && P.cur.h == h) return &P.cur;
    if (P.cur.w) {
        Set *slot = NULL;
        for (int i = 0; i < RETIRE_N; i++)
            if (!P.retired[i].w) slot = &P.retired[i];
        if (!slot) {   // dos cambios de tamaño seguidos dentro de la ventana de retiro: se destruye el más viejo
            slot = &P.retired[0];
            free_set(c, slot);
        }
        *slot = P.cur;
        slot->age = 0;
        memset(&P.cur, 0, sizeof P.cur);
    }
    return build_set(c, &P.cur, w, h) ? &P.cur : NULL;
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
    P.nrec++;
}

// Una barrera de memoria completa, como la de vk_contact.c tras tocar la imagen del juego
static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
    P.nrec++;
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
    P.nrec += 4;
}

// El interruptor, releído cuando la configuración cambia (es LIVE)
static void read_cfg(void) {
    unsigned v = cfg_version();
    if (v == P.seen) return;
    P.seen = v;
    P.presetOn = !strcmp(cfg_value("AC_POST_PRESET"), "elzeldapa");
    if (!P.presetOn) P.stage = 0;
}

// El nivel más grueso de la pirámide según la tabla LOWEST_LEVEL del original (sobre la mitad de la altura),
// acotado a los 8 niveles que guarda este porte
static int ef_levels(uint32_t h) {
    uint32_t H = h / 2;
    int k = H < 128 ? 3 : H < 256 ? 4 : H < 512 ? 5 : H < 1024 ? 6 : H < 2048 ? 7 : 8;
    return k > EF_LEVELS - 1 ? EF_LEVELS - 1 : k;
}

// La cadena del preset sobre la imagen terminada (origen = la imagen del juego, a tamaño completo): deja
// el resultado en la salida del juego de imágenes, RGBA16F. Cuenta sus comandos en P.nrec.
static void run_chain(VkCommandBuffer cb, Set *s, VkImage src, VkImageView sv, uint32_t w, uint32_t h) {
    const uint32_t l0w = lvl(w, 1), l0h = lvl(h, 1);
    const int k = ef_levels(h);
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // las escrituras del juego en src se hacen visibles; el contenido anterior de las imágenes propias se descarta
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, s->work, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // 1. Colourfulness (-0.30, lim_luma 0.7: los valores del preset) sobre la imagen, a la de trabajo
    struct { int32_t off[2], size[2]; float par[4]; } pcC = {{0, 0}, {(int)w, (int)h}, {-0.30f, 0.7f, 0, 0}};
    pass2(cb, &P.colour, P.sampNear, sv, s->workView, &pcC, sizeof pcC, w, h);
    barrier(cb, s->work, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 2. ExposureFusion siembra el nivel 0 desde la imagen de trabajo
    barrier(cb, s->pyr[0], VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    struct { int32_t off[2], src[2], dst[2]; float intensity, pad; } pcI = {
        {0, 0}, {(int)w, (int)h}, {(int)l0w, (int)l0h}, 0.5f, 0};
    pass2(cb, &P.efInit, P.sampLin, s->workView, s->pyrView[0], &pcI, sizeof pcI, l0w, l0h);
    barrier(cb, s->pyr[0], VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 3. la pirámide baja nivel a nivel (horizontal al temporal, vertical al nivel)
    for (int i = 1; i <= k; i++) {
        struct { int32_t src[2], dst[2], dir[2]; } pcD = {
            {(int)lvl(w, i), (int)lvl(h, i)}, {(int)lvl(w, i + 1), (int)lvl(h, i)}, {1, 0}};
        barrier(cb, s->tmp[i], VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        pass2(cb, &P.efDown, P.sampLin, s->pyrView[i - 1], s->tmpView[i], &pcD, sizeof pcD, lvl(w, i + 1), lvl(h, i));
        barrier(cb, s->tmp[i], VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        pcD.src[0] = (int)lvl(w, i + 1), pcD.src[1] = (int)lvl(h, i);
        pcD.dst[0] = (int)lvl(w, i + 1), pcD.dst[1] = (int)lvl(h, i + 1);
        pcD.dir[0] = 0, pcD.dir[1] = 1;
        barrier(cb, s->pyr[i], VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        pass2(cb, &P.efDown, P.sampLin, s->tmpView[i], s->pyrView[i], &pcD, sizeof pcD, lvl(w, i + 1), lvl(h, i + 1));
        barrier(cb, s->pyr[i], VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    // 4. la fusión de los niveles, a tamaño del nivel 0
    barrier(cb, s->fused, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        VkDescriptorImageInfo lv[EF_LEVELS], ci2 = {P.sampLin, s->workView, VK_IMAGE_LAYOUT_GENERAL},
                              oi = {VK_NULL_HANDLE, s->fusedView, VK_IMAGE_LAYOUT_GENERAL};
        for (int i = 0; i < EF_LEVELS; i++)
            lv[i] = (VkDescriptorImageInfo){P.sampLin, s->pyrView[i], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = EF_LEVELS,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = lv},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ci2},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
        struct { int32_t dst[2], off[2], src[2]; float eq; int32_t levels; } pcF = {
            {(int)l0w, (int)l0h}, {0, 0}, {(int)w, (int)h}, 0.5f, k};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.efFuse.pipe);
        rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.efFuse.layout, 0, 3, wr);
        vkCmdPushConstants(cb, P.efFuse.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcF, &pcF);
        vkCmdDispatch(cb, (l0w + 7) / 8, (l0h + 7) / 8, 1);
        P.nrec += 4;
    }
    barrier(cb, s->fused, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 5. el paso principal: el sesgo fundido sobre el color de la imagen de trabajo, a la salida
    barrier(cb, s->out, VK_IMAGE_LAYOUT_UNDEFINED, ALL, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        VkDescriptorImageInfo fi = {P.sampLin, s->fusedView, VK_IMAGE_LAYOUT_GENERAL},
                              ci2 = {P.sampLin, s->workView, VK_IMAGE_LAYOUT_GENERAL},
                              oi = {VK_NULL_HANDLE, s->outView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &fi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ci2},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
        struct { int32_t off[2], size[2], fused[2]; float intensity, target; } pcM = {
            {0, 0}, {(int)w, (int)h}, {(int)l0w, (int)l0h}, 0.5f, 0.3f};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.efMain.pipe);
        rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.efMain.layout, 0, 3, wr);
        vkCmdPushConstants(cb, P.efMain.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcM, &pcM);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        P.nrec += 4;
    }
    // la copia final lee la salida como textura; las lecturas de src terminan aquí
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, CS, 0, ALL, 0);
    barrier(cb, s->out, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (!s->logged) {
        s->logged = 1;
        rt_log("vk: image preset ELZELDAPA before the HUD on a %ux%u picture (Colourfulness + ExposureFusion)", w, h);
    }
}

// Aplica el preset a la imagen final pendiente, en el flujo del cuadro y antes del pase que va a dibujar
// sobre ella: la cadena de cómputo y, encima, la copia dibujada de vuelta a la imagen del juego.
static void apply(VkExec *ex) {
    VkCtx *c = vk_ctx();
    if (!P.tried) init(c);
    if (!P.presetOk || !P.copyOk) return;
    const VkExecTarget *pic = &P.pic;
    Set *s = ensure_set(c, pic->width, pic->height);
    if (!s) return;
    VkPipeline cp = copy_pipeline(pic->format);
    if (!cp) return;
    VkImageView pv = vk_exec_target_view(pic->img, pic->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                         pic->level, pic->base_layer, 1, 0);
    if (!pv) return;
    VkCommandBuffer cb = ex->cb;
    vk_exec_clears_flush(ex);   // un clear de la imagen que espera a su pase va antes que el preset
    P.nrec = 0;
    run_chain(cb, s, pic->image, pv, pic->width, pic->height);
    // la copia final: la salida dibujada sobre la imagen del juego, sin mezcla
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = pv,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {pic->width, pic->height}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cp);
    VkViewport v = {0, 0, (float)pic->width, (float)pic->height, 0, 1};
    VkRect2D sc = {{0, 0}, {pic->width, pic->height}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkDescriptorImageInfo ii = {P.sampNear, s->outView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P.copyLayout, 0, 1, &wr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    P.nrec += 7;
    mem_barrier(cb);   // y lo que sigue (el HUD) ve la imagen ya tratada
    ex->recorded += P.nrec;
    if (pic->tex) vk_texture_gpu_written(pic->tex);
    vk_exec_draw_new_cb();   // las tuberías y conjuntos del juego se sustituyeron: el dibujo siguiente ata todo
    ex->dirty = VK_EXEC_DIRTY_ALL;
}

// Un objetivo HDR de escena (el de luz y el composite), como scene_color de vk_dlaa.c y vk_ao.c
static int hdr_format(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

// Una profundidad de escena D32F(S8) de al menos 640 de ancho, como scene_depth de vk_dlaa.c
static int scene_depth(const VkExecTarget *z) {
    return z->image && z->width >= 640 && (z->format == VK_FORMAT_D32_SFLOAT_S8_UINT || z->format == VK_FORMAT_D32_SFLOAT);
}

// Los formatos de la imagen terminada que el HUD cubre: la RGBA8 medida y, por si acaso, la RGBA16F
static int pic_format(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

// El seguidor de la frontera, llamado cuando un pase va a empezar (vk_exec_pass_begin, con vk_ao_pass y
// vk_dlaa_pass): primero atiende lo pendiente y después sigue la estructura del cuadro.
void vk_preset_pass(VkExec *ex) {
    read_cfg();
    if (!P.presetOn) return;
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    if (P.stage == 5) {
        if (P.presentedSerial >= P.picSerial) {
            P.stage = 0;   // esa imagen ya se presentó: lo pendiente era del cuadro anterior, se descarta
        } else if (ex->pass_ncolor >= 1 && c->image == P.pic.image) {
            apply(ex);   // el pase siguiente (el HUD) dibuja sobre la imagen: el preset va primero
            P.stage = 6;
            return;
        }
    }
    // el prepass de profundidad (o un pase de shadow map: sin objetivo de color, inofensivo aquí)
    if (ex->pass_ncolor == 0 && scene_depth(z)) {
        if (P.stage != 1 && P.stage != 2) P.stage = 1;
        return;
    }
    // una pasada sobre el objetivo de luz; una pareja menor nunca releva a la armada
    if ((P.stage == 1 || P.stage == 2) && ex->pass_ncolor == 1 && c->image && hdr_format(c->format) && z->image &&
        c->width == z->width && c->height == z->height && c->samples <= 1 && !c->is3d &&
        (P.stage == 1 || (c->width >= P.light.width && c->height >= P.light.height))) {
        P.light = *c, P.depthImg = z->img, P.stage = 2;
        return;
    }
    if (P.stage == 2) {
        if (!ex->pass_w || ex->pass_w * 4 > P.light.width) return;   // la escena sigue
        P.stage = 3;   // el primer pase a un cuarto de ancho o menos (el bloom) termina la escena
    }
    if (P.stage == 3) {
        // el composite: un objetivo HDR del tamaño de la luz con la profundidad de la escena
        if (ex->pass_ncolor == 1 && c->image && hdr_format(c->format) && z->img == P.depthImg &&
            c->width == P.light.width && c->height == P.light.height && c->samples <= 1 && !c->is3d)
            P.stage = 4;
        return;
    }
    if (P.stage >= 4) {
        // candidatas a imagen final: un solo objetivo, sin profundidad, formato de imagen terminada; la
        // última antes del HUD es la buena (las de bordes y pesos del SMAA se relevan entre sí)
        if (ex->pass_ncolor == 1 && c->image && !z->image && pic_format(c->format) && c->width >= 640 &&
            c->samples <= 1 && !c->is3d) {
            P.pic = *c, P.picSerial = ++P.serial, P.stage = 5;
        }
        return;
    }
}

// En cada present (vk_backend.c, con vk_ao_frame): lo pendiente queda cubierto por este present, y los
// juegos de imágenes retirados envejecen hasta destruirse.
void vk_preset_present(void) {
    if (P.stage == 5) P.presentedSerial = P.picSerial;
    VkCtx *c = vk_ctx();
    if (!c || !P.tried) return;
    for (int i = 0; i < RETIRE_N; i++)
        if (P.retired[i].w && ++P.retired[i].age >= 3) free_set(c, &P.retired[i]);
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
    free_set(c, &P.cur);
    for (int i = 0; i < RETIRE_N; i++) free_set(c, &P.retired[i]);
    drop_pipe(c, &P.colour);
    drop_pipe(c, &P.efInit);
    drop_pipe(c, &P.efDown);
    drop_pipe(c, &P.efFuse);
    drop_pipe(c, &P.efMain);
    for (int i = 0; i < P.ncopy; i++)
        if (P.copy[i].pipe) vkDestroyPipeline(c->device, P.copy[i].pipe, NULL);
    if (P.copyLayout) vkDestroyPipelineLayout(c->device, P.copyLayout, NULL);
    if (P.copyDsl) vkDestroyDescriptorSetLayout(c->device, P.copyDsl, NULL);
    if (P.sampNear) vkDestroySampler(c->device, P.sampNear, NULL);
    if (P.sampLin) vkDestroySampler(c->device, P.sampLin, NULL);
    memset(&P, 0, sizeof P);
    P.seen = ~0u;
}
