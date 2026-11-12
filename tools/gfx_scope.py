"""Oscilloscope window: continuous SCOPE arm -> SCOPEDATA poll -> draw loop."""
from __future__ import annotations

import math
import tkinter as tk
from tkinter import ttk
from typing import TYPE_CHECKING

from gfx_link import SCOPE_POINTS, ScopeFrame
from gfx_widgets import BG, DIM, EDGE, PANEL, TEXT

if TYPE_CHECKING:
    from gfx_gui import App

TIMEBASES = {"5 ms": 5, "10 ms": 10, "20 ms": 20, "50 ms": 50, "100 ms": 100, "200 ms": 200,
             "500 ms": 500, "1 s": 1000, "2 s": 2000, "5 s": 5000}
ZOOMS = ("x1", "x2", "x4", "x8")
CHANNELS = (("in", "IN", "#aeb4c0"), ("l", "OUT L", "#22d3ee"), ("r", "OUT R", "#e879f9"))
POLL_MS = 30
DB_FLOOR = -60.0
H_DIVS, V_DIVS = 10, 8
MARGIN_L, MARGIN = 54, 12


def to_dbfs(amplitude: float) -> float:
    return DB_FLOOR if amplitude <= 0 else max(DB_FLOOR, 20 * math.log10(amplitude))


class ScopeWindow(tk.Toplevel):
    def __init__(self, app: "App") -> None:
        super().__init__(app.root, bg=BG)
        self.app = app
        self.title("GFX Scope")
        self.geometry("860x560")
        self.minsize(520, 360)
        self.alive = True
        self.running = True
        self.single_shot = False
        self.frames = 0
        self.frame: ScopeFrame | None = None
        self.frame_window = 0
        self._gen = 0
        self.timebase = tk.StringVar(value="100 ms")
        self.trigger = tk.BooleanVar(value=True)
        self.scale = tk.StringVar(value="Linear")
        self.zoom = tk.StringVar(value="x1")
        self.shown = {key: tk.BooleanVar(value=True) for key, _n, _c in CHANNELS}
        self.status = tk.StringVar(value="waiting for data...")
        self._build()
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.bind("<Escape>", lambda _e: self.close())
        self._start()

    # ---- layout ----------------------------------------------------------
    def _build(self) -> None:
        bar = tk.Frame(self, bg=BG)
        bar.pack(fill="x", padx=8, pady=(8, 4))
        self._combo(bar, "Time/window", self.timebase, list(TIMEBASES), self._on_timebase)
        ttk.Checkbutton(bar, text="Trigger", variable=self.trigger, takefocus=False,
                        command=self._restart_if_running).pack(side="left", padx=8)
        self._combo(bar, "Scale", self.scale, ["Linear", "dB"], self._on_scale, width=7)
        self.zoom_box = self._combo(bar, "Zoom", self.zoom, list(ZOOMS), lambda: self._redraw(), width=4)
        self.run_btn = ttk.Button(bar, text="Freeze", takefocus=False, command=self.toggle_run)
        self.run_btn.pack(side="left", padx=(12, 2))
        ttk.Button(bar, text="Single", takefocus=False, command=self.single).pack(side="left", padx=2)
        for key, name, color in CHANNELS:
            tk.Checkbutton(bar, text=name, variable=self.shown[key], command=self._redraw, fg=color, bg=BG,
                           selectcolor=PANEL, activebackground=BG, activeforeground=color,
                           font=("Segoe UI", 9, "bold"), takefocus=False).pack(side="right", padx=4)
        self.canvas = tk.Canvas(self, bg="#07100c", highlightthickness=1, highlightbackground=EDGE)
        self.canvas.pack(fill="both", expand=True, padx=8)
        self.canvas.bind("<Configure>", lambda _e: self._redraw())
        tk.Label(self, textvariable=self.status, bg=BG, fg=TEXT, anchor="w",
                 font=("Consolas", 10)).pack(fill="x", padx=10, pady=6)

    def _combo(self, parent: tk.Misc, label: str, var: tk.StringVar, values: list[str], on_select,
               width: int = 8) -> ttk.Combobox:
        ttk.Label(parent, text=label).pack(side="left", padx=(8, 3))
        box = ttk.Combobox(parent, textvariable=var, values=values, state="readonly", width=width,
                           takefocus=False)
        box.pack(side="left")
        box.bind("<<ComboboxSelected>>", lambda _e: on_select())
        return box

    # ---- acquisition loop ------------------------------------------------------
    def _start(self) -> None:
        """(Re)arm; any reply belonging to an older generation is ignored."""
        self._gen += 1
        gen, window = self._gen, TIMEBASES[self.timebase.get()]
        self.frame_window = window
        self.app.ask(lambda c: c.scope_arm(window, self.trigger.get()), lambda _d: self._poll(gen),
                     fail=lambda exc: self._failed(gen, exc))

    def _poll(self, gen: int) -> None:
        if self.alive and gen == self._gen:
            self.app.ask(lambda c: c.scope_data(), lambda frame: self._got(gen, frame),
                         fail=lambda exc: self._failed(gen, exc))

    def _got(self, gen: int, frame: ScopeFrame | None) -> None:
        if not self.alive or gen != self._gen:
            return
        if frame is None:
            self.after(POLL_MS, self._poll, gen)
            return
        self.frames += 1
        self.frame = frame
        self._redraw()
        if self.single_shot:
            self.single_shot = self.running = False
            self.run_btn.configure(text="Run")
        elif self.running:
            self._start()

    def _failed(self, gen: int, exc: Exception) -> None:
        if self.alive and gen == self._gen:
            self.running = False
            self.run_btn.configure(text="Run")
            self.status.set(f"stopped: {exc}")

    # ---- controls -----------------------------------------------------------------
    def _restart_if_running(self) -> None:
        if self.running:
            self._start()

    def _on_timebase(self) -> None:
        self._restart_if_running()
        self._redraw()

    def _on_scale(self) -> None:
        self.zoom_box.configure(state="readonly" if self.scale.get() == "Linear" else "disabled")
        self._redraw()

    def set_timebase(self, label: str) -> None:
        self.timebase.set(label)
        self._on_timebase()

    def set_scale(self, name: str) -> None:
        self.scale.set(name)
        self._on_scale()

    def toggle_run(self) -> None:
        self.running = not self.running
        self.run_btn.configure(text="Freeze" if self.running else "Run")
        if self.running:
            self._start()
        else:
            self._gen += 1  # drop whatever capture is in flight

    def single(self) -> None:
        self.single_shot = self.running = True
        self.run_btn.configure(text="Freeze")
        self._start()

    def close(self) -> None:
        self.alive = False
        self._gen += 1
        if self.app.scope is self:
            self.app.scope = None
        self.destroy()

    # ---- drawing ----------------------------------------------------------------------
    def _redraw(self) -> None:
        cv = self.canvas
        cv.delete("all")
        w, h = max(cv.winfo_width(), 200), max(cv.winfo_height(), 120)
        box = (MARGIN_L, MARGIN, w - MARGIN, h - MARGIN)
        self._graticule(box)
        if self.frame is None:
            return
        for key, _name, color in CHANNELS:
            if self.shown[key].get():
                self._trace(self.frame, key, color, box)
        self._update_status()

    def _graticule(self, box: tuple[float, float, float, float]) -> None:
        x0, y0, x1, y1 = box
        cv = self.canvas
        for i in range(H_DIVS + 1):
            x = x0 + (x1 - x0) * i / H_DIVS
            cv.create_line(x, y0, x, y1, fill="#1d3a2c" if i not in (0, H_DIVS) else "#2f6a4a")
        for j in range(V_DIVS + 1):
            y = y0 + (y1 - y0) * j / V_DIVS
            cv.create_line(x0, y, x1, y, fill="#2f6a4a" if j == V_DIVS // 2 else "#1d3a2c")
            cv.create_text(x0 - 6, y, text=self._axis_label((V_DIVS / 2 - j) / (V_DIVS / 2)), anchor="e",
                           fill=DIM, font=("Consolas", 8))
        window = self.frame_window
        per_div = window / H_DIVS
        text = f"{per_div:g} ms/div" if per_div < 1000 else f"{per_div / 1000:g} s/div"
        cv.create_text(x1 - 4, y0 + 10, text=text, anchor="e", fill="#7bd9a4", font=("Consolas", 10, "bold"))

    def _axis_label(self, level: float) -> str:
        if self.scale.get() == "dB":
            return f"{(abs(level) - 1) * -DB_FLOOR:.0f} dB"
        return f"{level / int(self.zoom.get()[1:]):g}"

    def _trace(self, frame: ScopeFrame, key: str, color: str, box: tuple[float, float, float, float]) -> None:
        x0, y0, x1, y1 = box
        mid, half = (y0 + y1) / 2, (y1 - y0) / 2
        db = self.scale.get() == "dB"
        zoom = 1 if db else int(self.zoom.get()[1:])
        points = frame.channels[key]
        xs = [x0 + (x1 - x0) * i / (len(points) - 1) for i in range(len(points))]
        if db:
            tops = [mid - half * (1 - to_dbfs(max(abs(lo), abs(hi))) / DB_FLOOR) for lo, hi in points]
            lows = [2 * mid - y for y in tops]
        else:
            tops = [max(y0, min(y1, mid - hi * zoom * half)) for _lo, hi in points]
            lows = [max(y0, min(y1, mid - lo * zoom * half)) for lo, _hi in points]
        if frame.decim == 1 and not db:
            self.canvas.create_line(*[c for xy in zip(xs, lows) for c in xy], fill=color, width=1.5)
            return
        ring = [c for xy in zip(xs, tops) for c in xy] + [c for xy in zip(reversed(xs), reversed(lows)) for c in xy]
        self.canvas.create_polygon(*ring, fill=color, stipple="gray50", outline="")
        self.canvas.create_line(*[c for xy in zip(xs, tops) for c in xy], fill=color)
        self.canvas.create_line(*[c for xy in zip(xs, lows) for c in xy], fill=color)

    def _update_status(self) -> None:
        frame = self.frame
        peaks = []
        for key, name, _c in CHANNELS:
            peak = max((max(abs(lo), abs(hi)) for lo, hi in frame.channels[key]), default=0.0)
            peaks.append(f"{name} {'-inf' if peak <= 0 else f'{20 * math.log10(peak):.1f}'}")
        mode = "TRIG" if frame.triggered else "FREE-RUN"
        self.status.set(f"{mode:8}  decim {frame.decim}  |  peak dBFS:  " + "   ".join(peaks))
