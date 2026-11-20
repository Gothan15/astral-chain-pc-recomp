# Plan: cámara directa (1:1) para el fork de Astral Chain

Rama: `camara-directa` (creada desde main el 2026-11-20). Release aparte cuando haya algo que probar.

## Objetivo

Que el ratón gire la cámara del juego escribiendo su yaw y su pitch, como en un juego de PC nativo: sin zona muerta, sin curva de aceleración y sin el tope de velocidad de giro del stick. Hoy los dos modos del fork (sparrow y puddz) emulan el stick derecho, así que heredan las tres cosas y un flick rápido se siente «de mando».

## La referencia: el Bloodborne de Supermedo

- Su modo `mouse_camera = direct` se engancha a la actualización de cámara del juego y le suma el movimiento del ratón ahí dentro. Sensibilidad en conteos por radián (1.0 = 900 conteos/rad).
- Con fijación de objetivo activa vuelve al camino del stick, porque el flick para cambiar de objetivo depende de él.
- Venía de un mod de Nexus (imedved): la ingeniería inversa de dónde vive la cámara de Bloodborne ya estaba hecha. Esa es la parte que a nosotros nos toca hacer desde cero.

## Nuestro punto de partida (recon del 2026-11-20, verificado en el código)

1. **El camino actual del stick:** `input.c` produce la deflexión del ratón → `ov_input.c` la sirve al juego como estado Npad (las funciones `nn::hid` que el juego enlaza) → el componente PlayerCamera del juego la integra con sus parámetros.
2. **Los parámetros de la cámara son datos:** `PlayerCameraParam.bgyml` (dentro de `Pack/Actor/PlayerCamera.pack.zs`) trae las tasas de giro `StickSensitivity {0.8, 1.2, 1.6, 2.0, 2.4}`, y el runtime ya parchea ese archivo para `AC_STICK_SENS` (`camera_speed.c`). Ahí pueden vivir también la zona muerta y la curva; está por inspeccionar.
3. **La infraestructura de escritura ya existe:** la máquina virtual de cheats (`cheats.c`) corre cada frame presentado y lee y escribe la memoria del huésped en sus propias direcciones; la memoria está mapeada y `mem_query` dice qué regiones son legibles y escribibles.
4. **Lo que no tenemos:** la dirección y la estructura del estado de la cámara (el yaw/pitch actuales) en la memoria del huésped. Nadie lo ha publicado para Astral Chain; Sparrow y Puddz se quedaron en el stick.

## Arquitectura propuesta

- La clave existente crece: `MOUSE_CAMERA = direct`, tercer valor junto a `sparrow|puddz`, en vivo. En ese modo, `MOUSE_SENS` se interpreta en conteos por radián.
- Un módulo nuevo del runtime que cada frame, en el mismo punto donde corre la VM de cheats (`cheats_frame`): acumula el movimiento del ratón que recoge `input.c` y, si el estado de la cámara está localizado y validado, escribe `yaw += dx · escala`, `pitch += dy · escala` con el pitch limitado, directamente en la estructura del huésped.
- Mientras el ratón manda, el stick derecho que se sirve al juego va centrado, para que la integración del juego no sume la suya; **excepto con fijación activa**, donde se vuelve al camino del stick como en Bloodborne. Detectar la fijación es otro dato del huésped por localizar.
- Fallo seguro: si la estructura no se localiza o deja de validar (valores imposibles), el modo cae a `sparrow`, lo dice en el log y no rompe nada.

## Fases

0. **Inspección de datos** (sin tocar código del juego): extraer `PlayerCameraParam.bgyml` del pack y catalogar todos los parámetros de cámara (zona muerta, curva, suavizado, topes). Si la curva y la zona muerta son datos, aplanarlos es una vía parcial rápida y, sobre todo, acota cuánto del «sabor a stick» viene de datos y cuánto de código.
1. **Localizar el estado de la cámara** (el grueso de la ingeniería inversa): herramienta de escaneo en el runtime, solo para depurar: volcados de las regiones legibles del huésped en reposo y tras giros conocidos, correlacionados con la matriz de vista que la capa Vulkan ya ve cada frame (los bloques de cámara se suben desde el huésped: su contenido da el yaw/pitch exactos por frame para validar candidatos). Criterio de éxito: dos flotantes que sigan el yaw/pitch con la relación exacta en varias escenas.
2. **Escritura mínima:** módulo y clave; escribir solo el yaw en una escena segura y comprobar que el juego no lo pisa al instante. Si su actualización sobrescribe, se mueve el punto de enganche dentro del frame, no el concepto.
3. **Integración completa:** pitch con su límite, sensibilidad en conteos por radián, fijación → stick, no aplicar en menús ni cinemáticas, y convivencia con la auto-rotación del juego.
4. **Prueba en Windows y release aparte** desde la rama, con etiqueta propia.

## Qué se necesita

- De Gothan: probar builds en su PC. En la fase 1, una sesión con él girando la cámara a petición en una escena tranquila mientras el escáner vuelca memoria.
- Nada del código original del juego: todo se hace desde el runtime y la memoria del huésped en tiempo real, como los cheats.
- Tiempo: la fase 1 es la incierta; puede salir en una sesión de escaneo o llevar varias. Las demás fases son mecánicas una vez localizado el estado.

## Riesgos

- Que el estado no sean dos flotantes simples (un cuaternión, o algo derivado de la matriz): el escaneo correlaciona también contra la matriz de vista.
- Que la actualización de cámara del juego pise lo escrito cada frame: se resuelve moviendo el punto de enganche.
- Que la fijación no se pueda detectar barato: la v1 iría directa siempre que el ratón se mueva, y se evalúa jugando.
