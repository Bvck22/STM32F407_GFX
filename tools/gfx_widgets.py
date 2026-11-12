"""Canvas-drawn pedal widgets: knob, LCD, meters, footswitch, chain strip."""
from __future__ import annotations

import math
import tkinter as tk
from typing import Callable

from gfx_link import ParamDesc

BG = "#14161a"
PANEL = "#1e2127"
PANEL2 = "#2a2e36"
EDGE = "#3b414c"
TEXT = "#dde1e8"
DIM = "#8b929e"
ACCENT = "#f0b43c"
GREEN, AMBER, RED = "#4ade80", "#fbbf24", "#ef4444"
LCD_BG, LCD_FG, LCD_DIM = "#120d02", "#ffb43c", "#6b4c14"
CATEGORY_COLORS = {"Delay": "#3b82f6", "Modulation": "#a855f7", "Pitch": "#14b8a6",
                   "Reverb": "#22c55e", "Drive": "#f97316", "Looper": "#ef4444", "None": "#6b7280"}
FONT = "Segoe UI"
MONO = "Consolas"


def rounded_rect(cv: tk.Canvas, x0: float, y0: float, x1: float, y1: float, r: float, **kw) -> int:
    pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1,
           x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
    return cv.create_polygon(pts, smooth=True, **kw)


def category_color(category: str) -> str:
    return CATEGORY_COLORS.get(category, CATEGORY_COLORS["None"])


class Knob(tk.Canvas):
    """Rotary knob: vertical drag (shift = fine), wheel = step, double-click = default."""

    SWEEP = 270
    DRAG_PIXELS = 200

    def __init__(self, parent: tk.Misc, size: int = 92, bg: str = PANEL,
                 on_change: Callable[[float], None] | None = None) -> None:
        super().__init__(parent, width=size, height=size + 36, bg=bg, highlightthickness=0)
        self.size = size
        self.on_change = on_change or (lambda _v: None)
        self.desc: ParamDesc | None = None
        self.value = 0.0
        self.color = ACCENT
        self._active = False
        self._drag: tuple[int, float] | None = None
        self.bind("<ButtonPress-1>", self._press)
        self.bind("<Double-Button-1>", self._double)
        self.bind("<B1-Motion>", self._motion)
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<MouseWheel>", self._wheel)
        self.bind("<Enter>", lambda _e: self._set_active(True))
        self.bind("<Leave>", lambda _e: self._set_active(self._drag is not None))
        self._draw()

    def bind_param(self, desc: ParamDesc | None, value: float = 0.0, color: str = ACCENT) -> None:
        self.desc, self.value, self.color = desc, value, color
        self._draw()

    def set_value(self, value: float) -> None:
        """Update from outside without firing on_change."""
        self.value = value
        self._draw()

    def user_set(self, value: float) -> None:
        """Programmatic equivalent of the user turning the knob."""
        if self.desc is not None:
            self._apply(self.desc.quantize(value))

    # -- interaction ------------------------------------------------------
    def _frac(self) -> float:
        d = self.desc
        return 0.0 if d is None or d.max == d.min else (self.value - d.min) / (d.max - d.min)

    def _apply(self, value: float) -> None:
        if value != self.value:
            self.value = value
            self._draw()
            self.on_change(value)

    def _set_active(self, active: bool) -> None:
        self._active = active
        self._draw()

    def _press(self, event: tk.Event) -> None:
        self._drag = (event.y, self._frac())
        self._set_active(True)

    def _double(self, event: tk.Event) -> None:
        if self.desc is not None:
            self._apply(self.desc.default)
        self._press(event)

    def _motion(self, event: tk.Event) -> None:
        if self.desc is None or self._drag is None:
            return
        y0, frac0 = self._drag
        fine = 4 if event.state & 0x1 else 1
        frac = min(1.0, max(0.0, frac0 + (y0 - event.y) / (self.DRAG_PIXELS * fine)))
        self._apply(self.desc.quantize(self.desc.min + frac * (self.desc.max - self.desc.min)))

    def _release(self, _event: tk.Event) -> None:
        self._drag = None
        self._set_active(False)

    def _wheel(self, event: tk.Event) -> None:
        if self.desc is not None:
            self._apply(self.desc.quantize(self.value + (1 if event.delta > 0 else -1) * self.desc.step))

    # -- drawing --------------------------------------------------------------
    def _draw(self) -> None:
        self.delete("all")
        s = self.size
        cx = cy = s / 2
        radius = s * 0.43
        live = self.desc is not None
        self.create_arc(cx - radius, cy - radius, cx + radius, cy + radius, start=225,
                        extent=-self.SWEEP, style="arc", width=6, outline="#2c3038")
        frac = self._frac()
        if live and frac > 0:
            self.create_arc(cx - radius, cy - radius, cx + radius, cy + radius, start=225,
                            extent=-self.SWEEP * frac, style="arc", width=6, outline=self.color)
        body = radius * 0.74
        self.create_oval(cx - body, cy - body, cx + body, cy + body, fill="#2d3139", outline="#4a505c", width=2)
        self.create_oval(cx - body * 0.8, cy - body * 0.8, cx + body * 0.8, cy + body * 0.8,
                         fill="#262a31", outline="")
        if live:
            ang = math.radians(225 - self.SWEEP * frac)
            self.create_line(cx + body * 0.25 * math.cos(ang), cy - body * 0.25 * math.sin(ang),
                             cx + body * 0.92 * math.cos(ang), cy - body * 0.92 * math.sin(ang),
                             fill=TEXT, width=3, capstyle="round")
        self._draw_labels(cx, s)

    def _draw_labels(self, cx: float, s: int) -> None:
        d = self.desc
        self.create_text(cx, s + 6, text=d.title if d else "-", fill=DIM if d is None else TEXT,
                         font=(FONT, 8, "bold"))
        if d is not None:
            shown = self._active or d.is_selector
            self.create_text(cx, s + 23, text=d.format(self.value), font=(MONO, 9, "bold"),
                             fill=self.color if shown else DIM)


class LCD(tk.Canvas):
    """Three-line amber display with a transient message on the last line."""

    def __init__(self, parent: tk.Misc, width: int = 470, height: int = 108) -> None:
        super().__init__(parent, width=width, height=height, bg=PANEL, highlightthickness=0)
        self.w, self.h = width, height
        self.lines = ["", "", ""]
        self._flash_id: str | None = None
        self._flash: tuple[str, str] | None = None
        self._draw()

    def set_lines(self, *lines: str) -> None:
        self.lines = list(lines)
        self._draw()

    def flash(self, message: str, color: str = RED, ms: int = 2500) -> None:
        self._flash = (message, color)
        if self._flash_id:
            self.after_cancel(self._flash_id)
        self._flash_id = self.after(ms, self._end_flash)
        self._draw()

    def _end_flash(self) -> None:
        self._flash, self._flash_id = None, None
        self._draw()

    def _draw(self) -> None:
        self.delete("all")
        rounded_rect(self, 2, 2, self.w - 2, self.h - 2, 10, fill="#0a0a0a", outline="#4a4f5a", width=2)
        rounded_rect(self, 8, 8, self.w - 8, self.h - 8, 6, fill=LCD_BG, outline="#2a1d05")
        for y in range(12, self.h - 10, 3):
            self.create_line(10, y, self.w - 10, y, fill="#1a1204")
        sizes = (15, 11, 12)
        for i, text in enumerate(self.lines[:3]):
            color = LCD_FG if i != 1 else "#d9962a"
            if i == 2 and self._flash:
                text, color = self._flash
            y = 26 + i * 27
            self.create_text(21, y + 1, text=text, anchor="w", fill=LCD_DIM, font=(MONO, sizes[i], "bold"))
            self.create_text(20, y, text=text, anchor="w", fill=color, font=(MONO, sizes[i], "bold"))


class PeakMeter(tk.Canvas):
    """Vertical LED-style bar; red segment at clip level."""

    SEGMENTS = 14

    def __init__(self, parent: tk.Misc, height: int = 112) -> None:
        super().__init__(parent, width=30, height=height, bg=PANEL, highlightthickness=0)
        self.h = height
        self.level = 0.0
        self.set_level(0.0)

    def set_level(self, peak: float) -> None:
        self.level = max(peak, self.level - 0.15)  # fall back between 5 Hz updates
        self.delete("all")
        seg_h = (self.h - 4) / self.SEGMENTS
        for i in range(self.SEGMENTS):
            lit = self.level >= (i + 0.5) / self.SEGMENTS or (i == self.SEGMENTS - 1 and self.level >= 0.99)
            base = RED if i >= self.SEGMENTS - 1 else AMBER if i >= self.SEGMENTS - 4 else GREEN
            y1 = self.h - 2 - i * seg_h
            self.create_rectangle(6, y1 - seg_h + 2, 24, y1, fill=base if lit else "#25282e", outline="")


class HBar(tk.Canvas):
    """Small horizontal gauge with overlaid text."""

    def __init__(self, parent: tk.Misc, width: int = 150, height: int = 18, bg: str = BG) -> None:
        super().__init__(parent, width=width, height=height, bg=bg, highlightthickness=0)
        self.w, self.h = width, height
        self.set(0.0, "")

    def set(self, frac: float, text: str, color: str = GREEN) -> None:
        self.delete("all")
        rounded_rect(self, 0, 0, self.w, self.h, 5, fill="#262a31", outline=EDGE)
        frac = min(1.0, max(0.0, frac))
        if frac > 0:
            rounded_rect(self, 1, 1, max(8, 1 + (self.w - 2) * frac), self.h - 1, 4, fill=color, outline="")
        self.create_text(self.w / 2, self.h / 2, text=text, fill=TEXT, font=(FONT, 8, "bold"))


STATE_COLORS = {"recording": RED, "playing": GREEN, "overdub": AMBER, "stopped": DIM, "empty": DIM}


class LoopBar(HBar):
    def show(self, state: str | None, pos: int, length: int, cap: int, rate: int = 48000) -> None:
        if state is None:
            self.set(0.0, "no looper")
            return
        recording = state == "recording"
        total = cap if recording else length
        text = f"{state.upper()}  {pos / rate:.1f} / {total / rate:.1f} s  (max {cap / rate:.1f})"
        self.set(pos / total if total else 0.0, text, STATE_COLORS.get(state, DIM))


class Footswitch(tk.Canvas):
    """Round foot button with a status LED above and a caption below."""

    def __init__(self, parent: tk.Misc, text: str, command: Callable[[], None],
                 size: int = 78, bg: str = PANEL) -> None:
        super().__init__(parent, width=size, height=size + 30, bg=bg, highlightthickness=0)
        self.size, self.text, self.command = size, text, command
        self.led: str | None = None
        self.enabled = True
        self._down = False
        self.bind("<ButtonPress-1>", lambda _e: self._set_down(True))
        self.bind("<ButtonRelease-1>", self._release)
        self._draw()

    def set_led(self, color: str | None) -> None:
        self.led = color
        self._draw()

    def set_text(self, text: str) -> None:
        self.text = text
        self._draw()

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
        self._draw()

    def _set_down(self, down: bool) -> None:
        self._down = down and self.enabled
        self._draw()

    def _release(self, event: tk.Event) -> None:
        was_down = self._down
        self._set_down(False)
        inside = 0 <= event.x <= self.size and 0 <= event.y <= self.size + 30
        if was_down and inside:
            self.command()

    def _draw(self) -> None:
        self.delete("all")
        cx, r = self.size / 2, self.size * 0.33
        cy = 28 + r
        if self.led and self.enabled:
            self.create_oval(cx - 9, 3, cx + 9, 21, fill="", outline=self.led, width=1)
        self.create_oval(cx - 5, 7, cx + 5, 17, fill=self.led if (self.led and self.enabled) else "#33262a",
                         outline="#000")
        top = "#7d838f" if self.enabled else "#4a4e56"
        self.create_oval(cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4, fill="#0c0d10", outline=EDGE, width=2)
        off = 2 if self._down else 0
        self.create_oval(cx - r, cy - r + off, cx + r, cy + r + off,
                         fill="#555a64" if self._down else top, outline="#a4aab6" if self.enabled else "#3d4048", width=2)
        self.create_oval(cx - r * 0.55, cy - r * 0.55 + off, cx + r * 0.55, cy + r * 0.55 + off,
                         fill="#6a707c" if self.enabled else "#42454c", outline="")
        self.create_text(cx, self.size + 21, text=self.text, fill=TEXT if self.enabled else "#5a5f69",
                         font=(FONT, 8, "bold"))


class ChainStrip(tk.Canvas):
    """IN -> 4 slot modules -> OUT. Click selects, drag reorders, LED toggles."""

    HEIGHT = 124
    GAP = 26
    END_W = 40
    DRAG_THRESHOLD = 6

    def __init__(self, parent: tk.Misc, select: Callable[[int], None], toggle: Callable[[int], None],
                 move: Callable[[int, int], None], browse: Callable[[int], None]) -> None:
        super().__init__(parent, height=self.HEIGHT, bg=BG, highlightthickness=0)
        self._cb = {"select": select, "toggle": toggle, "move": move, "browse": browse}
        self.items: list[tuple[str, str, bool]] = []
        self.selected = 0
        self._regions: list[tuple[tuple[float, float, float, float], str, int]] = []
        self._drag: dict | None = None
        self.bind("<Configure>", lambda _e: self._draw())
        self.bind("<ButtonPress-1>", self._press)
        self.bind("<B1-Motion>", self._motion)
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<Button-3>", self._right_click)

    def set_data(self, items: list[tuple[str, str, bool]], selected: int) -> None:
        """items: (algorithm name, category, on) per slot."""
        self.items, self.selected = items, selected
        self._draw()

    def module_box(self, n: int) -> tuple[float, float, float, float]:
        width = max(self.winfo_width(), 400)
        mw = (width - 2 * self.END_W - 2 * 12 - 5 * self.GAP) / 4
        x0 = 12 + self.END_W + self.GAP + n * (mw + self.GAP)
        return x0, 10, x0 + mw, self.HEIGHT - 10

    def _draw(self) -> None:
        self.delete("all")
        self._regions = []
        mid = self.HEIGHT / 2
        width = max(self.winfo_width(), 400)
        for label, x0 in (("IN", 12), ("OUT", width - 12 - self.END_W)):
            rounded_rect(self, x0, mid - 18, x0 + self.END_W, mid + 18, 8, fill=PANEL2, outline=EDGE)
            self.create_text(x0 + self.END_W / 2, mid, text=label, fill=TEXT, font=(FONT, 9, "bold"))
        x_prev = 12 + self.END_W
        for n in range(len(self.items) + 1):
            x_next = self.module_box(n)[0] if n < len(self.items) else width - 12 - self.END_W
            self.create_line(x_prev + 3, mid, x_next - 3, mid, fill=DIM, width=2, arrow="last",
                             arrowshape=(9, 11, 4))
            x_prev = self.module_box(n)[2] if n < len(self.items) else x_prev
        for n in range(len(self.items)):
            self._draw_module(n)

    def _draw_module(self, n: int) -> None:
        name, category, on = self.items[n]
        x0, y0, x1, y1 = self.module_box(n)
        color = category_color(category)
        empty = category == "None"
        selected = n == self.selected
        rounded_rect(self, x0, y0, x1, y1, 10, fill=PANEL if on or empty else "#1a1c21",
                     outline=ACCENT if selected else EDGE, width=3 if selected else 1,
                     dash=(4, 3) if empty else ())
        self._regions.append(((x0, y0, x1, y1), "body", n))
        if not empty:
            self.create_rectangle(x0 + 10, y0 + 4, x1 - 10, y0 + 9, fill=color if on else "#3a3d44", outline="")
        self.create_text(x0 + 16, y0 + 24, text=str(n + 1), fill=DIM, font=(FONT, 11, "bold"))
        self.create_text((x0 + x1) / 2, y0 + 48, text=name, width=x1 - x0 - 16,
                         fill=TEXT if on or empty else DIM, font=(FONT, 11, "bold"), justify="center")
        self.create_text((x0 + x1) / 2, y0 + 66, text="" if empty else category, fill=color if not empty else DIM,
                         font=(FONT, 8))
        self._draw_led(n, x1 - 18, y0 + 24, on, empty)
        self._draw_buttons(n, x0, y1, x1)

    def _draw_led(self, n: int, x: float, y: float, on: bool, empty: bool) -> None:
        if empty:
            return
        self.create_oval(x - 7, y - 7, x + 7, y + 7, fill=GREEN if on else "#26332b", outline="#000", width=1)
        self._regions.append(((x - 10, y - 10, x + 10, y + 10), "led", n))

    def _draw_buttons(self, n: int, x0: float, y1: float, x1: float) -> None:
        specs = [("left", "◀", x0 + 8, x0 + 30), ("browse", "▾  ALGO", x0 + 36, x1 - 36),
                 ("right", "▶", x1 - 30, x1 - 8)]
        for kind, text, bx0, bx1 in specs:
            if (kind == "left" and n == 0) or (kind == "right" and n == len(self.items) - 1):
                continue
            rounded_rect(self, bx0, y1 - 28, bx1, y1 - 8, 5, fill=PANEL2, outline=EDGE)
            self.create_text((bx0 + bx1) / 2, y1 - 18, text=text, fill=TEXT, font=(FONT, 8, "bold"))
            self._regions.append(((bx0, y1 - 28, bx1, y1 - 8), kind, n))

    def _hit(self, x: float, y: float) -> tuple[str, int] | None:
        for (x0, y0, x1, y1), kind, n in reversed(self._regions):
            if x0 <= x <= x1 and y0 <= y <= y1:
                return kind, n
        return None

    def _target(self, x: float) -> int:
        centers = [(self.module_box(n)[0] + self.module_box(n)[2]) / 2 for n in range(len(self.items))]
        return min(range(len(centers)), key=lambda i: abs(centers[i] - x))

    def _press(self, event: tk.Event) -> None:
        hit = self._hit(event.x, event.y)
        if hit is None:
            return
        kind, n = hit
        if kind == "led":
            self._cb["toggle"](n)
        elif kind == "browse":
            self._cb["browse"](n)
        elif kind in ("left", "right"):
            self._cb["move"](n, n - 1 if kind == "left" else n + 1)
        else:
            self._cb["select"](n)
            self._drag = {"slot": n, "x": event.x, "y": event.y, "active": False}

    def _motion(self, event: tk.Event) -> None:
        d = self._drag
        if d is None:
            return
        if not d["active"] and math.hypot(event.x - d["x"], event.y - d["y"]) < self.DRAG_THRESHOLD:
            return
        d["active"] = True
        d["target"] = self._target(event.x)
        self.delete("ghost")
        x0, y0, x1, y1 = self.module_box(d["target"])
        self.create_rectangle(x0 - 3, y0 - 3, x1 + 3, y1 + 3, outline=GREEN, width=2, dash=(5, 3), tags="ghost")
        w = self.module_box(0)[2] - self.module_box(0)[0]
        name = self.items[d["slot"]][0]
        self.create_rectangle(event.x - w / 2, y0 + 20, event.x + w / 2, y1 - 20, outline=ACCENT, width=2,
                              tags="ghost")
        self.create_text(event.x, (y0 + y1) / 2, text=name, fill=ACCENT, font=(FONT, 10, "bold"), tags="ghost")

    def _release(self, _event: tk.Event) -> None:
        d, self._drag = self._drag, None
        self.delete("ghost")
        if d and d["active"] and d["target"] != d["slot"]:
            self._cb["move"](d["slot"], d["target"])

    def _right_click(self, event: tk.Event) -> None:
        hit = self._hit(event.x, event.y)
        if hit:
            self._cb["browse"](hit[1])
