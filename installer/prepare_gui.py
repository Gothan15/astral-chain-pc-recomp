"""La ventana de preparación: python -m installer.prepare_gui (la abre Astral Chain-Preparar.exe).

Reprepara los shaders de UNA instalación ya hecha, usando el shader-answers.bin que el propio juego
fue grabando en esa carpeta mientras se jugaba: el Setup solo preparó las variantes que traía la
release, y las que el jugador encontró después quedan registradas pero sin preparar, así que el
juego las compila en mitad de la partida. Esta ventana corre las mismas herramientas que el paso
prepare del Setup (tools/prepare_shaders.py y tools/pack_shader_cache.py) contra la instalación
elegida.

Los textos viven en installer/text/<idioma>.json bajo "prepare.*" (Catalog de guilogic.py, con el
inglés de reserva), igual que la ventana del Setup.

    --lang en|ru|pt-BR|zh-Hans idioma de la ventana (por defecto, el de Windows)
    --root DIR                 la carpeta de la release (por defecto, la de este paquete)
"""
import argparse
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, ttk

from . import layout as layouts
from .guilogic import (Catalog, UI_LANGUAGE_LABELS, UI_LANGUAGES, inspect_folder,
                       windows_ui_language)
from .gui import dpi_aware, fix_tcl_paths
from .layout import version_tag

POLL_MS = 100
# Los mismos glifos y colores de estado que la ventana del Setup (installer/gui.py).
GLYPH = {'pending': '\u25cb', 'running': '\u25b6', 'done': '\u2714', 'skipped': '\u2013', 'failed': '\u2716'}
COLOR = {'pending': '#808080', 'running': '#1a5fb4', 'done': '#26a269', 'skipped': '#808080', 'failed': '#c01c28'}
OK_COLOR, BAD_COLOR, MUTED = '#26a269', '#c01c28', '#5e5c64'
PAD = 14
WRAP = 620

# Las líneas de progreso de las herramientas, con el mismo criterio que el paso prepare del Setup
# (installer/steps.py): la traducción la cuenta el propio juego (gfx_shader: prepare: N of M pairs)
# y los saltos por caída del driver los nombra prepare_shaders.py.
TRANSLATED = re.compile(r'gfx_shader: prepare: (\d+) of (\d+) pairs')
SKIPPED = re.compile(r'prepare-shaders: (\d+) variants skipped: the graphics driver crashed')
SEED = re.compile(r'(\d+) recorded variants and (\d+) default ones: (\d+) variants of (\d+) shaders')
PACKED = re.compile(r': (\d+) files, ([\d.]+) MiB')

# Espacio libre mínimo que la ventana exige antes de empezar.
FREE_SHADERS_GB = 2


def game_running():
    """¿Está astralchain.exe en marcha? La preparación usa ese mismo ejecutable y sus archivos."""
    try:
        if os.name == 'nt':
            out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq astralchain.exe', '/NH'],
                                 capture_output=True, text=True, timeout=15).stdout
            return 'astralchain.exe' in out.lower()
        return subprocess.run(['pgrep', '-x', 'astralchain'], capture_output=True, timeout=15).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def kill_tree(proc):
    """Acaba con la herramienta en curso y sus hijos (el juego que prepara o convierte)."""
    if proc.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
        return
    try:
        os.killpg(proc.pid, 9)
    except (ProcessLookupError, PermissionError, OSError):
        proc.kill()


class App:
    def __init__(self, root, ui_lang, layout):
        self.root, self.layout = root, layout
        self.cat = Catalog(ui_lang)
        self.bound = []                  # (widget, opción, id, params) que se retraducen al cambiar de idioma
        self.dest_var = tk.StringVar()
        self.events = queue.Queue()      # lo que el hilo de trabajo cuenta a la ventana
        self.proc = None                 # la herramienta en curso
        self.cancelled = False
        self.phase = None                # la fase en curso ('shaders', 'pack')
        self.result = None               # el resumen que deja el hilo al terminar
        self.log_tail = []               # las últimas líneas del registro, para la pantalla final si algo falla
        self.log_open = False
        self.scale = max(1.0, root.winfo_fpixels('1i') / 96.0)

        root.title(self.cat('prepare.title'))
        icon = layout.runtime / 'astralchain.ico'
        png = layout.runtime / 'astralchain.png'
        if os.name == 'nt' and icon.is_file():
            root.iconbitmap(default=str(icon))
        elif png.is_file():
            self.icon = tk.PhotoImage(file=str(png))
            root.iconphoto(True, self.icon)
        root.protocol('WM_DELETE_WINDOW', self.on_close)
        self.ui_font = 'Segoe UI' if os.name == 'nt' else 'DejaVu Sans'
        self.mono_font = 'Consolas' if os.name == 'nt' else 'DejaVu Sans Mono'
        style = ttk.Style(root)
        style.configure('Title.TLabel', font=(self.ui_font, 15, 'bold'))
        style.configure('Heading.TLabel', font=(self.ui_font, 12, 'bold'))
        style.configure('Notice.TLabel', background='#fff3cd', foreground='#664d03', padding=self.px(8))
        style.configure('Err.TLabel', foreground=BAD_COLOR)
        style.configure('Muted.TLabel', foreground=MUTED)
        style.configure('Version.TLabel', foreground='#808080')
        style.configure('Big.TButton', padding=(self.px(14), self.px(5)))

        top = ttk.Frame(root, padding=(self.px(PAD), self.px(PAD), self.px(PAD), 0))
        top.pack(fill='x')
        self.title = self.label(top, 'prepare.title', style='Title.TLabel')
        self.title.pack(side='left')
        version = ttk.Label(top, style='Version.TLabel')
        self.bind_text(version, 'text', 'ui.version', version=layout.release)
        version.pack(side='left', padx=(self.px(10), 0), pady=(self.px(6), 0))
        sw = ttk.Frame(top)
        sw.pack(side='right')
        self.ui_var = tk.StringVar(value=self.cat.lang)
        labels = [UI_LANGUAGE_LABELS[c] for c in UI_LANGUAGES]
        self.ui_box = ttk.Combobox(sw, values=labels, state='readonly', width=max(map(len, labels)) + 1)
        self.ui_box.set(UI_LANGUAGE_LABELS[self.cat.lang])
        self.ui_box.bind('<<ComboboxSelected>>', self.pick_language)
        self.ui_box.pack(side='left')
        self.body = ttk.Frame(root, padding=self.px(PAD))
        self.body.pack(fill='both', expand=True)
        self.frames = {'form': self.build_form(), 'run': self.build_run(), 'finish': self.build_finish()}
        self.dest_var.trace_add('write', lambda *_: self.refresh_form())
        self.refresh_form()
        self.show('form')
        root.after(POLL_MS, self.poll)

    def px(self, n):
        return int(round(n * self.scale))

    # ---- textos (el mismo mecanismo de retraducción que la ventana del Setup) -------------------

    def bind_text(self, widget, option, id, **params):
        self.bound = [b for b in self.bound if not (b[0] is widget and b[1] == option)]
        self.bound.append((widget, option, id, params))
        widget.configure(**{option: self.cat(id, **params)})

    def label(self, parent, id, **kw):
        w = ttk.Label(parent, **kw)
        self.bind_text(w, 'text', id)
        return w

    def wrapped(self, parent, id=None, width=WRAP, **kw):
        w = ttk.Label(parent, wraplength=self.px(width), justify='left', **kw)
        if id:
            self.bind_text(w, 'text', id)
        return w

    def button(self, parent, id, command, **kw):
        w = ttk.Button(parent, command=command, **kw)
        self.bind_text(w, 'text', id)
        return w

    def retext(self):
        self.root.title(self.cat('prepare.title'))
        for widget, option, id, params in self.bound:
            widget.configure(**{option: self.cat(id, **params)})
        self.refresh_form()
        self.refresh_finish()

    def pick_language(self, _event=None):
        self.ui_var.set(UI_LANGUAGES[self.ui_box.current()])
        self.cat.set_language(self.ui_var.get())
        self.ui_box.set(UI_LANGUAGE_LABELS[self.ui_var.get()])
        self.retext()
        self.fit()

    # ---- pantallas ------------------------------------------------------------------------------

    def show(self, name):
        for f in self.frames.values():
            f.pack_forget()
        self.frames[name].pack(fill='both', expand=True)
        self.screen = name
        self.fit()

    def fit(self):
        self.root.update_idletasks()
        sizes = [(f.winfo_reqwidth(), f.winfo_reqheight()) for f in self.frames.values()]
        chrome = self.root.winfo_reqheight() - self.body.winfo_reqheight()
        w = max(self.root.winfo_reqwidth(), max(s[0] for s in sizes) + 2 * self.px(PAD))
        h = chrome + max(s[1] for s in sizes) + 2 * self.px(PAD)
        self.root.minsize(w, h)
        self.root.geometry(f'{max(w, self.root.winfo_width())}x{max(h, self.root.winfo_height())}')

    def build_form(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(self.px(12), 0))
        self.start_button = self.button(bar, 'prepare.start', self.start, style='Big.TButton', default='active')
        self.start_button.pack(side='right')
        self.wrapped(f, 'prepare.intro').pack(anchor='w', pady=(0, self.px(12)))
        self.label(f, 'prepare.folder', style='Heading.TLabel').pack(anchor='w', pady=(0, self.px(4)))
        row = ttk.Frame(f)
        row.pack(fill='x')
        self.dest_entry = ttk.Entry(row, textvariable=self.dest_var)
        self.dest_entry.pack(side='left', fill='x', expand=True)
        self.button(row, 'ui.browse', self.browse).pack(side='left', padx=(self.px(8), 0))
        self.dest_status = ttk.Label(f, wraplength=self.px(WRAP), justify='left')
        self.dest_status.pack(anchor='w', pady=(self.px(6), 0))
        self.form_problem = ttk.Label(f, wraplength=self.px(WRAP), justify='left', style='Err.TLabel')
        self.form_problem.pack(anchor='w', pady=(self.px(10), 0))
        return f

    def build_run(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(self.px(8), 0))
        self.cancel_button = self.button(bar, 'ui.cancel', self.cancel)
        self.cancel_button.pack(side='right')
        self.log_toggle = self.button(bar, 'ui.show_log', self.toggle_log)
        self.log_toggle.pack(side='left')
        header = ttk.Label(f, style='Heading.TLabel')
        self.bind_text(header, 'text', 'prepare.running')
        header.pack(anchor='w')
        self.step_rows = {}
        self.steps_box = ttk.Frame(f)
        self.steps_box.pack(fill='x', pady=(self.px(8), self.px(8)))
        self.phase_label = self.wrapped(f)
        self.phase_label.pack(anchor='w')
        self.bar = ttk.Progressbar(f, mode='determinate', maximum=1000)
        self.bar.pack(fill='x', pady=(self.px(4), self.px(6)))
        self.log_frame = ttk.Frame(f)
        self.log = tk.Text(self.log_frame, height=10, wrap='word', state='disabled', font=(self.mono_font, 9))
        scroll = ttk.Scrollbar(self.log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)
        return f

    def build_finish(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(self.px(12), 0))
        self.button(bar, 'ui.close', self.on_close).pack(side='right')
        self.open_button = self.button(bar, 'ui.open_folder', self.open_folder)
        self.open_button.pack(side='right', padx=(0, self.px(8)))
        self.finish_title = ttk.Label(f, style='Title.TLabel', wraplength=self.px(WRAP), justify='left')
        self.finish_title.pack(anchor='w')
        self.finish_text = self.wrapped(f)
        self.finish_text.pack(anchor='w', pady=(self.px(8), 0))
        # El final del registro, visible solo cuando algo falló (el registro completo vive en la
        # pantalla de la marcha, y esta es su cola).
        self.finish_log = tk.Text(f, height=8, wrap='word', state='disabled', font=(self.mono_font, 9))
        return f

    # ---- el formulario: validar la carpeta y poder empezar ---------------------------------------

    def browse(self):
        chosen = filedialog.askdirectory(initialdir=self.dest_var.get() or None)
        if chosen:
            self.dest_var.set(chosen)

    def install_info(self):
        """(destino, versión del juego) si la carpeta es una instalación utilizable; (None, motivo)."""
        dest = Path(self.dest_var.get().strip())
        folder = inspect_folder(str(dest))
        if folder.kind != 'install':
            return None, 'not_install'
        if not (dest / 'astralchain.exe').is_file() or not (dest / 'romfs' / 'romfs.bin').is_file():
            return None, 'not_install'
        version = (folder.state or {}).get('version', '1.0.1')
        return dest, version

    def refresh_form(self):
        if getattr(self, 'screen', None) not in (None, 'form'):
            return
        dest, info = (None, None) if not self.dest_var.get().strip() else self.install_info()
        if dest is not None:
            self.dest_status.configure(style='Muted.TLabel')
            self.bind_text(self.dest_status, 'text', 'prepare.found', version=info)
            self.start_button.state(['!disabled'])
        else:
            self.dest_status.configure(style='Err.TLabel' if self.dest_var.get().strip() else 'Muted.TLabel')
            if self.dest_var.get().strip():
                self.bind_text(self.dest_status, 'text', 'prepare.not_install')
            else:
                self.dest_status.configure(text='')
                self.bound = [b for b in self.bound if b[0] is not self.dest_status]
            self.start_button.state(['disabled'])

    # ---- la marcha --------------------------------------------------------------------------------

    def start(self):
        dest, version = self.install_info()
        if dest is None:
            return
        if game_running():
            self.form_problem.configure(text=self.cat('prepare.game_running'))
            return
        need = FREE_SHADERS_GB
        free = shutil.disk_usage(dest).free / 2**30
        if free < need:
            self.form_problem.configure(text=self.cat('prepare.low_space', free=f'{free:.1f}', need=need))
            return
        self.form_problem.configure(text='')
        self.cancelled = False
        self.result = None
        steps = ['shaders', 'pack']
        for child in self.steps_box.winfo_children():
            child.destroy()
        self.step_rows = {}
        for step in steps:
            row = ttk.Frame(self.steps_box)
            row.pack(fill='x', pady=self.px(1))
            glyph = ttk.Label(row, text=GLYPH['pending'], foreground=COLOR['pending'], width=2)
            glyph.pack(side='left')
            name = ttk.Label(row)
            self.bind_text(name, 'text', f'prepare.step_{step}')
            name.pack(side='left')
            self.step_rows[step] = glyph
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.configure(state='disabled')
        self.show('run')
        self.set_step('shaders', 'running')
        threading.Thread(target=self.work, args=(dest, version), daemon=True).start()

    def set_step(self, step, state):
        glyph = self.step_rows.get(step)
        if glyph:
            glyph.configure(text=GLYPH[state], foreground=COLOR[state])

    def set_phase(self, id, **params):
        self.phase_label.configure(text=self.cat(id, **params) if id else '')
        self.bound = [b for b in self.bound if b[0] is not self.phase_label]
        if id:
            self.bound.append((self.phase_label, 'text', id, params))

    def run_tool(self, args, watch=None):
        """Corre una herramienta de la release contando sus líneas a la ventana (y a watch, en este
        mismo hilo, para que el trabajo anote el resumen). Devuelve su código de salida."""
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        self.proc = subprocess.Popen([str(self.layout.python), *map(str, args)],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding='utf-8', errors='replace', bufsize=1,
                                     creationflags=flags, start_new_session=os.name != 'nt')
        for line in self.proc.stdout:
            line = line.rstrip()
            self.events.put(('line', line))
            if watch:
                watch(line)
            if self.cancelled:
                break
        code = self.proc.wait()
        self.proc = None
        return code

    def work(self, dest, version):
        """El hilo de trabajo: shaders y empaquetado."""
        out = {'shaders': None, 'skipped': 0, 'pack': None}
        try:
            self.phase = 'shaders'
            self.events.put(('phase', 'prepare.phase_seed', {}))
            answers = dest / 'shader-answers.bin'
            args = [self.layout.tools / 'prepare_shaders.py', dest, '--version', version_tag(version),
                    '--romfs', dest / 'romfs']
            if answers.is_file():
                args += ['--answers', answers]      # las variantes que ESTE jugador grabó
            elif self.layout.shader_answers:
                args += ['--answers', self.layout.shader_answers]

            def watch_shaders(line):
                m = SEED.search(line)
                if m:
                    out['seed'] = (int(m.group(3)), int(m.group(4)))   # variantes, shaders
                m = SKIPPED.search(line)
                if m:
                    out['skipped'] = int(m.group(1))

            code = self.run_tool(args, watch_shaders)
            if self.cancelled:
                return self.finish_thread('cancelled', out)
            if code != 0 or not (dest / 'shader-prepared.bin').is_file():
                return self.finish_thread('failed', out)
            self.events.put(('step', 'shaders', 'done'))
            self.phase = 'pack'
            self.set_step_event('pack', 'running')
            self.events.put(('phase', 'prepare.phase_pack', {}))
            self.events.put(('bar', None))

            def watch_pack(line):
                m = PACKED.search(line)
                if m:
                    out['pack'] = (int(m.group(1)), m.group(2))

            code = self.run_tool([self.layout.tools / 'pack_shader_cache.py', dest / 'shader-cache'],
                                 watch_pack)
            if self.cancelled:
                return self.finish_thread('cancelled', out)
            if code != 0:
                return self.finish_thread('failed', out)
            self.events.put(('step', 'pack', 'done'))
            self.finish_thread('done', out)
        except Exception as e:   # la ventana lo cuenta como un fallo con el detalle en el registro
            self.events.put(('line', f'{type(e).__name__}: {e}'))
            self.finish_thread('failed', out)

    def set_step_event(self, step, state):
        self.events.put(('step', step, state))

    def finish_thread(self, result, out):
        self.result = (result, out)
        self.events.put(('finish', result))

    # ---- el sondeo: pasar lo del hilo a la ventana -------------------------------------------------

    def poll(self):
        try:
            while True:
                event = self.events.get_nowait()
                kind = event[0]
                if kind == 'line':
                    self.on_line(event[1])
                elif kind == 'step':
                    self.set_step(event[1], event[2])
                elif kind == 'phase':
                    self.set_phase(event[1], **event[2])
                elif kind == 'bar':
                    self.bar.configure(mode='indeterminate' if event[1] is None else 'determinate')
                    if event[1] is not None:
                        self.bar.configure(value=event[1] * 1000)
                elif kind == 'finish':
                    self.refresh_finish()
                    self.show('finish')
        except queue.Empty:
            pass
        self.root.after(POLL_MS, self.poll)

    def on_line(self, line):
        self.log.configure(state='normal')
        self.log.insert('end', line + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')
        self.log_tail = (self.log_tail + [line])[-40:]
        m = TRANSLATED.search(line)
        if m:
            done, total = int(m.group(1)), int(m.group(2))
            self.bar.configure(mode='determinate', value=1000 * done / max(total, 1))
            self.set_phase('prepare.phase_translate', done=done, total=total)

    # ---- cancelar, cerrar, la pantalla final -------------------------------------------------------

    def cancel(self):
        self.cancelled = True
        if self.proc is not None:
            kill_tree(self.proc)

    def on_close(self):
        if self.proc is not None:
            self.cancel()
        self.root.destroy()

    def toggle_log(self):
        self.log_open = not self.log_open
        if self.log_open:
            self.log_frame.pack(fill='both', expand=True, pady=(self.px(6), 0))
            self.bind_text(self.log_toggle, 'text', 'ui.hide_log')
        else:
            self.log_frame.pack_forget()
            self.bind_text(self.log_toggle, 'text', 'ui.show_log')
        self.fit()

    def open_folder(self):
        dest = self.dest_var.get().strip()
        if not dest:
            return
        if os.name == 'nt':
            os.startfile(dest)   # noqa: S606 - la carpeta que el jugador eligió
        else:
            subprocess.Popen(['xdg-open', dest])

    def refresh_finish(self):
        if self.result is None:
            return
        result, out = self.result
        titles = {'done': 'prepare.done_title', 'failed': 'prepare.failed_title',
                  'cancelled': 'prepare.cancelled_title'}
        self.bind_text(self.finish_title, 'text', titles[result])
        lines = []
        seed = out.get('seed')
        if result == 'done' and seed:
            lines.append(self.cat('prepare.sum_shaders', variants=seed[0], shaders=seed[1]))
        if out.get('skipped'):
            lines.append(self.cat('prepare.sum_skipped', count=out['skipped']))
        pack = out.get('pack')
        if result == 'done' and pack:
            lines.append(self.cat('prepare.sum_pack', files=pack[0], mb=pack[1]))
        if result == 'failed':
            lines.append(self.cat('prepare.failed_text'))
        elif result == 'cancelled':
            lines.append(self.cat('prepare.cancelled_text'))
        self.finish_text.configure(text='\n'.join(lines))
        self.bound = [b for b in self.bound if b[0] is not self.finish_text]
        if result == 'failed':
            self.finish_log.configure(state='normal')
            self.finish_log.delete('1.0', 'end')
            self.finish_log.insert('end', '\n'.join(self.log_tail))
            self.finish_log.configure(state='disabled')
            self.finish_log.pack(fill='x', pady=(self.px(8), 0))
        else:
            self.finish_log.pack_forget()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--lang', choices=UI_LANGUAGES)
    ap.add_argument('--root', type=Path)
    args = ap.parse_args(argv)
    layout = layouts.find(args.root)
    dpi_aware()
    fix_tcl_paths()
    root = tk.Tk()
    App(root, args.lang or windows_ui_language(), layout)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
