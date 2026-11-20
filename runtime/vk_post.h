// Interruptores del post-procesado del propio juego (AC_MOTION_BLUR, AC_DEPTH_OF_FIELD,
// AC_CHROMATIC_ABERRATION; 1 = como el juego, por defecto; claves en vivo).
//
// Tras el composite de iluminación, el juego pasa el color de escena por su cadena de post
// (vk_contact.c describe la tubería: G-buffer, iluminación a media resolución, composite, y
// después transparentes, cielo y post). Cada efecto de esta cadena es un filtro: lee el color de
// escena y escribe el color de escena. Si un pase así no dibuja, la imagen pasa intacta y el
// efecto desaparece sin tocar nada más: ese es el apagado.
//
// Qué pase es cada efecto NO lo dice el código del port: lo dice una captura AC_DUMP (passes.txt
// lista targets, pipelines y texturas de cada pase). La tabla de firmas de vk_post.c se rellena
// con esa captura; mientras una firma esté sin rellenar, su interruptor no salta ningún pase.
// AC_POST_DEBUG=1 registra la firma de cada pase posterior al composite, una vez por firma
// distinta, para poder hacer la asignación sin adivinar.
#pragma once
struct VkExec;
void vk_post_pass_begin(struct VkExec *ex);   // un pase va a empezar (targets resueltos)
void vk_post_draw(struct VkExec *ex);         // un draw va a grabarse en el pase abierto
int vk_post_skip_draw(void);                  // 1: el pase abierto es de un efecto apagado
