# Astral Chain Recomp — fork de Gothan15

Fork de [astral-chain-pc-recomp](https://github.com/captainj-sparrow/astral-chain-pc-recomp), la recompilación
estática de *Astral Chain* (Nintendo Switch, 2019) a una aplicación nativa x86-64 para Windows, construida sobre la
pipeline de [TotK Recomp](https://github.com/valentderah/totk-pc-recomp)
([docs/architecture.md](docs/architecture.md)).

Sobre la v1.19.3 del proyecto base, este fork añade: **ReShade aplicado solo a la escena, antes del HUD**, un paquete
de mejoras para jugar con **teclado y ratón**, y **texturas BC7 con caché precómputada**. Las versiones publicadas
están en [Releases](../../releases): `reshade-1.19.3`, `reshade-fase1` y `reshade-fase2`.

> [!IMPORTANT]
> **No se incluye ningún archivo del juego.** Necesitas tu propia copia de *Astral Chain*, obtenida legalmente y
> volcada desde tu propia consola: el juego base (versión 1.0.0) y la actualización 1.0.1. No se proporcionan
> instrucciones para obtener esos archivos.
>
> El juego se construye desde tus archivos en tu máquina, así que el resultado contiene el juego. Es para tu uso
> propio. No lo compartas.

## Lo que añade este fork

### ReShade antes del HUD

- Addon `reshade_hud.addon64` y técnica `AstralChainHudComposite` (`AstralChainHud.fx`): los efectos de ReShade se
  aplican solo a la escena 3D y el HUD del juego se compone después, limpio, encima — al revés que aplicar ReShade
  sobre el fotograma completo.
- Se activa con `reshade = 1` en `astralchain.ini`, con ReShade con soporte completo de addons instalado; la técnica
  `AstralChainHudComposite` debe ir la última en la lista de técnicas.

### Fase 1: teclado y ratón

- Arreglado el bug que impedía restaurar una tecla a su valor de fábrica.
- Esquema de teclado y ratón de Puddz como predeterminado: WASD, cámara en F/T/G/H, acciones en J/K/I/U, ataque con
  clic izquierdo, Legión con clic derecho, y el resto de acciones repartidas por el teclado.
- Iconos de teclado y ratón dentro del juego (estilo «Keyboard» en los prompts), pintados con los binds reales del
  jugador en vez de con un esquema fijo.
- La rueda del ratón es bindeable (por defecto cambia de arma) y Esc es una tecla normal y bindeable: el overlay del
  port se abre solo con F1.
- Cámara con ratón con suelo de inclinación del 20 % y retención de velocidad de 25 ms.
- Descripciones de botones corregidas para Astral Chain en los cuatro idiomas completos del menú.
- Sliders de sombras de contacto (0–100) y de PCSS (0–200) en la pestaña de imagen.
- Interruptores de motion blur, profundidad de campo y aberración cromática presentes en el menú pero aún sin
  asignar: se activarán tras identificar los pases de postprocesado con una captura del juego.

### Fase 2: BC7 y caché de texturas

- `astc_bc = bc7` en `astralchain.ini`: todas las texturas ASTC se codifican a BC7 en vez de a BC1/BC3 — la misma
  memoria de vídeo que BC3 (una cuarta parte de la textura original), con más precisión de color y alfa real. En
  `auto` se mantiene el criterio del proyecto base.
- Caché BC7 precómputada: con `bc7_cache = 1` (por defecto), las texturas que estén en `cache/bc7` junto al exe se
  sirven ya codificadas y se saltan la codificación en vivo, que es la parte lenta del BC7; las que no estén se
  codifican al momento como siempre.
- Generador de la caché en `tools/bc7_precompute.py`, que conduce dos modos del propio exe: `--bc7-keys` para
  listar y validar las claves de las texturas del romfs, y `--bc7-convert` para codificarlas en la GPU y escribir
  la caché. Con la variable `AC_BC7_CACHE_LOG=1`, el juego anota la clave de cada textura BC7 que sube, para
  cruzarla con la lista del generador antes de codificar nada.

## Estado

El juego se puede jugar de la pantalla de título a los créditos. El port no tiene problemas de jugabilidad
conocidos, con los tiempos correctos a 60 FPS (esquiva perfecta, ataque de asalto, giro de 360°, etc.) y sin
problemas visuales conocidos. Se disfruta por su experiencia sin tirones, sus visuales, la ausencia de
desincronización de audio (desde la actualización 1.13.0 del port) y su buen rendimiento. El objetivo del proyecto
es dar a conocer esta joya oculta de PlatinumGames.

Los problemas se reportan como issues, siguiendo [CONTRIBUTING.md](CONTRIBUTING.md).

## Características del proyecto base

- Código nativo, no emulación. Funciona ya en sistemas muy antiguos.
- Probado externamente (Reddit, YouTube, ...): jugable de la intro a los créditos.
- [60 FPS planos en Steam Deck](https://steamdeckhq.com/news/astral-chains-pc-port-60-fps-on-steam-deck/)
- Salida HDR (HDR10), con brillo separado para el juego y para el HUD
- Escalado temporal y antialiasing para la escena 3D hasta 4K, desde nativo (DLAA / Native AA) hasta Ultra
  Performance: NVIDIA DLSS en tarjetas RTX (con `nvngx_dlss.dll` junto a `astralchain.exe`), Intel XeSS en la
  mayoría de GPU recientes, AMD FSR 3 en cualquier GPU
- Filtrado anisotrópico de texturas hasta 16x, para suelos nítidos a lo lejos
- Modo de baja latencia: NVIDIA Reflex o AMD Anti-Lag 2 donde el driver los ofrece, y la temporización propia del
  port en cualquier otra GPU (Intel, Steam Deck); alrededor de un tercio menos de tiempo del mando a la pantalla a
  60 fps, la mitad a 30
- Menú de ajustes en el juego (F1) y overlay de rendimiento (F10)
- Modo sin HUD para capturas y vídeos: F12 (o la pestaña Juego del menú) oculta indicadores, mapa, prompts y
  marcadores durante el juego y los vuelve a mostrar; los menús se quedan
- Setup y menú del juego en inglés, ruso, portugués de Brasil y chino simplificado
- Teclado, ratón y mando, con remapeo, vibración y giroscopio
- Prompts de botones de Xbox y PlayStation, cada uno con su disposición de botones (mod de PlayStation de
  https://gamebanana.com/mods/604666)
- Shaders preparados en la instalación en vez de durante el juego
- Ultrawide: de 16:10 a 48:9
- Oclusión ambiental, y en su nivel máximo luz rebotada: las superficies iluminadas aclaran y tiñen lo que tienen
  enfrente

## Instalación

Descarga `Astral Chain-Setup-<versión>-windows.zip` de [Releases](../../releases), descomprímelo donde quieras y
arranca `Astral Chain-Setup.exe`. Te pedirá:

- tu propia copia del juego base (versión 1.0.0, XCI o NSP) y de la actualización 1.0.1 (NSP);
- el idioma del juego y la carpeta de instalación.

Después extrae los datos del juego, le fusiona la actualización, levanta el código del juego a C y construye
`astralchain.exe` en tu máquina: alrededor de un minuto en una CPU rápida, unos minutos en una lenta, unos 14 GB
para la carpeta instalada y unos 11 GB de espacio temporal mientras se fusiona la actualización. El Setup no
lleva datos del juego ni código derivado de él.

Necesitas Windows 10 u 11 en x86-64 (SSE4.2 como mínimo) y una tarjeta gráfica Vulkan 1.3. El exe no está firmado,
así que SmartScreen avisa en el primer arranque.

Para el ReShade de este fork: coloca `reshade_hud.addon64` junto a `astralchain.exe` y `AstralChainHud.fx` en la
carpeta de shaders de ReShade, pon `reshade = 1` en `astralchain.ini` y deja la técnica `AstralChainHudComposite`
la última de la lista.

## Créditos

- [captainj-sparrow](https://github.com/captainj-sparrow) — autor del port base,
  [astral-chain-pc-recomp](https://github.com/captainj-sparrow/astral-chain-pc-recomp), sobre el que se construye
  todo lo demás de este fork.
- [@capibawa](https://github.com/capibawa) — agradecimiento especial del proyecto base por sus múltiples
  contribuciones: rendimiento, revisión de código, corrección de tiempos, visuales, HDR y escalado, entre otras.
- [Puddz](https://github.com/Puddz) — autor de [AstralChain-PC](https://github.com/Puddz/AstralChain-PC). De su
  código (licencia MIT) se adaptaron los iconos de teclado y ratón dentro del juego; su esquema de binds es el
  predeterminado de la fase 1, y el diseño del BC7 y de su caché precómputada sigue el suyo.
- [crosire](https://github.com/crosire) y el equipo de [ReShade](https://reshade.me) — ReShade y su API de
  addons, sobre la que se construye la opción de aplicar los efectos antes del HUD.
- [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) — la pipeline de la que parte el proyecto base:
  el lifter, el runtime, la traducción gráfica, el instalador y la mayor parte de la documentación inicial. Lo que
  se tomó de cada proyecto, con su licencia, está en [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):
  [LAS Recomp](https://github.com/peiikai/LASRecomp),
  [MacNativeGames](https://github.com/ly05010419/MacNativeGames),
  [nxnative](https://github.com/seedds/nxnative), [Ryujinx](https://github.com/Ryujinx/Ryujinx),
  [deko3d](https://github.com/devkitPro/deko3d), [SDL 3](https://libsdl.org),
  [Dear ImGui](https://github.com/ocornut/imgui),
  [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR),
  [AMD FSR 3](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK),
  [Intel XeSS](https://github.com/intel/xess),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
  [libopus](https://opus-codec.org), [Zstandard](https://github.com/facebook/zstd),
  [Switchbrew](https://switchbrew.org) y la comunidad homebrew de Switch.
- PlatinumGames y Nintendo — autores del *Astral Chain* original.
- [Gothan15](https://github.com/Gothan15) — este fork: la opción de ReShade antes del HUD, la fase 1 de teclado y
  ratón y la fase 2 de BC7 y caché.

## Licencia

El código propio del proyecto está bajo la [licencia MIT](LICENSE); el código de terceros conserva su propia
licencia ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). La licencia no concede nada sobre el juego de
Nintendo.
