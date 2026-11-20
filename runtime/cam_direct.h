// Cámara directa (1:1) del ratón: la interfaz entre input.c (que recoge los conteos del ratón),
// boot.c (que llama una vez por frame presentado) y el módulo cam_direct.c (que los escribe en el
// estado de la cámara del juego). Ver el plan en docs/camara-directa-plan.md.
#ifndef CAM_DIRECT_H
#define CAM_DIRECT_H

#include <stdint.h>

// Suma conteos de ratón a lo pendiente de aplicar (hilo de entrada; la toma el frame).
void cam_direct_push(int32_t dx, int32_t dy);
// Descarta lo pendiente (al soltar la captura del cursor no debe quedar giro guardado).
void cam_direct_reset(void);
// 1 si el estado de la cámara está localizado y validado: input.c entonces deja el stick centrado
// y manda los conteos aquí; 0 = input.c sigue con la emulación del stick de siempre.
int cam_direct_active(void);
// Una vez por frame presentado (boot.c, rt_frame_presented): aplica lo pendiente y, si
// AC_CAM_SCAN=1, vuelca la memoria del huésped para la fase de localización.
void cam_direct_frame(uint64_t frame);

#endif
