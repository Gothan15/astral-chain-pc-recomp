// La opción «reshade» (AC_RESHADE): los pases de la fase HUD del juego se redirigen a una imagen propia
// (la capa del HUD) para que la textura de ventana llegue al present con la escena sola; ReShade aplica
// su preset sobre la escena y el addon compañero compone la capa encima (ver vk_reshade.h).
//
// Cómo se reconoce la fase HUD: el juego escribe la escena en la textura de ventana con un primer pase
// (el composite con su SMAA) y después dibuja el HUD y los menús en esa misma textura hasta presentarla.
// Así que, dentro de un frame, el primer pase sobre la textura de ventana es la escena y los siguientes
// son HUD. La textura de ventana se conoce porque vk_backend.c publica la de cada present; el primer
// frame de la partida no redirige (aún no se conoce) y cualquier frame sin addon cargado va normal.
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_reshade.h"

static int g_on;              // la opción está activa y el addon presente (se decide en cada present)
static VkImage g_win;         // la imagen de la textura de ventana del último present
static int g_win_passes;      // los pases sobre la textura de ventana en lo que va de frame
static int g_redir;           // el pase abierto está redirigido a la capa
static int g_hud_started;     // la capa ya se limpió en este frame (solo se limpia en su primer pase)
static uint64_t g_frame;      // el contador de presents

static VkImage g_hud;             // la capa del HUD
static VmaAllocation g_hud_mem;
static VkImageView g_hud_view;    // su vista de color, la que usa el adjunto redirigido
static VkFormat g_hud_fmt;
static uint32_t g_hud_w, g_hud_h;
static int g_hud_fresh;           // recién creada: su primera barrera parte de UNDEFINED

// Una barrera de imagen sobre la capa, en el estilo de la casa (sincronización 2, layout GENERAL: la capa
// vive en GENERAL como las imágenes del invitado cuando son objetivo de un pase).
static void hud_barrier(VkCommandBuffer cb, VkImageLayout from, VkPipelineStageFlags2 s0, VkAccessFlags2 a0,
                        VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = s0, .srcAccessMask = a0,
                               .dstStageMask = s1, .dstAccessMask = a1, .oldLayout = from,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_hud,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

// La capa existe y tiene el tamaño y el formato del objetivo; si cambia, la vieja se retira con
// destrucción diferida (un frame en vuelo o el addon pueden estar leyéndola todavía).
static int hud_ensure(const VkExecTarget *t) {
    if (g_hud && g_hud_w == t->width && g_hud_h == t->height && g_hud_fmt == t->format) return 1;
    const VkCtx *c = vk_ctx();
    if (g_hud) {
        vk_defer_view(g_hud_view);
        vk_defer_image(g_hud, g_hud_mem);
        g_hud = VK_NULL_HANDLE, g_hud_view = VK_NULL_HANDLE, g_hud_mem = NULL;
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = t->format, .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &g_hud, &g_hud_mem, NULL) != VK_SUCCESS) return g_hud = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_hud,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = t->format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &g_hud_view) != VK_SUCCESS) {
        vk_defer_image(g_hud, g_hud_mem);
        return g_hud = VK_NULL_HANDLE, 0;
    }
    g_hud_w = t->width, g_hud_h = t->height, g_hud_fmt = t->format;
    g_hud_fresh = 1;
    rt_log("reshade: capa del HUD de %ux%u (formato %d)", t->width, t->height, (int)t->format);
    return 1;
}

void vk_reshade_present(VkImage image, VkFormat format, int w, int h) {
    (void)format, (void)w, (void)h;
    g_frame++;
    g_win = image;
    g_win_passes = 0;
    g_hud_started = 0;
    g_redir = 0;
    int on = cfg_bool("AC_RESHADE") && vk_reshade_contract_addon_present();
    if (on != g_on) {
        g_on = on;
        rt_log("reshade: la separación del HUD queda %s", on ? "activada" : "desactivada");
    }
    // El contrato se publica siempre: con la capa válida si la opción está activa, y sin capa si no.
    vk_reshade_contract_publish(g_on && g_hud ? (uint64_t)(uintptr_t)g_hud : 0, g_hud_w, g_hud_h,
                                (uint32_t)g_hud_fmt, g_frame);
}

int vk_reshade_pass_begin(VkExec *ex) {
    g_redir = 0;
    if (!g_on || !g_win) return 0;
    if (ex->pass_ncolor != 1) return 0;             // solo pases de un único objetivo de color
    const VkExecTarget *t = &ex->color[0];
    if (!t->image || t->image != g_win) return 0;   // el pase no escribe la textura de ventana
    if (t->samples > 1) return 0;                   // con multisample no se redirige (no debería pasar)
    g_win_passes++;
    if (g_win_passes == 1) return 0;                // el primer pase de ventana del frame escribe la escena
    if (!hud_ensure(t)) return 0;
    // La capa queda lista antes de que el pase nazca. En su primer uso del frame se limpia a transparente
    // (el HUD se compone por mezcla alfa sobre la escena; lo que no dibuja debe dejar ver la escena), y en
    // los demás basta ordenar las escrituras de un pase a otro.
    if (!g_hud_started) {
        hud_barrier(ex->cb, g_hud_fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        g_hud_fresh = 0;
        VkClearColorValue negro = {.float32 = {0, 0, 0, 0}};
        VkImageSubresourceRange todo = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(ex->cb, g_hud, VK_IMAGE_LAYOUT_GENERAL, &negro, 1, &todo);
        hud_barrier(ex->cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        g_hud_started = 1;
        ex->recorded += 3;   // las dos barreras y la limpieza quedan contadas como el resto de comandos
    } else {
        hud_barrier(ex->cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        ex->recorded += 1;
    }
    g_redir = 1;
    return 1;
}

int vk_reshade_redirected(void) { return g_redir; }

VkImageView vk_reshade_overlay_view(void) { return g_hud_view; }

void vk_reshade_pass_end(VkExec *ex) {
    if (!g_redir) return;
    g_redir = 0;
    // Lo escrito en la capa queda visible para quien la lea después (el addon, en el present).
    hud_barrier(ex->cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    ex->recorded += 1;
}

void vk_reshade_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !c->device) return;
    if (g_hud_view) vkDestroyImageView(c->device, g_hud_view, NULL);
    if (g_hud) vmaDestroyImage(c->vma, g_hud, g_hud_mem);
    g_hud = VK_NULL_HANDLE, g_hud_view = VK_NULL_HANDLE, g_hud_mem = NULL;
}
