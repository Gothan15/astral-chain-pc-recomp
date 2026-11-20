// La opción «reshade» (astralchain.ini: reshade = 1, AC_RESHADE): los efectos de ReShade se aplican solo a
// la escena y el HUD del juego queda intacto. El cómo: los pases del juego que escriben la textura de
// ventana después de que la escena quedó escrita (la fase del HUD y los menús) se redirigen a una imagen
// propia (la capa del HUD); el present presenta la textura de ventana con la escena sola, ReShade aplica
// su preset sobre ella, y el addon compañero (addon/) compone la capa encima antes de que el frame salga
// a pantalla. Sin el addon cargado la opción no hace nada: los frames van normales.
#ifndef VK_RESHADE_H
#define VK_RESHADE_H

#include "vk_exec.h"   // VkExec, VkExecTarget (el mismo patrón que vk_dlaa.h)

// vk_backend.c, en cada present del juego: publica cuál es la textura de ventana y cierra el frame
// anterior (reinicia la cuenta de pases de ventana y publica el contrato para el addon).
void vk_reshade_present(VkImage image, VkFormat format, int w, int h);

// vk_exec.c, en vk_exec_pass_begin tras resolver los objetivos: decide si el pase que nace es de la fase
// HUD y, si lo es, prepara la capa (la crea, la limpia en su primer uso del frame y deja sus barreras).
// Devuelve 1 si el pase quedó redirigido.
int vk_reshade_pass_begin(VkExec *ex);

// La consulta del punto donde vk_exec_pass_begin monta los adjuntos: si el pase abierto está redirigido,
// el adjunto de color 0 usa esta vista y carga sin limpiar (la capa ya se limpió en el hook).
int vk_reshade_redirected(void);
VkImageView vk_reshade_overlay_view(void);

// vk_exec.c, en vk_exec_pass_end_why tras vkCmdEndRendering: la barrera de salida de la capa, para que
// quien la lea después (el addon) vea lo que el pase escribió.
void vk_reshade_pass_end(VkExec *ex);

// vk_device.c, con el dispositivo ya en reposo: destruye la capa.
void vk_reshade_release(void);

// El transporte del contrato (vk_reshade_win.c; fuera de Windows no hay contrato y todo queda inerte).
void vk_reshade_contract_publish(uint64_t hud_image, uint32_t w, uint32_t h, uint32_t format, uint64_t frame);
int vk_reshade_contract_addon_present(void);

#endif
