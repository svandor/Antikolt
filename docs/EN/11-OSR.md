# 11 — OSR or native window? (Q5 in detail)

The question: how the web engine's image gets into the browser window. There are two paths, and the difference is not theoretical —
it decides whether Linux (Wayland) works at all, and how much CPU scrolling consumes.

## 1. The essence in one paragraph

**Native window:** the engine paints into **its own little window**, and we "insert" that window into our
own window (as if we put a picture into a frame). Fast, hardware-accelerated, but requires a separate trick per platform,
and it is not reliable under Wayland.

**OSR (offscreen rendering):** the engine paints **into memory** (into a texture), and we output it to the screen
(like a video player does with film frames). A bit more expensive, but works everywhere, and the engine can
render even when it has no window of its own.

## 2. Native window embedding

**How:** the engine's window becomes a **child** of our window.

- Windows: `SetParent(motorHWND, aMiHWND)`, then tracking size/position (`MoveWindow`), and forwarding focus and
  input.
- Linux X11: `XReparentWindow` — the same principle.
- Linux Wayland: **has no reliable equivalent.** The `xdg-foreign` protocol could be used to embed a foreign surface,
  but support varies by compositor, and input/focus handling is fragile.

| Advantage | Disadvantage |
|---|---|
| Hardware acceleration "for free" (the engine paints directly to the screen) | Separate code per platform (Windows/X11) |
| No frame copying → low CPU, good battery | Not a viable path under Wayland |
| Video/WebGL/DRM works the usual way | Keeping the two windows in sync (resize, scrolling, DPI change, dragging between monitors) is fragile |
| Less own code (no compositing) | The "engine in a separate process" only works if window handoff also works (Windows/X11: yes) |

## 3. OSR (offscreen rendering)

**How:** the engine receives a buffer/texture, renders into it, and **signals that it is done** (a "paint" event).
The shell draws this texture onto its own surface, and forwards user input (mouse, keyboard, IME,
scrolling) **backwards** to the engine.

- Accelerated variant: shared GPU texture (Windows: D3D11 shared handle, Linux: dmabuf) — in this case the
  cost is small.
- Software variant: CPU copy — simple, but expensive (1080p @60 fps ≈ 500 MB/s of memory movement).

| Advantage | Disadvantage |
|---|---|
| **Platform-independent** (Windows, X11, Wayland alike) | Per-frame compositing: CPU/GPU overhead |
| The engine can render **invisibly** too → preview, background snapshot, "windowless engine" is also easy | Manual forwarding of input, IME, cursor, drag&drop, selection — many opportunities for bugs |
| Uniform multi-engine integration (every engine "provides frames") | **Accessibility**: the engine's own accessibility tree does not connect to Qt automatically — a bridge must be built (screen reader!) |
| The engine's process can be stopped at any time, the image remains (raster home page!) | Video/DRM/GPU acceleration sometimes requires a separate path |

## 4. Why native on Windows, OSR on Linux

Not a matter of principle, but a property of the platform:

| Platform | Primary path | Why |
|---|---|---|
| Windows 11 | native window | fast, stable, hardware-accelerated; the Wayland problem does not exist |
| Linux X11 | native window | the same as on Windows |
| Linux Wayland | **OSR** (or native + `xdg-foreign`, if the compositor supports it) | embedding is not reliable; OSR is the safe path |

The two live behind **the same abstraction** (`EngineSurface`), so the upper layers do not know which
one is running. The engine adapter's contract does not change.

## 5. What this means for you, as a user?

- **Nothing of it is visible.** The same UI, the same cards.
- On Windows, scrolling and video are fast the usual way.
- On Linux (Wayland), rendering consumes a bit more CPU — this is offset by the Power-saving profile and the
  media quality limit.
- The **preview images and the raster home page** work even without OSR (the engine can provide a screenshot on
  its own side), so the memory promise does not depend on this decision.

## 6. Risks (what needs to be tested in Phase 0)

1. **DPI change / dragging between monitors** with native embedding (the most common source of bugs).
2. **Focus and IME** (Hungarian accented input, composed characters) in both paths.
3. **Cursor and selection** with OSR (the cursor must be requested by the engine, not by Qt).
4. **Drag & drop** of files onto the page.
5. **Accessibility** with OSR: the screen-reader bridge (this is a separate milestone, it cannot be skipped).
6. **Video and DRM** (Netflix-like content) behavior in OSR.
7. **Latency** (input latency) with OSR: compositing can add 1–3 extra frames — it must be measured whether
   it is perceptible.

## 7. Decision (clarification of ADR-006)

- Windows 11 (v1): **native window embedding**.
- Linux (v0.3): **OSR** on Wayland, native on X11.
- The code is built **from day one** so that the integration mode is swappable; the OSR branch gets a minimal
  prototype in Phase 0 (one page, scrolling, click), so that the risk surfaces early.
- If OSR turns out to be unacceptably expensive in Phase 0, the Linux path slips, but the Windows product does not.

**In one sentence:** native embedding is the faster and simpler one, OSR is the more portable one — therefore
we use the first on Windows and the second on Linux, behind the same interface.
