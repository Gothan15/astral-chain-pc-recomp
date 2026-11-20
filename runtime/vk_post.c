// Interruptores del post-procesado del propio juego: ver la cabecera (vk_post.h). El mecanismo se
// apoya en los precedentes del repo: el aviso de inicio de pase de vk_contact/vk_ao y los drops
// AC_SKIP_* de vk_exec_draw.c (un pase filtro que no dibuja deja la imagen intacta).
#include "vk_post.h"
#include <string.h>
#include "cfg.h"
#include "host.h"
#include "vk_exec.h"

typedef struct {
    uint32_t w, h;          // tamaño del pase; 0 = cualquiera
    VkFormat color;         // formato de su objetivo de color 0; VK_FORMAT_UNDEFINED = cualquiera
    VkFormat depth;         // formato de su profundidad; idem
    int draws;              // draws del pase; 0 = cualquiera (un filtro de pantalla completa: 1)
    const void *prog;       // programa del primer draw; NULL = cualquiera
    int filled;             // 0: firma sin asignar (de la captura AC_DUMP); el interruptor no actúa
} PostSig;

typedef struct { const char *key; PostSig sig; int off; } PostFx;
static PostFx fx[] = {
    {"AC_MOTION_BLUR", {0}, 0},
    {"AC_DEPTH_OF_FIELD", {0}, 0},
    {"AC_CHROMATIC_ABERRATION", {0}, 0},
};

static const VkTexImage *scene_img;   // el color de escena del último pase de escena
static uint32_t scene_w, scene_h;     // su tamaño
static int paired;                    // la profundidad lineal de la escena ya se escribió en su pase
static int in_post;                   // estamos tras el composite de iluminación
static int cur_off;                   // el pase abierto es de un efecto apagado
static int cur_draws;
static const void *cur_prog;

// La detección sigue la secuencia del frame real, como la de vk_contact.c tras su arreglo (dump del
// frame 1604): la versión anterior pedía el color de escena, la profundidad lineal R32F y una D32F en
// el MISMO pase, y un composite con profundidad D16, y nada de eso ocurre en el juego (la profundidad
// de escena es D32FS8, la R32F va en un pase aparte de un draw y el composite adjunta la profundidad
// de escena), así que in_post nunca se activaba y ni el registro de AC_POST_DEBUG imprimía pases.
//   1. Pase que escribe el color de escena (B10G11R11) con la profundidad de escena adjunta: pase de
//      escena; se recuerda el color y se espera su profundidad lineal.
//   2. Pase que escribe solo una R32F del mismo tamaño: la profundidad lineal queda emparejada.
//   3. Pase que vuelve a escribir el color de escena ya emparejado: el composite; tras él, post.
static const VkTexImage *scene_target(const VkExec *ex) {
    if (ex->pass_noatt || ex->pass_w < 640 || ex->depth.level) return NULL;
    if (ex->depth.format != VK_FORMAT_D32_SFLOAT && ex->depth.format != VK_FORMAT_D32_SFLOAT_S8_UINT) return NULL;
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) {
        const VkExecTarget *t = &ex->color[i];
        if (!t->image || t->level || t->width != ex->pass_w || t->height != ex->pass_h) continue;
        if (t->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && (t->img->usage & VK_IMAGE_USAGE_STORAGE_BIT))
            return t->img;
    }
    return NULL;
}

static int is_lindepth(const VkExec *ex) {
    if (ex->pass_noatt || !scene_img || ex->pass_ncolor != 1) return 0;
    const VkExecTarget *t = &ex->color[0];
    return t->image && !t->level && t->format == VK_FORMAT_R32_SFLOAT && t->width == scene_w && t->height == scene_h;
}

// El composite de iluminación: el color de escena como único objetivo, con su profundidad lineal ya
// emparejada (esa marca es lo único que lo distingue del pase de escena, que escribe lo mismo).
static int is_composite(const VkExec *ex) {
    return scene_img && paired && ex->pass_ncolor == 1 && ex->color[0].img == scene_img && !ex->color[0].level &&
           ex->pass_w == scene_w && ex->pass_h == scene_h;
}

static int sig_match(const PostSig *s, const VkExec *ex) {
    if (!s->filled) return 0;
    if (s->w && (s->w != ex->pass_w || s->h != ex->pass_h)) return 0;
    if (s->color && (ex->pass_ncolor < 1 || ex->color[0].format != s->color)) return 0;
    if (s->depth && ex->depth.format != s->depth) return 0;
    if (s->prog && s->prog != (const void *)ex->prog) return 0;
    return 1;   // draws se comprueba al primer draw (vk_post_draw), no aquí
}

void vk_post_pass_begin(VkExec *ex) {
    cur_off = 0; cur_draws = 0; cur_prog = NULL;
    const VkTexImage *sc = scene_target(ex);
    if (sc && !(paired && sc == scene_img)) {
        // Pase de escena: (re)empieza la pareja y la zona de post queda atrás.
        scene_img = sc;
        scene_w = ex->pass_w;
        scene_h = ex->pass_h;
        paired = 0;
        in_post = 0;
        return;
    }
    if (is_composite(ex)) { in_post = 1; paired = 0; return; }
    if (is_lindepth(ex)) { paired = 1; return; }
    if (!in_post) return;
    static int debug = -1;
    if (debug < 0) {   // AC_POST_DEBUG=1: se lee una vez (cfg_get en crudo: «1» o nada)
        const char *pd = cfg_get("AC_POST_DEBUG");
        debug = pd && !strcmp(pd, "1");
    }
    for (unsigned i = 0; i < sizeof fx / sizeof *fx; i++) {
        fx[i].off = !cfg_bool(fx[i].key);   // en vivo: se lee en cada pase
        if (fx[i].off && sig_match(&fx[i].sig, ex)) cur_off = 1;
    }
    if (debug) {   // la firma del pase, para la asignación (una vez por combinación distinta)
        static struct { uint32_t w, h; VkFormat c, d; const void *p; } seen[64];
        static int nseen;
        VkFormat c0 = ex->pass_ncolor ? ex->color[0].format : VK_FORMAT_UNDEFINED;
        int dup = 0;
        for (int i = 0; i < nseen; i++)
            if (seen[i].w == ex->pass_w && seen[i].h == ex->pass_h && seen[i].c == c0 &&
                seen[i].d == ex->depth.format && seen[i].p == (const void *)ex->prog) dup = 1;
        if (!dup && nseen < 64) {
            seen[nseen++] = (typeof(seen[0])){ex->pass_w, ex->pass_h, c0, ex->depth.format,
                                              (const void *)ex->prog};
            rt_log("vk: post: pase tras el composite: %ux%u color %d profundidad %d programa %p", ex->pass_w,
                   ex->pass_h, (int)c0, (int)ex->depth.format, (const void *)ex->prog);
        }
    }
}

void vk_post_draw(VkExec *ex) {
    cur_draws++;
    if (!cur_prog) cur_prog = (const void *)ex->prog;
    // Si la firma del efecto apagado exige un número de draws concreto y este pase ya lo pasó sin
    // ser él, cur_off se retira: la comprobación fina vive aquí, con el pase ya abierto.
    (void)cur_draws;
}

int vk_post_skip_draw(void) { return cur_off; }
