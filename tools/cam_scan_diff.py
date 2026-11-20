# Compara los volcados del escáner de la cámara directa (AC_CAM_SCAN=1, runtime/cam_direct.c) y
# busca el estado de la cámara: los flotantes que están quietos en reposo, cambian al girar la
# cámara y vuelven a su valor al regresar. Es la fase 1 del plan de docs/camara-directa-plan.md.
#
# La sesión que este análisis espera (la que produce el escáner: un volcado cada 8 s):
#   volcados 0 y 1: cámara quieta (reposo)
#   volcado 2: tras girar la cámara a la derecha (yaw)
#   volcado 3: de vuelta a la vista original (reposo otra vez)
#   volcado 4: tras inclinar la cámara arriba o abajo (pitch)
#   volcado 5: de vuelta a la vista original
# Otro reparto se indica con --reposo 0,1,3,5 --yaw 2 --pitch 4.
#
# Uso: python cam_scan_diff.py <carpeta cam-scan>
# Salida: los candidatos con sus valores en cada volcado y, si queda un único par, el archivo
# cam-direct.txt listo para copiar a la carpeta del juego.
import argparse
import struct
import sys
from pathlib import Path

BLOQUE = 1 << 23   # 8 MiB: la comparación por bloques con XOR de enteros va en C y descarta
                   # de golpe todo lo que no cambió entre dos volcados


def cargar(carpeta, n):
    """Un volcado como lista de (dirección del huésped, bytes), según su manifiesto .txt."""
    txt = Path(carpeta) / f'dump-{n}.txt'
    binario = Path(carpeta) / f'dump-{n}.bin'
    if not txt.is_file() or not binario.is_file():
        sys.exit(f'falta el volcado {n} en {carpeta} (dump-{n}.txt / dump-{n}.bin)')
    datos = binario.read_bytes()
    trozos = []
    for linea in txt.read_text().splitlines():
        partes = linea.split()
        if len(partes) == 4 and partes[0] == 'chunk':
            base, size, offset = int(partes[1], 16), int(partes[2]), int(partes[3])
            trozos.append((base, datos[offset:offset + size]))
    return trozos


def mapa(trozos):
    return {base: datos for base, datos in trozos}


def difieren(a, b):
    """Los desplazamientos de flotante (de 4 en 4 bytes) en los que a y b difieren, por bloques."""
    n = min(len(a), len(b)) & ~3
    sitios = []
    for i in range(0, n, BLOQUE):
        trozo_a, trozo_b = a[i:i + BLOQUE], b[i:i + BLOQUE]
        if trozo_a == trozo_b:
            continue
        x = int.from_bytes(trozo_a, 'little') ^ int.from_bytes(trozo_b, 'little')
        xb = x.to_bytes(len(trozo_a), 'little')
        for j in range(0, len(xb) - 3, 4):
            if xb[j:j + 4] != b'\0\0\0\0':
                sitios.append(i + j)
    return sitios


def flotante(datos, offset):
    return struct.unpack_from('<f', datos, offset)[0]


def main():
    ap = argparse.ArgumentParser(description='Busca el estado de la cámara en los volcados del escáner.')
    ap.add_argument('carpeta', help='la carpeta cam-scan con dump-0..dump-5')
    ap.add_argument('--reposo', default='0,1,3,5', help='volcados de reposo (defecto: 0,1,3,5)')
    ap.add_argument('--yaw', type=int, default=2, help='volcado tras girar a un lado (defecto: 2)')
    ap.add_argument('--pitch', type=int, default=4, help='volcado tras inclinar (defecto: 4)')
    args = ap.parse_args()

    reposo = [int(x) for x in args.reposo.split(',')]
    numeros = sorted(set(reposo + [args.yaw, args.pitch]))
    volcados = {n: cargar(args.carpeta, n) for n in numeros}
    yaw = buscar_por_numero(volcados, reposo, args.yaw, 'yaw')
    pitch = buscar_por_numero(volcados, reposo, args.pitch, 'pitch')
    if len(yaw) == 1 and len(pitch) == 1:
        texto = f'yaw {yaw[0][0]:#x}\npitch {pitch[0][0]:#x}\n'
        Path('cam-direct.txt').write_text(texto)
        print('\nUn único par: cam-direct.txt escrito en la carpeta actual, listo para copiar '
              'a la carpeta del juego.')
    elif yaw and pitch:
        print('\nHay varios candidatos: el par bueno suele ser el de direcciones cercanas entre '
              'sí (el yaw y el pitch viven en la misma estructura).')


def buscar_por_numero(volcados, reposo, girado, nombre):
    """Los candidatos a ser el flotante buscado, con los volcados indexados por su número: lo que
    el giro movió respecto al primer reposo, filtrado a lo que en todos los reposos vale lo mismo
    (la cámara vuelve a su valor al regresar; lo animado, no)."""
    base0 = mapa(volcados[reposo[0]])
    giro = mapa(volcados[girado])
    movidos = set()
    for base, datos in base0.items():
        if base in giro:
            movidos.update((base, off) for off in difieren(datos, giro[base]))
    mapas = {n: mapa(volcados[n]) for n in volcados}
    salida = []
    for base, off in sorted(movidos):
        v0 = flotante(base0[base], off)
        if any(base not in mapas[r] or off + 4 > len(mapas[r][base])
               or flotante(mapas[r][base], off) != v0 for r in reposo):
            continue   # en reposo no siempre vale lo mismo: no es la cámara
        vg = flotante(giro[base], off)
        if abs(vg - v0) > 0.02 and abs(vg) < 100 and abs(v0) < 100:
            valores = [flotante(mapas[n][base], off) if base in mapas[n] and off + 4 <= len(mapas[n][base])
                       else None for n in sorted(volcados)]
            salida.append((base + off, valores, vg - v0))
    print(f'{nombre}: {len(salida)} candidato(s)')
    for addr, valores, delta in salida[:20]:
        vs = '  '.join('—' if v is None else f'{v:+.4f}' for v in valores)
        print(f'  {addr:#x}  delta {delta:+.4f}  valores: {vs}')
    return salida


if __name__ == '__main__':
    main()
