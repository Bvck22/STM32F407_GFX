"""GFX multi-effect pedal controller (Tkinter). Talks to the board or the built-in simulator."""
from __future__ import annotations

import argparse
import json
import queue
import sys
import tkinter as tk
from concurrent.futures import Future
from tkinter import filedialog, messagebox, ttk
from typing import Callable

import gfx_link
from gfx_scope import ScopeWindow
from gfx_link import Algorithm, ChainState, Connection, DeviceError, LoopStatus, ParamDesc, Status
from gfx_widgets import (ACCENT, AMBER, BG, DIM, EDGE, GREEN, LCD, PANEL, PANEL2, RED, TEXT, ChainStrip,
                         Footswitch, HBar, Knob, LoopBar, PeakMeter, category_color)

SIM_LABEL = "Simulator"
SET_INTERVAL_MS = 30
STATUS_INTERVAL_MS = 200
PEAK_CLIP = 0.99
KNOBS = 8
CATEGORY_TABS = ["Delay", "Modulation", "Pitch", "Reverb", "Drive", "Looper", "None"]
ERROR_TEXT = {"ERR memory": "ERR: NOT ENOUGH MEMORY", "ERR cpu": "ERR: CPU BUDGET EXCEEDED",
              "ERR no looper": "ERR: NO LOOPER IN CHAIN", "ERR flash": "ERR: FLASH WRITE FAILED",
              "ERR no preset": "ERR: EMPTY PRESET"}


class Coalescer:
    """At most one send per key per interval; the final value always goes out."""

    def __init__(self, root: tk.Misc, send: Callable[[tuple, float], None]) -> None:
        self._root = root
        self._send = send
        self._pending: dict[tuple, float] = {}

    def submit(self, key: tuple, value: float) -> None:
        if key not in self._pending:
            self._root.after(SET_INTERVAL_MS, self._flush, key)
        self._pending[key] = value

    def _flush(self, key: tuple) -> None:
        if key in self._pending:
            self._send(key, self._pending.pop(key))

    def flush_all(self) -> None:
        for key in list(self._pending):
            self._flush(key)


class AlgoBrowser(tk.Toplevel):
    """Algorithm list grouped by category tabs."""

    def __init__(self, app: "App", slot: int) -> None:
        super().__init__(app.root, bg=BG)
        self.title(f"Slot {slot + 1} - choose algorithm")
        self.transient(app.root)
        self.bind("<Escape>", lambda _e: self.destroy())
        current = app.chain.slots[slot].algo_id if app.chain else -1
        book = ttk.Notebook(self)
        book.pack(fill="both", expand=True, padx=8, pady=8)
        for category in CATEGORY_TABS:
            members = [a for a in app.algos.values() if a.category == category]
            if not members:
                continue
            tab = tk.Frame(book, bg=PANEL)
            book.add(tab, text="Empty" if category == "None" else category)
            for i, algo in enumerate(members):
                self._add_button(tab, i, algo, algo.id == current, lambda a=algo: self._pick(app, slot, a))
        self.geometry(f"+{app.root.winfo_rootx() + 120}+{app.root.winfo_rooty() + 120}")

    @staticmethod
    def _add_button(tab: tk.Frame, i: int, algo: Algorithm, current: bool, command: Callable[[], None]) -> None:
        text = f"{algo.name}\n{len(algo.params)} params" if algo.params else algo.name
        btn = tk.Button(tab, text=text, width=16, height=3, command=command, relief="flat",
                        bg=category_color(algo.category), fg="white", activebackground=ACCENT,
                        font=("Segoe UI", 10, "bold"), highlightthickness=3 if current else 0,
                        highlightbackground=ACCENT, highlightcolor=ACCENT, bd=0)
        btn.grid(row=i // 4, column=i % 4, padx=6, pady=6)

    def _pick(self, app: "App", slot: int, algo: Algorithm) -> None:
        self.destroy()
        app.choose_algo(slot, algo.id)


class SaveDialog(tk.Toplevel):
    """Ask for a preset slot and name."""

    def __init__(self, app: "App") -> None:
        super().__init__(app.root, bg=PANEL)
        self.title("Save preset")
        self.transient(app.root)
        chain = app.chain
        free = next((i for i in range(16) if i not in app.presets), 0)
        index = chain.preset_index if chain and chain.preset_index >= 0 else free
        self.slot_var = tk.StringVar(value=str(index + 1))
        self.name_var = tk.StringVar(value=(chain.preset_name if chain else "") or "New preset")
        ttk.Label(self, text="Slot").grid(row=0, column=0, padx=8, pady=8, sticky="w")
        ttk.Combobox(self, textvariable=self.slot_var, width=4, state="readonly",
                     values=[str(i + 1) for i in range(16)]).grid(row=0, column=1, sticky="w")
        ttk.Label(self, text="Name").grid(row=1, column=0, padx=8, sticky="w")
        entry = ttk.Entry(self, textvariable=self.name_var, width=22)
        entry.grid(row=1, column=1, padx=8, pady=4)
        entry.focus_set()
        ttk.Button(self, text="Save", command=lambda: self._ok(app)).grid(row=2, column=1, pady=8, sticky="e", padx=8)
        self.bind("<Return>", lambda _e: self._ok(app))
        self.bind("<Escape>", lambda _e: self.destroy())

    def _ok(self, app: "App") -> None:
        name = self.name_var.get().strip()[:15]
        if name:
            self.destroy()
            app.save_preset(int(self.slot_var.get()) - 1, name)


class App:
    def __init__(self, root: tk.Tk, port: str | None, sim: bool) -> None:
        self.root = root
        self.conn: Connection | None = None
        self.algos: dict[int, Algorithm] = {}
        self.chain: ChainState | None = None
        self.presets: dict[int, str] = {}
        self.selected = 0
        self.loop: LoopStatus | None = None
        self.scope: ScopeWindow | None = None
        self.play = False
        self.last_param: tuple[ParamDesc, float] | None = None
        self.errors: list[BaseException] = []
        self.status_busy = False
        self.ui_queue: queue.Queue[Callable[[], None]] = queue.Queue()
        self.coalescer = Coalescer(root, self._send_value)
        self.sim_var = tk.BooleanVar(value=sim)
        root.report_callback_exception = self._on_tk_error
        root.title("GFX")
        root.configure(bg=BG)
        root.geometry("1200x760")
        root.minsize(980, 680)
        self._style()
        self._build_connection_bar()
        self._build_status_bar()
        self._build_sidebar()
        self._build_body()
        self._bind_keys()
        self._refresh_ports(port)
        self._show_idle_gauges()
        self.refresh_all()
        root.after(20, self._drain_ui)
        root.after(STATUS_INTERVAL_MS, self._poll_status)
        root.protocol("WM_DELETE_WINDOW", self._quit)

    # ---- styling & layout -------------------------------------------------
    def _style(self) -> None:
        style = ttk.Style()
        style.theme_use("clam")
        style.configure(".", background=BG, foreground=TEXT, fieldbackground=PANEL2, bordercolor=EDGE,
                        lightcolor=PANEL2, darkcolor=PANEL2, troughcolor=PANEL, font=("Segoe UI", 9))
        style.configure("TButton", background=PANEL2, padding=(10, 4))
        style.map("TButton", background=[("active", EDGE), ("disabled", PANEL)],
                  foreground=[("disabled", DIM)])
        style.configure("TCheckbutton", background=BG)
        style.map("TCheckbutton", background=[("active", BG)])
        style.configure("TNotebook", background=BG)
        style.configure("TNotebook.Tab", background=PANEL2, foreground=TEXT, padding=(14, 6))
        style.map("TNotebook.Tab", background=[("selected", ACCENT)], foreground=[("selected", "#000")])
        style.map("TCombobox", fieldbackground=[("readonly", PANEL2)], foreground=[("readonly", TEXT)])
        self.root.option_add("*TCombobox*Listbox.background", PANEL2)
        self.root.option_add("*TCombobox*Listbox.foreground", TEXT)

    def _build_connection_bar(self) -> None:
        bar = tk.Frame(self.root, bg=BG)
        bar.pack(fill="x", padx=10, pady=(8, 4))
        ttk.Label(bar, text="Port").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(bar, textvariable=self.port_var, width=28)
        self.port_box.pack(side="left", padx=6)
        ttk.Button(bar, text="Refresh", takefocus=False, command=self._refresh_ports).pack(side="left")
        ttk.Checkbutton(bar, text="Simulator", variable=self.sim_var, takefocus=False).pack(side="left", padx=10)
        self.connect_btn = ttk.Button(bar, text="Connect", takefocus=False, command=self._toggle_connection)
        self.connect_btn.pack(side="left")
        ttk.Button(bar, text="Scope", takefocus=False, command=self.open_scope).pack(side="left", padx=(10, 0))
        self.state_var = tk.StringVar(value="Disconnected")
        ttk.Label(bar, textvariable=self.state_var, foreground=DIM).pack(side="left", padx=14)

    def _build_status_bar(self) -> None:
        bar = tk.Frame(self.root, bg=BG)
        bar.pack(side="bottom", fill="x", padx=10, pady=(4, 8))
        self.cpu_bar, self.mem_bar, self.cost_bar = (HBar(bar, 140) for _ in range(3))
        for widget in (self.cpu_bar, self.mem_bar, self.cost_bar):
            widget.pack(side="left", padx=(0, 10))
        self.hint_var = tk.StringVar(value="Drag knobs (Shift = fine) - wheel = step - double-click = default"
                                           "   |   keys: 1-4 slot, Space play, B bypass, R rec, S scope")
        ttk.Label(bar, textvariable=self.hint_var, foreground=DIM).pack(side="right")

    def _build_sidebar(self) -> None:
        side = tk.Frame(self.root, bg=PANEL, highlightbackground=EDGE, highlightthickness=1)
        side.pack(side="right", fill="y", padx=(0, 10), pady=4)
        tk.Label(side, text="PRESETS", bg=PANEL, fg=ACCENT, font=("Segoe UI", 10, "bold")).pack(pady=(10, 4))
        self.preset_list = tk.Listbox(side, width=24, height=16, bg="#111317", fg=TEXT, bd=0, takefocus=0,
                                      selectbackground=ACCENT, selectforeground="#000", exportselection=False,
                                      activestyle="none", font=("Consolas", 10), highlightthickness=0)
        self.preset_list.pack(padx=10, pady=4)
        self.preset_list.bind("<<ListboxSelect>>", self._on_preset_click)
        grid = tk.Frame(side, bg=PANEL)
        grid.pack(padx=10, pady=8)
        for i, (text, cmd) in enumerate((("Save...", self._ask_save), ("Delete", self._delete_selected),
                                         ("Export JSON", self._export), ("Import JSON", self._import))):
            ttk.Button(grid, text=text, takefocus=False, command=cmd, width=11).grid(
                row=i // 2, column=i % 2, padx=2, pady=2)
        ttk.Button(side, text="Reset to defaults", takefocus=False, command=self._defaults).pack(pady=(0, 10))

    def _build_body(self) -> None:
        body = tk.Frame(self.root, bg=BG)
        body.pack(side="left", fill="both", expand=True, padx=(10, 10), pady=4)
        self._build_header(body)
        self._build_knob_row(body)
        self.strip = ChainStrip(body, self.select_slot, self.toggle_slot, self.move_slot, self.open_browser)
        self.strip.pack(fill="x", pady=8)
        self._build_footswitches(body)

    @staticmethod
    def _panel(parent: tk.Misc) -> tk.Frame:
        return tk.Frame(parent, bg=PANEL, highlightbackground=EDGE, highlightthickness=1)

    def _build_header(self, body: tk.Frame) -> None:
        head = self._panel(body)
        head.pack(fill="x")
        mark = tk.Canvas(head, width=110, height=112, bg=PANEL, highlightthickness=0)
        mark.create_text(55, 46, text="GFX", fill=ACCENT, font=("Segoe UI Black", 30, "italic"))
        mark.create_text(55, 80, text="MULTI-EFFECT", fill=DIM, font=("Segoe UI", 8, "bold"))
        mark.pack(side="left", padx=(8, 0), pady=8)
        self.lcd = LCD(head)
        self.lcd.pack(side="left", padx=8, pady=8, expand=True)
        self.meter = PeakMeter(head)
        self.meter.pack(side="right", padx=(4, 12), pady=8)
        self.global_knobs: dict[str, Knob] = {}
        for name in ("out.level", "in.level"):
            knob = Knob(head, size=62, on_change=lambda v, n=name: self._on_global(n, v))
            knob.pack(side="right", padx=2, pady=8)
            self.global_knobs[name] = knob

    def _build_knob_row(self, body: tk.Frame) -> None:
        row = self._panel(body)
        row.pack(fill="x", pady=(8, 0))
        self.knobs: list[Knob] = []
        for i in range(KNOBS):
            row.columnconfigure(i, weight=1)
            knob = Knob(row, size=84, on_change=lambda v, i=i: self._on_knob(i, v))
            knob.grid(row=0, column=i, padx=2, pady=(10, 6))
            self.knobs.append(knob)

    def _build_footswitches(self, body: tk.Frame) -> None:
        foot = self._panel(body)
        foot.pack(fill="both", expand=True)
        row = tk.Frame(foot, bg=PANEL)
        row.pack(side="left", padx=10, pady=6)
        self.play_sw = Footswitch(row, "PLAY / STOP", self.toggle_play)
        self.bypass_sw = Footswitch(row, "BYPASS", self.toggle_bypass)
        self.play_sw.pack(side="left", padx=6)
        self.bypass_sw.pack(side="left", padx=6)
        ttk.Separator(row, orient="vertical").pack(side="left", fill="y", padx=14)
        self.loop_sw = {
            "REC": Footswitch(row, "REC", lambda: self.loop_action("REC")),
            "PLAY": Footswitch(row, "PLAY / DUB", self.loop_play_dub),
            "STOP": Footswitch(row, "STOP", lambda: self.loop_action("STOP")),
            "CLEAR": Footswitch(row, "CLEAR", lambda: self.loop_action("CLEAR")),
        }
        for sw in self.loop_sw.values():
            sw.pack(side="left", padx=6)
        right = tk.Frame(foot, bg=PANEL)
        right.pack(side="left", fill="x", expand=True, padx=10)
        tk.Label(right, text="LOOPER", bg=PANEL, fg=category_color("Looper"),
                 font=("Segoe UI", 9, "bold")).pack(anchor="w")
        self.loop_bar = LoopBar(right, 280, 22, bg=PANEL)
        self.loop_bar.pack(anchor="w", pady=4)
        self.loop_hint = tk.Label(right, text="", bg=PANEL, fg=DIM, font=("Segoe UI", 8))
        self.loop_hint.pack(anchor="w")

    def open_scope(self) -> None:
        if self.scope:
            self.scope.lift()
        elif self.conn:
            self.scope = ScopeWindow(self)
        else:
            self.lcd.flash("NOT CONNECTED")

    def _bind_keys(self) -> None:
        self.root.bind_all("<Key>", self._on_key)

    # ---- thread hop & async helpers ------------------------------------------
    def ui(self, fn: Callable[[], None]) -> None:
        """Thread-safe: run fn on the Tk thread."""
        self.ui_queue.put(fn)

    def _drain_ui(self) -> None:
        while True:
            try:
                self.ui_queue.get_nowait()()
            except queue.Empty:
                break
        self.root.after(20, self._drain_ui)

    def _on_tk_error(self, exc_type, exc, tb) -> None:
        self.errors.append(exc)
        sys.excepthook(exc_type, exc, tb)

    def ask(self, make: Callable[[Connection], Future], ok: Callable = lambda _r: None,
            resync_on_error: bool = False, fail: Callable[[Exception], None] | None = None) -> None:
        """Issue a command; `ok` and error reporting run on the Tk thread."""
        conn = self.conn
        if conn is None:
            return

        def done(f: Future) -> None:
            try:
                result = f.result()
            except Exception as exc:
                self.ui(lambda: self._failed(exc, resync_on_error, fail))
            else:
                self.ui(lambda: ok(result))

        make(conn).add_done_callback(done)

    def _failed(self, exc: Exception, resync: bool, fail: Callable[[Exception], None] | None) -> None:
        if fail:
            fail(exc)
        self._on_error(exc, resync)

    def _on_error(self, exc: Exception, resync: bool) -> None:
        if isinstance(exc, gfx_link.LinkError):
            return  # the disconnect handler reports it
        text = str(exc)
        self.lcd.flash(ERROR_TEXT.get(text, text.upper()[:36]))
        if resync:
            self._resync_state()

    # ---- connection ------------------------------------------------------------
    def _refresh_ports(self, prefer: str | None = None) -> None:
        ports = gfx_link.find_ports()
        self.port_box["values"] = ports
        if isinstance(prefer, str):
            self.port_var.set(prefer)
        elif self.port_var.get() not in ports:
            self.port_var.set(ports[0] if ports else "")

    def _toggle_connection(self) -> None:
        if self.conn:
            self._disconnect("Disconnected")
        else:
            self.connect()

    def connect(self) -> None:
        callbacks = dict(on_event=lambda line: self.ui(lambda: self._on_event(line)),
                         on_disconnect=lambda why: self.ui(lambda: self._on_link_lost(why)))
        target = SIM_LABEL if self.sim_var.get() else self.port_var.get().strip()
        try:
            self.conn = (Connection.open_sim(**callbacks) if self.sim_var.get()
                         else Connection.open_serial(target, **callbacks))
        except gfx_link.LinkError as exc:
            self.state_var.set(f"Cannot open {target}: {exc}")
            return
        self.state_var.set(f"Connected: {target}")
        self.connect_btn.configure(text="Disconnect")
        self.ask(lambda c: c.algos(), self._on_algos)

    def _on_algos(self, algos: list[Algorithm]) -> None:
        self.algos = {a.id: a for a in algos}
        self._resync_state()

    def _resync_state(self) -> None:
        self.ask(lambda c: c.state(), self._apply_state)
        self.ask(lambda c: c.preset_list(), self._apply_presets)

    def _disconnect(self, message: str) -> None:
        conn, self.conn = self.conn, None
        if conn:
            conn.close()
        self.status_busy = False
        self.loop = None
        self.connect_btn.configure(text="Connect")
        self.state_var.set(message)
        self._show_idle_gauges()
        self.refresh_all()

    def _on_link_lost(self, reason: str) -> None:
        if self.conn is not None:
            self._disconnect(f"Connection lost ({reason})")

    # ---- model -> widgets ------------------------------------------------------------
    def _apply_state(self, chain: ChainState) -> None:
        self.chain = chain
        self.selected = min(self.selected, len(chain.slots) - 1)
        self.refresh_all()

    def _apply_presets(self, presets: dict[int, str]) -> None:
        self.presets = presets
        self.preset_list.delete(0, "end")
        for i in range(16):
            self.preset_list.insert("end", f"{i + 1:02d}  {presets.get(i, '') if i in presets else '---'}")
        self._mark_current_preset()

    def _mark_current_preset(self) -> None:
        self.preset_list.selection_clear(0, "end")
        if self.chain and self.chain.preset_index >= 0:
            self.preset_list.selection_set(self.chain.preset_index)

    def _algo_of(self, slot: int) -> Algorithm | None:
        if not self.chain:
            return None
        return self.algos.get(self.chain.slots[slot].algo_id)

    def refresh_all(self) -> None:
        self.refresh_strip()
        self.refresh_knobs()
        self.refresh_lcd()
        self.refresh_globals()
        self.refresh_loop()
        self._mark_current_preset()

    def refresh_strip(self) -> None:
        items = []
        for n in range(len(self.chain.slots) if self.chain else 0):
            algo = self._algo_of(n)
            items.append((algo.name if algo else "?", algo.category if algo else "None",
                          self.chain.slots[n].on))
        self.strip.set_data(items, self.selected)

    def refresh_knobs(self) -> None:
        algo = self._algo_of(self.selected)
        slot = self.chain.slots[self.selected] if self.chain else None
        color = category_color(algo.category) if algo else ACCENT
        for i, knob in enumerate(self.knobs):
            if algo and i < len(algo.params):
                knob.bind_param(algo.params[i], slot.values.get(i, algo.params[i].default), color)
            else:
                knob.bind_param(None)

    def refresh_globals(self) -> None:
        for name, knob in self.global_knobs.items():
            if self.chain and name in self.chain.globals:
                desc = self.chain.globals[name]
                knob.bind_param(ParamDesc(name.split(".")[0], desc.min, desc.max, desc.step, desc.default),
                                self.chain.global_values[name])
            else:
                knob.bind_param(None)
        bypass = bool(self.chain and self.chain.global_values.get("bypass"))
        self.bypass_sw.set_led(AMBER if bypass else None)
        self.play_sw.set_led(GREEN if self.play else None)

    def refresh_lcd(self) -> None:
        chain = self.chain
        if not chain or not self.conn:
            self.lcd.set_lines("GFX", "not connected", "")
            return
        first = (f"P{chain.preset_index + 1:02d} {chain.preset_name}" if chain.preset_index >= 0
                 else "P-- (unsaved)")
        algo = self._algo_of(self.selected)
        second = f"{self.selected + 1}: {algo.name}  [{algo.category}]" if algo else ""
        third = "Turn a knob"
        if self.last_param:
            desc, value = self.last_param
            third = f"{desc.title:<10}{desc.format(value)}"
        self.lcd.set_lines(first, second, third)

    def _looper_slot(self) -> int | None:
        for n in range(len(self.chain.slots) if self.chain else 0):
            algo = self._algo_of(n)
            if algo and algo.category == "Looper":
                return n
        return None

    def refresh_loop(self) -> None:
        present = self._looper_slot() is not None and self.conn is not None
        for sw in self.loop_sw.values():
            sw.set_enabled(present)
        self.loop_hint.configure(text="" if present else "Add a Looper algorithm to a slot to enable")
        state = self.loop.state if self.loop and present else None
        self.loop_sw["REC"].set_led(RED if state == "recording" else None)
        self.loop_sw["PLAY"].set_led({"playing": GREEN, "overdub": AMBER}.get(state or ""))
        if self.loop and present:
            self.loop_bar.show(self.loop.state, self.loop.pos, self.loop.len, self.loop.max)
        else:
            self.loop_bar.show(None, 0, 0, 0)

    # ---- editing ------------------------------------------------------------------------
    def select_slot(self, n: int) -> None:
        if not self.chain or not 0 <= n < len(self.chain.slots):
            return
        self.selected, self.last_param = n, None
        self.refresh_strip()
        self.refresh_knobs()
        self.refresh_lcd()

    def _on_knob(self, idx: int, value: float) -> None:
        algo = self._algo_of(self.selected)
        if not (self.chain and algo):
            return
        self.chain.slots[self.selected].values[idx] = value
        self.last_param = (algo.params[idx], value)
        self.refresh_lcd()
        self.coalescer.submit(("V", self.selected, idx), value)

    def _on_global(self, name: str, value: float) -> None:
        if self.chain:
            self.chain.global_values[name] = value
            self.last_param = (ParamDesc(name.replace(".level", "").upper() + " LEVEL", 0, 1, 0.01, 0), value)
            self.refresh_lcd()
            self.coalescer.submit(("G", name), value)

    def _send_value(self, key: tuple, value: float) -> None:
        if key[0] == "V":
            self.ask(lambda c: c.set_value(key[1], key[2], value))
        else:
            self.ask(lambda c: c.set_global(key[1], value))

    def open_browser(self, slot: int) -> None:
        if self.chain and self.conn:
            self.select_slot(slot)
            AlgoBrowser(self, slot)

    def choose_algo(self, slot: int, algo_id: int) -> None:
        self.coalescer.flush_all()
        self.select_slot(slot)
        self.ask(lambda c: c.set_slot(slot, algo_id), lambda _v: self._resync_state())

    def toggle_slot(self, n: int) -> None:
        if not self.chain:
            return
        slot = self.chain.slots[n]
        slot.on = not slot.on
        self.refresh_strip()
        self.ask(lambda c: c.set_on(n, slot.on), resync_on_error=True)

    def move_slot(self, src: int, dst: int) -> None:
        if not self.chain or src == dst or not 0 <= dst < len(self.chain.slots):
            return
        self.coalescer.flush_all()
        self.chain.slots.insert(dst, self.chain.slots.pop(src))
        self.selected, self.last_param = dst, None
        self.refresh_all()
        self.ask(lambda c: c.move(src, dst), resync_on_error=True)

    def _defaults(self) -> None:
        self.ask(lambda c: c.request("DEFAULTS"), lambda _r: self._resync_state())

    # ---- footswitches --------------------------------------------------------------------
    def toggle_play(self) -> None:
        self.play = not self.play
        self.play_sw.set_led(GREEN if self.play else None)
        self.ask(lambda c: c.request("PLAY" if self.play else "STOP"))

    def toggle_bypass(self) -> None:
        if not self.chain:
            return
        value = 0.0 if self.chain.global_values.get("bypass") else 1.0
        self.chain.global_values["bypass"] = value
        self.refresh_globals()
        self.ask(lambda c: c.set_global("bypass", value))

    def loop_action(self, action: str) -> None:
        slot = self._looper_slot()
        if slot is not None:
            self.ask(lambda c: c.loop(slot, action), self._set_loop_state)

    def loop_play_dub(self) -> None:
        playing = self.loop and self.loop.state in ("playing", "overdub")
        self.loop_action("DUB" if playing else "PLAY")

    def _set_loop_state(self, state: str) -> None:
        if self.loop:
            self.loop.state = state
        self.refresh_loop()

    # ---- presets & files -------------------------------------------------------------------
    def _on_preset_click(self, _event: tk.Event) -> None:
        sel = self.preset_list.curselection()
        if sel and sel[0] in self.presets:
            self.load_preset(sel[0])
        else:
            self._mark_current_preset()
        self.root.after_idle(self.root.focus_set)

    def load_preset(self, index: int) -> None:
        self.coalescer.flush_all()
        self.ask(lambda c: c.preset_load(index), lambda _r: self._resync_state())

    def _ask_save(self) -> None:
        if self.chain:
            SaveDialog(self)

    def save_preset(self, index: int, name: str) -> None:
        self.coalescer.flush_all()
        self.lcd.flash("SAVING TO FLASH...", AMBER, 1500)
        self.ask(lambda c: c.preset_save(index, name), lambda _r: self._resync_state())

    def _delete_selected(self) -> None:
        sel = self.preset_list.curselection()
        index = sel[0] if sel else (self.chain.preset_index if self.chain else -1)
        if index in self.presets:
            self.delete_preset(index)

    def delete_preset(self, index: int) -> None:
        self.ask(lambda c: c.preset_delete(index), lambda _r: self._resync_state())

    def _export(self) -> None:
        path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("JSON", "*.json")])
        if path and self.chain:
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(gfx_link.export_chain(self.chain, self.algos), fh, indent=2)
            self.lcd.flash("EXPORTED", GREEN)

    def _import(self) -> None:
        path = filedialog.askopenfilename(filetypes=[("JSON", "*.json")])
        if path:
            self.import_chain(path)

    def import_chain(self, path: str) -> None:
        try:
            with open(path, encoding="utf-8") as fh:
                cmds = gfx_link.chain_commands(json.load(fh), self.algos)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            messagebox.showerror("Import failed", str(exc))
            return
        self._run_sequence(cmds)

    def _run_sequence(self, cmds: list[str]) -> None:
        if not cmds:
            self._resync_state()
            return
        self.ask(lambda c: c.request(cmds[0]), lambda _r: self._run_sequence(cmds[1:]),
                  resync_on_error=True)

    # ---- status & events ---------------------------------------------------------------------
    def _poll_status(self) -> None:
        if self.conn and not self.status_busy:
            self.status_busy = True
            conn = self.conn
            conn.status().add_done_callback(lambda f: self.ui(lambda: self._status_done(f)))
        self.root.after(STATUS_INTERVAL_MS, self._poll_status)

    def _status_done(self, future: Future) -> None:
        self.status_busy = False
        if future.exception() is None:
            self._show_status(future.result())

    def _show_idle_gauges(self) -> None:
        self.cpu_bar.set(0, "CPU --")
        self.mem_bar.set(0, "MEM --")
        self.cost_bar.set(0, "COST --")
        self.meter.set_level(0)

    def _show_status(self, st: Status) -> None:
        self.play = st.play
        if self.chain:
            self.chain.global_values["bypass"] = float(st.bypass)
        self.meter.set_level(st.peak)
        self.cpu_bar.set(st.cpu / 100, f"CPU {st.cpu:.0f}%", RED if st.cpu > 85 else AMBER if st.cpu > 60 else GREEN)
        self.mem_bar.set(st.mem_used / st.mem_total, f"MEM {st.mem_used / 1024:.0f}/{st.mem_total / 1024:.0f} KB",
                         RED if st.mem_used > 0.95 * st.mem_total else ACCENT)
        self.cost_bar.set(st.cost_used / st.cost_budget, f"COST {st.cost_used:g}/{st.cost_budget:g}",
                          RED if st.cost_used > 0.9 * st.cost_budget else ACCENT)
        self.loop = st.loop
        self.refresh_globals()
        self.refresh_loop()

    def _on_event(self, line: str) -> None:
        parts = line.split()
        if parts[:2] == ["EVT", "PLAY"] and len(parts) == 3:
            self.play = parts[2] == "1"
            self.refresh_globals()
        elif parts[:2] == ["EVT", "LOOP"] and len(parts) == 4 and self.loop and self.loop.slot == int(parts[2]):
            self._set_loop_state(parts[3])

    def _on_key(self, event: tk.Event) -> None:
        try:
            focus = self.root.focus_get()
        except KeyError:
            focus = None
        if isinstance(focus, (tk.Entry, ttk.Entry, ttk.Combobox)):
            return
        if event.char in ("1", "2", "3", "4"):
            self.select_slot(int(event.char) - 1)
        elif event.keysym == "space":
            self.toggle_play()
        elif event.keysym.lower() == "b":
            self.toggle_bypass()
        elif event.keysym.lower() == "r":
            self.loop_action("REC")
        elif event.keysym.lower() == "s":
            self.open_scope()

    def _quit(self) -> None:
        if self.conn:
            self.conn.close()
        self.root.destroy()

    # ---- self test ---------------------------------------------------------------------------
    def run_selftest(self) -> None:
        """Scripted smoke test against the simulator; exits via _selftest_finish."""
        steps = [(100, self.connect), (900, lambda: (self.select_slot(2), self.choose_algo(2, 2))),
                 (1300, lambda: self.knobs[0].user_set(500)), (1600, lambda: self.move_slot(1, 2)),
                 (1900, lambda: self.choose_algo(3, 27)), (2300, lambda: self.loop_action("REC")),
                 (2500, lambda: self.loop_action("REC")), (2800, lambda: self.save_preset(3, "Selftest")),
                 (3000, self.toggle_play), (3200, self.open_scope),
                 (3900, lambda: self.scope.set_timebase("20 ms")), (4300, lambda: self.scope.set_scale("dB")),
                 (4800, self._selftest_close_scope), (5000, self._selftest_finish)]
        for delay, fn in steps:
            self.root.after(delay, fn)

    def _selftest_close_scope(self) -> None:
        self.scope_frames = self.scope.frames
        self.scope.close()

    def _selftest_finish(self) -> None:
        sim = self.conn.transport if self.conn else None
        checks = {
            "no tk errors": not self.errors,
            "tape echo placed": bool(sim) and 2 in [s.algo_id for s in sim.slots],
            "knob value sent": bool(sim) and any(s.values.get(0) == 500 for s in sim.slots if s.algo_id == 2),
            "slots reordered": bool(sim) and [s.algo_id for s in sim.slots][:3] == [21, 2, 6],
            "looper playing": bool(sim) and any(s.loop.state == "playing" for s in sim.slots),
            "preset saved": bool(sim) and sim.presets[3] is not None and sim.preset_index == 3,
            "status polled": self.loop is not None,
            "scope frames": self.scope_frames > 0 and self.scope is None,
        }
        failed = [name for name, ok in checks.items() if not ok]
        print("selftest", "OK" if not failed else f"FAILED: {failed}")
        self._quit()
        self.exit_code = 1 if failed else 0

    exit_code = 0
    scope_frames = 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="serial port, e.g. COM5")
    ap.add_argument("--sim", action="store_true", help="use the built-in simulator")
    ap.add_argument("--selftest", action="store_true", help="run a scripted ~4 s smoke test")
    args = ap.parse_args(argv)
    root = tk.Tk()
    app = App(root, args.port, args.sim or args.selftest)
    if args.selftest:
        app.run_selftest()
    elif args.sim or args.port:
        root.after(100, app.connect)
    root.mainloop()
    return app.exit_code


if __name__ == "__main__":
    sys.exit(main())
