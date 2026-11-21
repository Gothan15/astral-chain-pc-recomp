"""astralchain.ini: set the game language and keep everything else (keys and syntax as runtime/cfg.c reads them).

A new file starts from astralchain.ini.example, whose lines are all commented defaults. The language goes into `lang`
(a Switch language code, runtime/ov_system.c): an active `lang = ...` line is rewritten in place, else the commented
`# lang = ...` line of the example is activated, else the line is added to [language] (created at the end if
missing). Other lines, comments and line order stay as they are.
"""
import os
import re
from pathlib import Path

# runtime/ov_system.c: the codes AC_LANG offers. Each language appears once, under the code the game accepts.
LANGUAGES = ('ja', 'en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'nl', 'pt', 'pt-BR', 'ru', 'ko',
             'zh-Hans', 'zh-Hant')
# Older installer builds wrote these two codes; they are read as the canonical ones and rewritten on the next install.
ALIASES = {'zh-CN': 'zh-Hans', 'zh-TW': 'zh-Hant'}
# Their text draws with the Switch shared fonts (runtime/ov_system.c). The setup has no firmware to take them from;
# the runtime gives the game a substitute for shared font 0 only.
NEED_SHARED_FONTS = ('ko', 'zh-Hans', 'zh-Hant')

ACTIVE = re.compile(r'^(\s*)lang(\s*=\s*)(.*?)(\s*)$', re.IGNORECASE)
COMMENTED = re.compile(r'^\s*[#;]\s*lang\s*=.*$', re.IGNORECASE)
SECTION = re.compile(r'^\s*\[(.*)\]\s*$')


def canonical_language(lang):
    """The code LANGUAGES lists for lang: an alias of an older installer build is mapped, anything else is kept."""
    return ALIASES.get(lang, lang)


def set_language(text, lang):
    """text with `lang = <lang>` (an alias written as its canonical code); see the module docstring."""
    lang = canonical_language(lang)
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = ACTIVE.match(line)
        if m:
            lines[i] = f'{m.group(1)}lang{m.group(2)}{lang}'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        if COMMENTED.match(line):
            lines[i] = f'lang = {lang}'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        m = SECTION.match(line)
        if m and m.group(1).strip().lower() == 'language':
            lines.insert(i + 1, f'lang = {lang}')
            return '\n'.join(lines) + '\n'
    if lines and lines[-1].strip():
        lines.append('')
    lines += ['[language]', f'lang = {lang}']
    return '\n'.join(lines) + '\n'


def get_language(text):
    for line in text.splitlines():
        m = ACTIVE.match(line)
        if m:
            return m.group(3)
    return None


def write(dest, lang, example=None):
    """Write dest/astralchain.ini with lang; an existing file keeps its other lines. Returns the path."""
    path = Path(dest) / 'astralchain.ini'
    if path.is_file():
        text = path.read_text(encoding='utf-8-sig')
    elif example and Path(example).is_file():
        text = Path(example).read_text(encoding='utf-8')
    else:
        text = ''
    new = set_language(text, lang)
    if path.is_file() and new == text:
        return path
    tmp = path.with_name('astralchain.ini.tmp')
    tmp.write_text(new, encoding='utf-8', newline='\n')
    os.replace(tmp, path)
    return path


# astc_bc (runtime/cfg.c lo lee de [performance]): la ventana de preparación lo pone a bc7 cuando
# construye la caché de texturas, con las mismas reglas que set_language para lang.
BC_ACTIVE = re.compile(r'^(\s*)astc_bc(\s*=\s*)(.*?)(\s*)$', re.IGNORECASE)
BC_COMMENTED = re.compile(r'^\s*[#;]\s*astc_bc\s*=.*$', re.IGNORECASE)


def set_astc_bc(text):
    """text con `astc_bc = bc7` activo; el resto del archivo queda como estaba."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = BC_ACTIVE.match(line)
        if m:
            lines[i] = f'{m.group(1)}astc_bc{m.group(2)}bc7'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        if BC_COMMENTED.match(line):
            lines[i] = 'astc_bc = bc7'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        m = SECTION.match(line)
        if m and m.group(1).strip().lower() == 'performance':
            lines.insert(i + 1, 'astc_bc = bc7')
            return '\n'.join(lines) + '\n'
    if lines and lines[-1].strip():
        lines.append('')
    lines += ['[performance]', 'astc_bc = bc7']
    return '\n'.join(lines) + '\n'


def write_astc_bc(dest):
    """Pone astc_bc = bc7 en dest/astralchain.ini conservando lo demás. Devuelve el path."""
    path = Path(dest) / 'astralchain.ini'
    text = path.read_text(encoding='utf-8-sig') if path.is_file() else ''
    new = set_astc_bc(text)
    if path.is_file() and new == text:
        return path
    tmp = path.with_name('astralchain.ini.tmp')
    tmp.write_text(new, encoding='utf-8', newline='\n')
    os.replace(tmp, path)
    return path
