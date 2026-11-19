// El contrato entre el runtime y el addon compañero de ReShade (addon/reshade_hud.cpp): una estructura
// pequeña en memoria compartida con nombre. El fork publica en cada present la imagen de la capa del HUD;
// el addon la lee tras aplicar los efectos y compone la capa sobre la imagen final. Este encabezado es la
// única definición de la estructura: lo incluyen el runtime (C) y el addon (C++), y no debe divergir.
#ifndef VK_RESHADE_CONTRACT_H
#define VK_RESHADE_CONTRACT_H

#include <stdint.h>

// El nombre del bloque de memoria compartida (en Windows, CreateFileMappingA/OpenFileMappingA).
#define AC_RESHADE_CONTRACT_NAME "Local\\AstralChainReShadeHud"

// Las marcas de presencia: cada lado escribe la suya al llegar, para que el otro sepa que está.
#define AC_RESHADE_FORK_MAGIC 0x41435253u    // "ACRS": el fork creó el bloque
#define AC_RESHADE_ADDON_MAGIC 0x41435241u   // "ACRA": el addon está cargado y compone

typedef struct {
    uint32_t fork_magic;    // la escribe el fork al crear el bloque
    uint32_t addon_magic;   // la escribe el addon al cargarse; sin ella el fork no redirige nada
    uint64_t hud_image;     // el VkImage nativo de la capa del HUD; 0 si este frame no hay capa
    uint32_t width;         // el tamaño de la capa
    uint32_t height;
    uint32_t format;        // su VkFormat
    uint32_t reserved;
    uint64_t frame;         // el contador de presents publicados por el fork
    uint64_t addon_frame;   // el último frame que el addon compuso (diagnóstico)
} AcReshadeContract;

#endif
