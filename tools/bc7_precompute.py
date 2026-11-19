#!/usr/bin/env python3
# Precómputo BC7 (fase 2, pieza A, doc 04): enumera TODAS las texturas ASTC del juego
# desde romfs.bin (romfs -> .pkz de raíz -> packs .dat/.dtt/.evn/.eff o sueltos ->
# pares .wta/.wtp o .wtb -> descriptores XT1), las escribe como fichero de trabajos
# y deja que el propio ejecutable haga el resto con sus dos modos:
#   --keys   astralchain.exe --bc7-keys:    claves (hash y total) sin codificar.
#   (sin)    astralchain.exe --bc7-convert: claves + bloques BC7 (GPU, modo 6) a
#            data.bin.partial; este script monta index.bin y el sello complete.txt.
# La caché resultante (cache/bc7/ junto al exe) la consulta el runtime en caliente
# (runtime/vk_bc7_cache.c, formato en runtime/bc7_cache.h).
# Adaptado de astral_chain/bc7_cache.py y shader_cache.py de AstralChain-PC
# (Puddz, MIT): la cadena de enumeración es la suya; lo que cambia es el conversor
# (aquí es el exe, no una DLL con bc7enc) y por tanto el flujo de trabajos.
# Uso:
#   bc7_precompute.py --romfs <carpeta romfs o romfs.bin> --out <carpeta cache/bc7>
#                     [--exe <astralchain.exe>] [--keys claves.txt] [--limit N]
# Sin --keys convierte; con --keys solo vuelca las claves y sale (validación, doc 04 §5).
# Necesita Python 3.14 (compression.zstd) o el paquete zstandard, como shader_census.py.
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

# Los 14 footprints ASTC en el orden de los formatos NVN 0x79..0x86 (UNORM) y
# 0x87..0x94 (sRGB): el mismo orden que nvn_fmt_bpb resuelve en el runtime.
_FOOTPRINTS = ((4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8), (10, 5), (10, 6),
               (10, 8), (10, 10), (12, 10), (12, 12))
_ASTC_BLOCKS = {0x79 + i: fp for i, fp in enumerate(_FOOTPRINTS)} | \
               {0x87 + i: fp for i, fp in enumerate(_FOOTPRINTS)}
_MAGIC, _VERSION = 0x43374342, 1          # "BC7C", el de bc7_cache.h
_FREE_MIN = 20 * 2**30                    # ~20 GB libres exigidos (la caché son ~16 GB)


def _zstd_decompress():
    try:
        from compression import zstd      # Python 3.14+
        return zstd.decompress
    except ImportError:
        pass
    try:
        import zstandard
        return lambda data: zstandard.ZstdDecompressor().decompressobj().decompress(data)
    except ImportError:
        return None


def _pkz_entries(f, base, decompress):
    """(nombre, tamaño, offset de datos, tamaño empaquetado) de un PKZ abierto en f."""
    f.seek(base)
    magic, _, _, count, table, names_size = struct.unpack('<4sIQIIQ', f.read(0x20))
    if magic != b'pkzl':
        raise ValueError('not a PKZ archive')
    f.seek(base + table)
    entries = f.read(count * 0x20)
    names = f.read(names_size)
    for i in range(count):
        name_off, size, data_off, packed = struct.unpack_from('<QQQQ', entries, i * 0x20)
        yield names[name_off:names.index(b'\0', name_off)].decode('ascii'), size, data_off, packed


def _dat_files(data):
    """(nombre, bytes) de cada fichero de un pack DAT."""
    if data[:4] != b'DAT\0':
        raise ValueError('not a DAT pack')
    count, files, _, names, sizes, _ = struct.unpack_from('<6I', data, 4)
    width = struct.unpack_from('<I', data, names)[0]
    for i in range(count):
        offset = struct.unpack_from('<I', data, files + 4 * i)[0]
        size = struct.unpack_from('<I', data, sizes + 4 * i)[0]
        name = data[names + 4 + i * width:names + 4 + (i + 1) * width].split(b'\0')[0].decode('ascii', 'replace')
        yield name, data[offset:offset + size]


def _inner(data):
    """Cada fichero de un DAT, packs anidados incluidos."""
    try:
        items = list(_dat_files(data))
    except Exception:
        return
    for name, blob in items:
        if blob[:4] == b'DAT\0':
            yield from _inner(blob)
        yield name, blob


def _textures(wta, wtp):
    """(campos XT1, datos, niveles 'short' o 0) de cada textura de un par WTA/WTP.
    campos = (fmt, w, h, levels, layers, packaged), lo que el conversor consume.
    Algunas texturas se crean con los mipmaps solo hasta un bloque ASTC (los mismos
    datos, menos niveles): short es ese número de niveles."""
    if wta[:4] != b'WTB\0':
        return
    count, offsets, sizes, _, _, info = struct.unpack_from('<6I', wta, 8)
    for k in range(count):
        x = wta[info + k * 0x38:info + (k + 1) * 0x38]
        if x[:4] != b'XT1\0':
            continue
        off, = struct.unpack_from('<I', wta, offsets + 4 * k)
        size, = struct.unpack_from('<I', wta, sizes + 4 * k)
        levels, kind, fmt, w, h, depth, _, packaged = struct.unpack_from('<8I', x, 0x14)
        if kind not in (1, 8) or depth > 1 or not 0x79 <= fmt <= 0x94:
            continue                      # fuera: no-2D/no-cubo, 3D y no-ASTC
        bw, bh = _ASTC_BLOCKS[fmt]
        short = 1
        while max(w >> short, 1) >= bw and max(h >> short, 1) >= bh and short < levels:
            short += 1
        yield (fmt, w, h, levels, 6 if kind == 8 else 1, packaged), \
            wtp[off:off + size], short if short < levels else 0


def _pairs(romfs_bin, decompress):
    """(campos, datos, short) de cada textura del juego, pack a pack, desde la imagen."""
    with romfs_bin.open('rb') as f:
        h = struct.unpack('<10Q', f.read(80))
        f.seek(h[7])
        table = f.read(h[8])
        packs, off = [], 0
        while off < len(table):
            _, _, doff, _, _, nlen = struct.unpack_from('<IIQQII', table, off)
            name = table[off + 32:off + 32 + nlen].decode('utf-8', 'replace')
            if name.endswith('.pkz'):
                packs.append(h[9] + doff)
            off += 32 + ((nlen + 3) & ~3)
        for base in packs:
            groups = {}
            for name, size, data_off, packed in _pkz_entries(f, base, decompress):
                stem, ext = os.path.splitext(name)
                if ext in ('.dat', '.dtt', '.evn', '.eff', '.wta', '.wtp', '.wtb'):
                    groups.setdefault(stem, []).append((name, size, data_off, packed))
            for entries in groups.values():
                wta, wtp, wtb = {}, {}, []
                for entry, size, data_off, packed in entries:
                    f.seek(base + data_off)
                    raw = f.read(packed)
                    data = raw if packed == size else decompress(raw)
                    for name, blob in [(entry, data)] if data[:4] != b'DAT\0' else _inner(data):
                        stem, ext = os.path.splitext(name)
                        if ext == '.wta':
                            wta[stem] = blob
                        elif ext == '.wtp':
                            wtp[stem] = blob
                        elif ext == '.wtb':
                            wtb.append(blob)
                for stem, a in wta.items():
                    if stem in wtp:
                        yield from _textures(a, wtp[stem])
                for blob in wtb:
                    yield from _textures(blob, blob)   # (cabeceras y datos en un fichero)


def _bc7_size(w, h, levels, layers):
    """Los bytes BC7 de una textura: la ac_bc7_size de bc7_cache.h, en Python."""
    n = 0
    for lv in range(levels):
        lw = max(w >> lv, 1)
        lh = max(h >> lv, 1)
        n += ((lw + 3) // 4) * ((lh + 3) // 4) * 16
    return n * layers


def _write_jobs(romfs_bin, jobs_path, decompress, limit=0):
    """Enumera y escribe el fichero de trabajos (formato de runtime/bc7_convert.c).
    Devuelve la lista de trabajos: (fmt, w, h, levels, layers, packaged).
    Deduplica por (campos, sha256 de los datos): la misma textura aparece en varios
    packs y solo se convierte una vez. Las variantes short van como trabajos propios
    con los mismos datos (mismo offset) y levels=short: su clave sale de su layout."""
    jobs = []
    seen = set()
    records = []                     # (campos, data_off, data_size), en orden de trabajos
    with jobs_path.open('wb') as out:
        out.write(struct.pack('<4sIII', b'BC7J', 1, 0, 0))   # cuenta y records_off, al final
        for fields, data, short in _pairs(romfs_bin, decompress):
            variants = [fields]
            if short:
                variants.append(fields[:3] + (short,) + fields[4:])
            for v in variants:
                fingerprint = (v, hashlib.sha256(data).digest())
                if fingerprint in seen:
                    continue
                seen.add(fingerprint)
                # Los datos se escriben al momento (el offset se comparte entre la
                # variante completa y su short: el exe lee de cada trabajo su trozo).
                records.append((v, out.tell(), len(data)))
                jobs.append(v)
                out.write(data)
            if limit and len(jobs) >= limit:
                break
        records_off = out.tell()
        for v, data_off, data_size in records:
            out.write(struct.pack('<6I2Q', *v, data_off, data_size))
        out.seek(0)
        out.write(struct.pack('<4sIII', b'BC7J', 1, len(records), records_off))
    return jobs


def _run_exe(exe, mode, jobs_path, out_dir):
    """Corre el exe en el modo dado y devuelve sus líneas de salida, en orden."""
    env = dict(os.environ)
    env['AC_BC7_JOBS'] = str(jobs_path)
    if out_dir is not None:
        env['AC_BC7_OUT'] = str(out_dir)
    r = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True)
    if r.returncode not in (0, 1) or (mode == '--bc7-keys' and r.returncode != 0):
        sys.stderr.write(r.stderr)
        raise SystemExit(f'{exe.name} {mode} salió con código {r.returncode}')
    if r.stderr:
        sys.stderr.write(r.stderr)
    return r.stdout.splitlines()


def _parse_keys(lines, count):
    """trabajo -> (hash, total) o None, cruzando las líneas del exe. Imprime en orden
    de trabajos: 'key <hash> <total> ...' por trabajo con clave, 'skip <i>' si no la
    tiene y, en --bc7-convert, 'fail <i>' DESPUÉS de su key si la conversión falló.
    Las líneas de progreso y de resumen (empiezan por 'bc7-') se ignoran."""
    keys = [None] * count
    current = 0
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        if parts[0] == 'key' and current < count:
            keys[current] = (int(parts[1], 16), int(parts[2]))
            current += 1
        elif parts[0] == 'skip':
            current = int(parts[1]) + 1
        elif parts[0] == 'fail':
            keys[int(parts[1])] = None
    return keys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', required=True, help='carpeta romfs de la instalación, o su romfs.bin')
    ap.add_argument('--out', required=True, help='carpeta de la caché (cache/bc7 junto al exe)')
    ap.add_argument('--exe', help='astralchain.exe (por defecto, junto a la carpeta romfs)')
    ap.add_argument('--keys', help='solo volcar las claves a este fichero y salir (validación)')
    ap.add_argument('--limit', type=int, default=0, help='parar tras N texturas (pruebas)')
    args = ap.parse_args()

    romfs = Path(args.romfs)
    romfs_bin = romfs if romfs.is_file() else romfs / 'romfs.bin'
    if not romfs_bin.is_file():
        raise SystemExit(f'no se encuentra romfs.bin en {args.romfs}')
    install = romfs_bin.parent.parent
    exe = Path(args.exe) if args.exe else install / 'astralchain.exe'
    if not exe.is_file():
        raise SystemExit(f'no se encuentra el ejecutable: {exe} (usa --exe)')
    out = Path(args.out)
    decompress = _zstd_decompress()
    if decompress is None:
        raise SystemExit('sin descompresor zstd: Python 3.14 o el paquete zstandard')

    # El sello hace el paso idempotente: misma herramienta y misma romfs = nada que hacer.
    seal = f'{hashlib.sha256(exe.read_bytes()).hexdigest()[:16]} gpu {romfs_bin.stat().st_size}'
    if not args.keys and (out / 'complete.txt').is_file() and \
            (out / 'complete.txt').read_text(encoding='ascii').strip() == seal:
        print('BC7 precompute: la caché ya está al día (sello)')
        return 0
    if not args.keys and shutil.disk_usage(out.parent if out.parent.exists() else Path('.')).free < _FREE_MIN:
        print('BC7 precompute: menos de ~20 GB libres; se salta (el juego codifica en vivo)')
        return 0

    out.mkdir(parents=True, exist_ok=True)
    jobs_path = out / 'bc7-jobs.bin'
    t0 = time.time()
    print('  Enumerando texturas…', flush=True)
    jobs = _write_jobs(romfs_bin, jobs_path, decompress, args.limit)
    print(f'  {len(jobs)} trabajos únicos ({time.time() - t0:.0f} s)', flush=True)

    if args.keys:
        lines = _run_exe(exe, '--bc7-keys', jobs_path, None)
        keys = _parse_keys(lines, len(jobs))
        with open(args.keys, 'w', encoding='ascii') as f:
            for job, k in zip(jobs, keys):
                if k:
                    f.write(f'{k[0]:016x} {k[1]} {job[0]:#x} {job[1]}x{job[2]} {job[3]} {job[4]}\n')
        print(f'{sum(1 for k in keys if k)} claves en {args.keys} ({time.time() - t0:.0f} s)')
        return 0

    lines = _run_exe(exe, '--bc7-convert', jobs_path, out)
    keys = _parse_keys(lines, len(jobs))
    if len(keys) != len(jobs):
        raise SystemExit(f'el exe informó de {len(keys)} trabajos y se le dieron {len(jobs)}')
    # Los registros: solo trabajos convertidos (clave presente y sin línea fail; el
    # parseo ya dejó None en skip/fail), deduplicados por clave+geometría como en el
    # runtime, con el offset que sus bloques ocupan en data.bin.partial (orden de
    # trabajos, solo convertidos). Ordenados por hash al final: la búsqueda binaria
    # de vk_bc7_cache.c lo exige.
    records, seen, offset = [], set(), 0
    failed = 0
    for job, k in zip(jobs, keys):
        if k is None:
            failed += 1
            continue
        size = _bc7_size(job[1], job[2], job[3], job[4])
        record_key = (k[0], k[1], job)
        if record_key not in seen:
            seen.add(record_key)
            records.append(struct.pack('<4Q6I', k[0], k[1], offset, size, *job[:5], 0))
        offset += size
    records.sort(key=lambda r: struct.unpack_from('<Q', r)[0])
    (out / 'index.bin.partial').write_bytes(struct.pack('<3I', _MAGIC, _VERSION, len(records)) + b''.join(records))
    for name in ('data.bin', 'index.bin'):
        (out / (name + '.partial')).replace(out / name)
    (out / 'complete.txt').write_text(seal, encoding='ascii')
    jobs_path.unlink()
    print(f'{len(records)} texturas, {offset / 2**30:.2f} GB, {failed} no convertidas, '
          f'{time.time() - t0:.0f} s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
