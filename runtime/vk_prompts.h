// Iconos de teclado y ratón para los prompts del juego (AC_PROMPTS = keyboard, o auto sin mando).
//
// El juego dibuja todos sus prompts (en textos, en el HUD y en los menús) desde un único atlas de
// 256x512 RGBA8 en ui_core (core.wtp, textura 0): formas blancas sobre transparencia con la letra
// recortada, que el juego tiñe. Cuando esa textura se sube a la GPU, este módulo repinta su nivel 0
// ya deswizzleado (en el búfer de staging de vk_texture.c, antes de la copia) con la tecla o el botón
// de ratón que cada acción tiene asignado, leído del snapshot de input_map.c, en el mismo estilo y
// dentro de la misma huella del icono original.
//
// Los estilos de mando (Xbox, PlayStation, Nintendo) NO pasan por aquí: siguen en los overlays romfs
// de Sparrow. Si un overlay está activo, el atlas llega ya sustituido, el hash no casa y no se
// repinta nada: el módulo se autoprotege.
//
// Adaptado de button_prompts.c de Puddz (AstralChain-PC, MIT): la tabla de huecos, el lienzo de
// cobertura con supersampling, el ratón dibujado y el recorte de texto con GDI son suyos; la
// identificación se hace sobre los bytes crudos del almacenamiento invitado y las asignaciones se
// leen de input_map.c en vez de su input_bindings.h.
#pragma once
#include <stdint.h>

// 1 cuando el almacenamiento (mem, size) con ese formato y tamaño es el atlas original de iconos.
// El hash se calcula sobre los bytes CRUDOS del almacenamiento (como hace Puddz), nunca sobre los
// píxeles ya deswizzleados.
int vk_prompts_identify(const void *mem, uint64_t size, uint32_t format, uint32_t w, uint32_t h);

// Si la textura es el atlas y el estilo en efecto es el de teclado, repinta su nivel 0 en dst
// (píxeles RGBA8 deswizzleados, `row` bytes por fila). Devuelve 1 si pintó.
int vk_prompts_paint(uint8_t *dst, uint32_t row, const void *mem, uint64_t size, uint32_t format, uint32_t w,
                     uint32_t h);

// Generación de los prompts: cambia con el estilo en efecto y con cualquier cambio de los binds.
// Es el equivalente a texpatch_generation de Puddz: vk_texture.c la usa para resubir el atlas.
uint32_t vk_prompts_generation(void);

// vk_texture.c: tras pintar el atlas de la textura con dirección invitada addr en una subida.
void vk_prompts_note_painted(uint64_t addr);
// 1 cuando addr es el atlas ya pintado y la generación actual difiere de la pintada: hay que
// resubirlo aunque su almacenamiento no haya cambiado (vk_texture.c, make_fresh/make_fresh_snap).
int vk_prompts_stale(uint64_t addr);
