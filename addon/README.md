# Addon «HUD tras ReShade» — compañero de la opción `reshade`

Con la opción `reshade = 1` en `astralchain.ini`, el juego dibuja su HUD y sus menús en una imagen
aparte (la capa) y presenta la escena sola: el preset de ReShade se aplica solo a la escena. Este addon
es la otra mitad: enlaza esa capa a la técnica de `AstralChainHud.fx`, que la compone sobre la imagen
final tras todos los efectos, con el HUD intacto.

## Requisitos

- Una build de ReShade **con soporte completo de addons** (la oficial «with full add-on support»);
  la build básica no carga addons `.addon64`.
- El fork compilado con la opción (la rama `reshade-antes-del-hud`).

## Compilar el addon

La vía automática: el workflow **Addon** de este repo (`.github/workflows/addon.yml`) lo compila con
MSVC en cada push a `addon/**` de esta rama y deja el `reshade_hud.addon64` como artefacto
`reshade_hud` de la corrida (pestaña Actions). También se puede lanzar a mano desde ahí.

A mano es un solo archivo (`reshade_hud.cpp`) contra los encabezados del SDK de ReShade, vendored en
`include/` (BSD-3/MIT, (C) Patrick Mours). Con MSVC, desde una consola de desarrollador:

```
cl /std:c++17 /EHsc /LD /I include reshade_hud.cpp /Fe:reshade_hud.addon64
```

Con el clang de llvm-mingw (el mismo de la build del juego):

```
clang++ -std=c++17 -shared -I include reshade_hud.cpp -o reshade_hud.addon64
```

## Instalar y usar

1. Copia `reshade_hud.addon64` junto a `astralchain.exe` (o a la carpeta de addons que uses).
2. Copia `AstralChainHud.fx` junto a tus efectos (la carpeta de shaders de ReShade). No lo renombres:
   el addon busca el efecto y la técnica por nombre.
3. Pon `reshade = 1` en `astralchain.ini`.
4. En el overlay de ReShade, activa la técnica **Astral Chain — HUD composite** en tu preset y déjala
   **la última de la lista**: todo lo que vaya después de ella sí tocaría el HUD.

## Cómo se protege solo

- Sin el addon cargado (o sin la técnica activada), el fork no redirige nada: el juego va normal.
- Si el addon desaparece a mitad de partida, el fork deja de redirigir en unos frames.
- Si la opción está a 0, la técnica no pinta nada (el addon la mantiene desactivada por dentro).

## Qué comprobar en la primera prueba

- Que el HUD se ve y el preset no lo afecta (p. ej. un preset con saturación fuerte: la escena cambia,
  el HUD no).
- Que al cambiar la resolución en el juego el HUD sigue alineado (la capa se recrea; un frame puede
  verse con el tamaño anterior).
- El log del juego anota `reshade: capa del HUD de …` al crear la capa y
  `reshade: la separación del HUD queda activada` cuando el addon y el fork se dan la mano.
