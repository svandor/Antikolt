# 09 — Target hardware and device profiles

The measurements and the budget controller are based on the actual target machine. This document records the hardware we
measured, and what design decisions it entails.

## 1. The target machine (VECTOR) — measured data

Measurement: 2026-11, from a DSH session running on the machine, based on registry + process list.

| Component | Value |
|---|---|
| Host | `VECTOR` (laptop) |
| CPU | **Intel Core Ultra 9 275HX** — 24 threads (8 P-cores + 16 E-cores), Arrow Lake-HX |
| RAM | **32 GB** (provided by the user; the sandbox disallowed WMI, so `TotalPhysicalMemory` is not readable) |
| GPU (discrete) | **NVIDIA GeForce RTX 5080 Laptop GPU**, 16 GB VRAM, driver 32.0.16.2124 |
| GPU (integrated) | **Intel Graphics** (Arc, ~18 GB shared), driver 32.0.101.9150 |
| Display | 2 monitors, the primary's effective resolution **4096×1152** (scaled) |
| Storage | 1 TB SSD, of which **655 GB free** |
| OS | Windows 11 (the build number is not readable due to the sandbox) |

**Instantaneous load at the time of measurement** (not the browser, but things running on the machine — this is the real noise level):

| Process | RAM |
|---|---|
| `node` (this development environment) | 1866 MB |
| `Memory Compression` | 969 MB |
| `msedgewebview2` (3 processes, this is the assistant's UI) | ~1680 MB |
| `dwm` (window manager) | 696 MB |
| `chrome` | 548 MB |
| `thunderbird` | 398 MB |

Two conclusions follow from this: (a) **noise filtering is needed** for the measurements (DSH and the developer tools should run
on another machine, or in a closed measurement environment); (b) the user's complaint is understandable: of the 32 GB, ~5–6 GB
already goes to the background before a single browser window opens.

## 2. What this means for the design

1. **1 GB is not a constraint, but a discipline.** With 32 GB of RAM, the reason for 1 GB is not that there is no more,
   but that the goal is a *portable, quiet, battery-friendly* browser. Because of the machine's power, development
   will be fast (24 threads: snapshot compression and preview encoding can be parallelized), but the
   *acceptance criteria* remain the same.
2. **Two GPUs = two measurement branches.** The browser (engine + GPU process) can run on the Intel iGPU (efficient) or
   on the RTX 5080 (fast). Both must be measured: the RAM/CPU cost of OSR compositing and video playback
   differs. Default: **integrated GPU** (battery), the discrete GPU only on request
   (e.g. a WebGL-heavy page).
3. **16 GB VRAM is more than enough** for hardware OSR (shared textures), so OSR is not VRAM-limited.
4. **Two monitors + scaling**: DPI testing is not optional (125/150/175%); native window embedding
   is the most fragile point on DPI change and when dragging between monitors (this must be tested in Phase 0).
5. **Battery operation**: a separate profile — more aggressive hibernation, preview refresh disabled,
   media quality limit, hot engine (T1) disabled.
6. **655 GB free space**: the "several tens of GB" disk usage fits easily. The **base quota stays 50 GB**
   (per the decision), but can be overridden per machine — on VECTOR, for example, it can be set to 100 GB
   (`config/devices/vector.toml`), if time travel and snapshots require it.

## 3. Device profiles (Q4's answer)

The user request: "let there be profiles, the production test will decide anyway". Resource behavior is therefore
packaged into **profiles**, not a single switch. A profile is a bundle: engine lifetime,
hibernation time, preview refresh, media quality, budget bands, GPU selection.

| Profile | Engine | Hibernation | Preview refresh | Video | When |
|---|---|---|---|---|---|
| **Fast** | warm (T1) | 15 min | yes | full | desktop machine, wall power |
| **Balanced** *(default)* | warm (T1) | 10 min | yes, when idle | 1080p | everyday use |
| **Efficient** | cold (T2) after 2 min | 5 min | no | 720p | laptop on battery |
| **Ultra** | cold (T2) immediately | 2 min | no | 480–720p | memory-critical situation |
| **Manual** | every value adjustable individually | — | — | — | advanced user |

- The profile can **switch automatically** (wall power ↔ battery, memory pressure), but the user
  can pin it.
- Configuration remembered per machine (`config/devices/<hostname>.toml`), so that the default is different on VECTOR
  and on a weaker machine.
- Profile switching **never interrupts work**: it only sets future behavior.
- Behind every profile stand the same three metrics: RAM, card-open time, battery drain.
  The profile is not marketing, but **configuration** — the measurements (`perf/`) say which one is good.

## 4. Measurement baseline on the target machine

The `perf/` benchmark runs on VECTOR, in three modes:

| Mode | What it records |
|---|---|
| `ac` (wall power) | full budget, T1/T2, with preview refresh |
| `battery` | the same, but with the Efficient profile, with consumption measurement (if available) |
| `cold` | empty cache, first launch |

The benchmark's **noise filtering**: closing the DSH/Node/Edge processes or using a separate measurement account;
measuring the private working set per process; the median of 5 runs (see `03-MEMORIA.md` section 5).

## 5. What we do NOT have to do because of the hardware

- We do not have to aggressively reduce the cache or the resolution for the sake of memory — there is RAM.
- We do not have to give up hardware video playback or WebGL.
- We do not have to restrict the user because of "heavy pages" — the budget controller signals, but does not block.

The goal remains that **the same browser should be usable on an 8 GB machine too** — that is what the
profiles and the budget controller are for; VECTOR is only the place of development and the baseline.
