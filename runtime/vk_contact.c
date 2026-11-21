// Screen-space contact shadows (AC_CONTACT_SHADOWS, percent strength, 0 = off). Frame structure as captured in a
// real dump (frame 1604, 2026-11-20): a depth prepass writes the scene depth (D32FS8), the scene pass writes the
// B10G11R11 scene colour against that same depth, a one-draw pass converts the depth into an R32F linear depth,
// and a later pass composites the lighting back into the scene colour, again with the scene depth attached. When
// the first pass that no longer writes the scene colour begins, runtime/shaders/contact_shadow.comp marches each
// pixel a short way toward the sun through the linear depth and darkens the scene colour where the ray passes
// behind nearby geometry: the small shadows at feet, under stones and in grass that the shadow maps are too
// coarse for. Transparent surfaces, the sky and post-processing come after, untouched.
//
// Inputs from the game, read from the fragment stage's uniform block c4 bound in the composite pass (guest memory,
// so it may be one frame newer than the replay): the projection scales c4[7].x and c4[8].y, the far plane c4[14].y,
// and the first sun cascade's depth row c4[44].xyz, whose negated direction points from a view-space position toward
// the sun (the G-buffer shaders compare dot(c4[44].xyz, pos) + c4[44].w with the cascade map). The scene colour images
// get STORAGE usage when the setting is on (vk_texture.c create_tex).
#include "vk.h"

#include <math.h>
#include <string.h>

#include "cfg.h"
#include "contact_shadow_spv.h"
#include "host.h"
#include "vk_exec.h"

static struct {
    int tried, cfg_loaded, ok, strength, debug, logged;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    const VkTexImage *scene, *depth;   // the last G-buffer's scene colour and linear depth
    uint32_t w, h;
    int paired;                        // la profundidad lineal de la escena recordada ya se escribió en su pase
    int pending;                       // the composite into scene ran; the march waits for the next pass
    uint64_t c4;
    // Diagnóstico de una sola vez por puerta (2026-11-20: en la sesión de Gothan la marcha no disparó y
    // ninguna puerta escribía nada): cada línea marca hasta dónde llegó la cadena en una corrida suya.
    int d_scene, d_storage, d_pair, d_comp, d_inpass, d_sanity, d_views;
    uint64_t c4_size;                  // el último tamaño visto en el ubo[1][1] de fragmento
} C;

int vk_contact_strength(void) {
    // La configuración se carga una sola vez y con su PROPIA marca (cfg_loaded): no puede depender
    // de C.tried, que significa «init() ya corrió». Antes la carga iba tras !C.tried, pero init()
    // pone tried = 1 antes de llamar aquí, así que por esa vía la fuerza quedaba a 0 y el módulo
    // nacía muerto; solo se salvaba si la textura de color de escena se creaba antes que el primer
    // pase (vk_texture.c llama aquí al decidir el uso STORAGE), y si un pase cualquiera empezaba
    // antes, las sombras quedaban desactivadas toda la sesión y la textura nacía sin STORAGE.
    // (Revisión de ChatGPT del 2026-11-20, verificada contra este código; el fallo viene del base.)
    if (!C.cfg_loaded) {
        C.cfg_loaded = 1;
        int s = cfg_int("AC_CONTACT_SHADOWS");
        C.strength = s < 0 ? 0 : s > 100 ? 100 : s;
    }
    return C.strength;
}

static int init(VkCtx *c) {
    C.tried = 1;
    vk_contact_strength();
    if (!C.strength) return 0;
    const char *e = cfg_get("AC_CONTACT_DEBUG");   // 1: the frame shows the contact shadow factor in grey
    C.debug = e && *e && strcmp(e, "0");
    C.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &C.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof contact_shadow_spv, .pCode = contact_shadow_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!C.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &C.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &C.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &C.sampler) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: contact shadows off: their pipeline could not be created");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = C.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &C.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("vk: contact shadows off: their pipeline could not be created");
        return 0;
    }
    rt_log("vk: contact shadows on, strength %d%%%s", C.strength, C.debug ? " (debug view)" : "");
    return C.ok = 1;
}

// Detección re-apuntada al frame real (dump del 2026-11-20, frame 1604). La detección original pedía el
// G-buffer con los tres destinos en el MISMO pase (color B10G11R11 + profundidad lineal R32F + profundidad
// D32F) y un composite con profundidad D16, y ninguna de las dos cosas existe en el frame del juego: la
// profundidad de escena es D32FS8 (130), no D32F (126); la R32F se escribe en un pase aparte de un solo
// draw que lee esa profundidad; y el composite escribe la escena con la misma profundidad 130 adjunta.
// Por eso la marcha no disparó nunca para nadie en esta build. La detección nueva sigue la secuencia real:
//   1. Pase que escribe el color de escena (B10G11R11) con la profundidad de escena adjunta = pase de
//      escena: se recuerda el color y se espera su profundidad lineal.
//   2. Pase que escribe SOLO una R32F del mismo tamaño = profundidad lineal: queda emparejada.
//   3. Pase que vuelve a escribir el color de escena ya emparejado = composite: la marcha queda pendiente
//      y sale cuando empieza el primer pase que ya no escribe la escena (vk_contact_flush).
// El AO (vk_ao.c) acepta la misma pareja de formatos de profundidad, D32F o D32FS8: de ahí la lista.
static const VkTexImage *scene_target(const VkExec *ex) {
    if (ex->pass_noatt || ex->pass_w < 640 || ex->depth.level) return NULL;
    if (ex->depth.format != VK_FORMAT_D32_SFLOAT && ex->depth.format != VK_FORMAT_D32_SFLOAT_S8_UINT) return NULL;
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) {
        const VkExecTarget *t = &ex->color[i];
        if (!t->image || t->level || t->width != ex->pass_w || t->height != ex->pass_h) continue;
        if (t->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
            if (t->img->usage & VK_IMAGE_USAGE_STORAGE_BIT) return t->img;
            if (!C.d_storage) {
                C.d_storage = 1;
                rt_log("vk: contact diag: the %ux%u B10G11R11 scene colour has no STORAGE usage; the march "
                       "cannot write it",
                       t->width, t->height);
            }
        }
    }
    return NULL;
}

// El pase de la profundidad lineal: un único destino R32F del tamaño de la escena recordada.
static const VkTexImage *lindepth_target(const VkExec *ex) {
    if (ex->pass_noatt || !C.scene || ex->pass_ncolor != 1) return NULL;
    const VkExecTarget *t = &ex->color[0];
    if (!t->image || t->level || t->format != VK_FORMAT_R32_SFLOAT) return NULL;
    if (t->width != C.w || t->height != C.h) return NULL;
    return t->img;
}

static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

void vk_contact_flush(VkExec *ex) {
    if (!C.pending) return;
    C.pending = 0;
    if (ex->in_pass) {
        if (!C.d_inpass) {
            C.d_inpass = 1;
            rt_log("vk: contact diag: the march was dropped: a pass was still open at the flush point");
        }
        return;
    }
    const float *u = (const float *)(uintptr_t)C.c4;
    float lx = -u[44 * 4 + 0], ly = -u[44 * 4 + 1], lz = -u[44 * 4 + 2], ll = sqrtf(lx * lx + ly * ly + lz * lz);
    float px = u[7 * 4 + 0], py = u[8 * 4 + 1], zfar = u[14 * 4 + 1];
    if (!(ll > 1e-6f) || !(px > 0) || !(py > 0) || !(zfar > 1)) {
        if (!C.d_sanity) {
            C.d_sanity = 1;
            rt_log("vk: contact diag: the march is skipped: the camera block reads sun length %g, projection "
                   "%g/%g, far %g",
                   (double)ll, (double)px, (double)py, (double)zfar);
        }
        return;
    }
    VkImageView in = vk_exec_target_view(C.depth, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, 0, 1, 0);
    VkImageView out = vk_exec_target_view(C.scene, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_VIEW_TYPE_2D,
                                          VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    if (!in || !out) {
        if (!C.d_views) {
            C.d_views = 1;
            rt_log("vk: contact diag: the march is skipped: a target view could not be made");
        }
        return;
    }
    VkCommandBuffer cb = ex->cb;
    float pcv[16] = {px, py, zfar, (float)C.strength / 100.0f,
                     lx / ll, ly / ll, lz / ll, 0.6f,
                     (float)C.w, (float)C.h, 1.0f / (float)C.w, 1.0f / (float)C.h,
                     (float)C.debug, 0, 0, 0};
    if (!C.logged) {
        C.logged = 1;
        rt_log("vk: contact shadows on a %ux%u scene, sun toward %.3f %.3f %.3f (view space), far %g", C.w, C.h,
               pcv[4], pcv[5], pcv[6], zfar);
    }
    vk_exec_clears_flush(ex);   // a clear of the scene that waits for its pass comes before the march
    mem_barrier(cb);   // the compose passes' writes, before the march reads and writes them
    VkDescriptorImageInfo ii = {C.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe);
    rec_push(C.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.layout, 0, 2, w);
    vkCmdPushConstants(cb, C.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcv, pcv);
    vkCmdDispatch(cb, (C.w + 7) / 8, (C.h + 7) / 8, 1);
    mem_barrier(cb);   // and what follows sees the darkened colour
    ex->recorded += 6;
    vk_exec_draw_new_cb();   // the game's bound compute pipeline and descriptors were replaced: bind them again
}

// The lighting composite: the remembered scene colour as the only target, once its linear depth is paired.
// (Antes pedía una profundidad D16 que el juego no usa en ningún pase del frame: nunca casaba.)
static int is_composite(const VkExec *ex) {
    return C.scene && C.paired && ex->pass_ncolor == 1 && ex->color[0].img == C.scene && !ex->color[0].level &&
           ex->pass_w == C.w && ex->pass_h == C.h;
}

void vk_contact_pass_begin(VkExec *ex) {
    if (!C.tried && !init(vk_ctx())) return;
    if (!C.ok) return;
    // The camera block: c4 of the fragment stage, 0x940 bytes. The composite binds it only after its pass begins, so
    // the newest one seen at any pass start serves.
    const p_bind_buf *b = &ex->ubo[1][1];
    if (b->addr) {
        C.c4_size = b->size;
        if (b->size == 0x940) C.c4 = b->addr;
    }
    const VkTexImage *sc = scene_target(ex);
    if (sc && !(C.paired && sc == C.scene)) {
        if (!C.d_scene) {
            C.d_scene = 1;
            rt_log("vk: contact diag: scene pass recognized (%ux%u)", ex->pass_w, ex->pass_h);
        }
        // Pase de escena (su profundidad lineal de este frame aún no se escribió, o es una escena nueva):
        // empieza la pareja. La marca paired es la que distingue este pase del composite: ambos escriben
        // el mismo color con la misma profundidad adjunta, y solo el orden (la R32F llega entre los dos)
        // los separa. El composite consume la marca, así que el pase de escena del frame siguiente vuelve
        // a entrar por aquí aunque la pareja anterior siga recordada.
        C.pending = 0;
        C.scene = sc, C.depth = NULL, C.paired = 0, C.w = ex->pass_w, C.h = ex->pass_h;
        return;
    }
    if (is_composite(ex)) {
        C.pending = C.c4 != 0;
        if (!C.d_comp) {
            C.d_comp = 1;
            if (C.c4)
                rt_log("vk: contact diag: composite seen; the march is pending");
            else
                rt_log("vk: contact diag: composite seen but no 0x940 camera block at the fragment ubo[1][1] "
                       "(last size there 0x%llx); the march stays off",
                       (unsigned long long)C.c4_size);
        }
        C.paired = 0;   // consumida: la próxima escritura de la escena ya es el pase de escena de otro frame
        return;
    }
    const VkTexImage *ld = lindepth_target(ex);
    if (ld) {
        C.depth = ld;   // la profundidad lineal de esta escena, escrita en su propio pase
        C.paired = 1;
        if (!C.d_pair) {
            C.d_pair = 1;
            rt_log("vk: contact diag: linear depth paired (%ux%u)", C.w, C.h);
        }
        return;
    }
    vk_contact_flush(ex);
}

void vk_contact_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !C.tried) return;
    if (C.pipe) vkDestroyPipeline(c->device, C.pipe, NULL);
    if (C.sampler) vkDestroySampler(c->device, C.sampler, NULL);
    if (C.layout) vkDestroyPipelineLayout(c->device, C.layout, NULL);
    if (C.dsl) vkDestroyDescriptorSetLayout(c->device, C.dsl, NULL);
    memset(&C, 0, sizeof C);
}
