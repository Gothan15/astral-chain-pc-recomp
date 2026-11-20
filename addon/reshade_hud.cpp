// El addon compañero de la opción «reshade» del fork (runtime/vk_reshade.c): compone el HUD del juego
// sobre la escena ya pasada por los efectos de ReShade, para que el preset no toque la interfaz.
//
// El trabajo se reparte así: el fork dibuja la fase HUD en una imagen suya (la capa) y publica su handle
// en un bloque de memoria compartida (runtime/vk_reshade_contract.h); la composición la hace una técnica
// del efecto AstralChainHud.fx, que va la última del preset y muestrea la textura de la semántica
// HUDLAYER. Este addon solo hace de puente: da señales de vida al fork, comprueba que la técnica está
// cargada y activada, y enlaza la vista de la capa a esa semántica (o una textura transparente de
// reserva cuando el frame no trae capa). Todo se hace en reshade_present, tras los efectos: los enlaces
// valen para el frame siguiente, y como la capa solo cambia de handle al redimensionar, el estado es
// estable frame a frame.
//
// Requiere una build de ReShade con soporte completo de addons (la oficial «full add-on support»).
#include <reshade.hpp>
#include <windows.h>

#include <cstdint>

#include "../runtime/vk_reshade_contract.h"

using namespace reshade;

namespace {

HANDLE g_map = nullptr;             // el bloque de memoria compartida con el fork
AcReshadeContract *g_sh = nullptr;  // su vista mapeada

api::device *g_device = nullptr;          // el dispositivo del que cuelgan las vistas creadas
api::resource_view g_layer_srv = { 0 };   // la vista de la capa del fork (caché por handle)
uint64_t g_layer_image = 0;               // el handle de la capa al que corresponde esa vista
api::resource_view g_bound_srv = { 0 };   // la vista enlazada ahora mismo a la semántica
api::resource g_blank = { 0 };            // la textura 1x1 transparente de reserva
api::resource_view g_blank_srv = { 0 };   // su vista

// El bloque del contrato, abierto a la primera que se puede: el fork lo crea en su primer present, así
// que al principio puede no existir todavía y hay que reintentar en frames siguientes.
AcReshadeContract *contract() {
    if (g_sh != nullptr) return g_sh;
    g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, AC_RESHADE_CONTRACT_NAME);
    if (g_map == nullptr) return nullptr;
    g_sh = static_cast<AcReshadeContract *>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AcReshadeContract)));
    return g_sh;
}

// Suelta todo lo creado sobre el dispositivo (al destruirse este o al descargarse el addon).
void release_views() {
    if (g_device == nullptr) return;
    if (g_layer_srv.handle != 0) g_device->destroy_resource_view(g_layer_srv);
    if (g_blank_srv.handle != 0) g_device->destroy_resource_view(g_blank_srv);
    if (g_blank.handle != 0) g_device->destroy_resource(g_blank);
    g_layer_srv = { 0 };
    g_bound_srv = { 0 };
    g_blank_srv = { 0 };
    g_blank = { 0 };
    g_layer_image = 0;
    g_device = nullptr;
}

// La textura de reserva: un píxel transparente que la técnica muestrea cuando no hay capa, para que
// componer no pinte nada.
bool ensure_blank(api::device *device) {
    if (g_blank.handle != 0) return true;
    const uint32_t cero = 0;
    api::subresource_data data = {};
    data.data = const_cast<uint32_t *>(&cero);
    data.row_pitch = 4;
    data.slice_pitch = 4;
    if (!device->create_resource(api::resource_desc(1, 1, 1, 1, api::format::r8g8b8a8_unorm, 1,
                                                    api::memory_heap::gpu_only, api::resource_usage::shader_resource),
                                 &data, api::resource_usage::shader_resource, &g_blank))
        return false;
    return device->create_resource_view(g_blank, api::resource_usage::shader_resource,
                                        api::resource_view_desc(api::format::r8g8b8a8_unorm), &g_blank_srv);
}

void on_reshade_present(api::effect_runtime *runtime) {
    AcReshadeContract *sh = contract();
    if (sh == nullptr) return;
    api::device *device = runtime->get_device();
    if (device != g_device) {
        release_views();
        g_device = device;
    }

    // La técnica compañera tiene que estar cargada y activada en el preset; si no, el addon no escribe
    // su marca y el fork deja de redirigir pases en unos frames (el HUD vuelve al camino normal).
    api::effect_technique tech = runtime->find_technique("AstralChainHud.fx", "AstralChainHudComposite");
    const bool ready = tech.handle != 0 && runtime->get_technique_state(tech);
    api::effect_uniform_variable active = runtime->find_uniform_variable("AstralChainHud.fx", "HudLayerActive");
    if (!ready) {
        sh->addon_magic = 0;
        if (active.handle != 0) runtime->set_uniform_value_bool(active, false);
        return;
    }
    sh->addon_magic = AC_RESHADE_ADDON_MAGIC;
    sh->addon_frame = sh->frame;   // la señal de vida que el fork vigila
    if (active.handle != 0) runtime->set_uniform_value_bool(active, true);

    // Sin capa este frame: se enlaza la textura de reserva y no hay más que hacer.
    // Si ni eso se puede, la marca se retira: el fork deja de redirigir y el HUD
    // vuelve al camino normal en vez de desaparecer (degradar, no romper).
    if (sh->hud_image == 0) {
        if (!ensure_blank(device)) { sh->addon_magic = 0; return; }
        if (g_bound_srv.handle != g_blank_srv.handle) {
            runtime->update_texture_bindings("HUDLAYER", g_blank_srv, g_blank_srv);
            g_bound_srv = g_blank_srv;
        }
        return;
    }

    // Con capa nueva (o la primera): se crea su vista y se enlaza antes de soltar la anterior. En Vulkan
    // el handle de recurso de la API es el handle nativo, y la capa vive en layout GENERAL, válido para
    // muestrearla tal cual.
    if (sh->hud_image != g_layer_image) {
        api::format fmt = api::format::r8g8b8a8_unorm;                    // VK_FORMAT_R8G8B8A8_UNORM (37)
        if (sh->format == 44) fmt = api::format::b8g8r8a8_unorm;          // VK_FORMAT_B8G8R8A8_UNORM (44)
        api::resource layer = { sh->hud_image };
        api::resource_view srv = { 0 };
        if (!device->create_resource_view(layer, api::resource_usage::shader_resource,
                                          api::resource_view_desc(fmt), &srv)) {
            // Sin vista no hay composición posible (p. ej. un formato de ventana
            // que no sea 37 ni 44): se retira la marca para que el fork deje de
            // redirigir y el HUD se quede en el camino normal.
            sh->addon_magic = 0;
            return;
        }
        runtime->update_texture_bindings("HUDLAYER", srv, srv);
        if (g_layer_srv.handle != 0) device->destroy_resource_view(g_layer_srv);
        g_layer_srv = srv;
        g_layer_image = sh->hud_image;
        g_bound_srv = srv;
    }
}

void on_destroy_device(api::device *device) {
    if (device == g_device) release_views();
}

} // namespace

extern "C" __declspec(dllexport) const char *NAME = "Astral Chain HUD composite";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Compone el HUD de Astral Chain tras los efectos de ReShade; compañero de la opción reshade del fork.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!register_addon(module)) return FALSE;
        register_event<addon_event::reshade_present>(on_reshade_present);
        register_event<addon_event::destroy_device>(on_destroy_device);
        break;
    case DLL_PROCESS_DETACH:
        unregister_event<addon_event::destroy_device>(on_destroy_device);
        unregister_event<addon_event::reshade_present>(on_reshade_present);
        unregister_addon(module);
        break;
    }
    return TRUE;
}
