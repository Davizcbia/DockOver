# Motion Engine v0.1

Status: engine and Dock/menu migration implemented and unit-tested; **no real-desktop visual or latency acceptance yet**. Tree: `docky0.1.0` (not the older `dock-wgc-p0.4` worktree; `dock_model.cpp` was identical in both, so the engine ports directly).

## Provenance of every claim

| Item | Basis |
| --- | --- |
| Two-parameter spring model (`response`/`dampingFraction`, `duration`/`bounce`, bounce range -1..1) | Public SwiftUI API; confirmed by a web lookup of Apple's WWDC23 "Animate with springs" (10158) listing. |
| `stiffness = (2π/duration)²`, `damping = 4π(1-bounce)/duration` (bounce ≥ 0), `4π/(duration(1+bounce))` (bounce < 0), mass 1 | Recalled from WWDC18/WWDC23 material. The lookup did **not** show the equations. **Not re-verified.** |
| Preset values Smooth 0 / Snappy 0.15 / Bouncy 0.3 bounce, 0.5 s duration | Commonly cited; Apple's pages only describe them qualitatively (smooth = no bounce, snappy = small, bouncy = higher). **Approximations.** |
| Closed-form oscillator solution, retarget keeps value and velocity | Textbook physics, verified against an independent brute-force integrator in `tests/motion_test.cpp`. |
| `MomentumProject`, `Rubberband` | Functions published in Apple's Fluid Interfaces material, as given in the `apple-design` skill. |
| Apple's internal solver, additive-animation blending, per-OS tuning, `interpolatingSpring`/`interactiveSpring` internals, `matchedGeometryEffect`, `PhaseAnimator`, `KeyframeAnimator` internals | **Not public. Not reproduced.** Nothing in this code claims to be Apple's algorithm. |
| Dock hover/launch/auto-hide numbers | Tuned to match the *old* settle times of this project, **not measured from macOS**. The project notes say dynamic macOS Dock behaviour is not measured. |

## What exists

`src/motion.h` (header-only, no Windows dependency):

- `SpringSpec`: `FromResponse`, `FromDuration`, `Smooth/Snappy/Bouncy`.
- `Spring`: scalar spring solved exactly for any `dt` (under-, critically, over-damped; over-damped branch uses two decaying exponentials so it cannot overflow). `SetTarget` keeps value **and** velocity; `SetTarget(t, v)` hands off a gesture velocity; `AddVelocity`, `Reverse`, `Pause/Resume`, `Cancel` (freeze in place), `Finish`, `SnapTo`, `SetValue`. Non-finite or negative `dt` is ignored, `dt > 0.5 s` is capped, a non-finite result snaps to target. Rest detection snaps exactly and stops work.
- `SpringN<N>`: several properties with one spec (2D motion is two independent springs, per the apple-design guidance).
- `Bounce`: ballistic hop (gravity + restitution, solved per ground contact so any `dt` is exact). Relaunching while airborne keeps the height (no positional jump).
- `MomentumProject`, `Rubberband` helpers (not yet used by a surface).

## Migrated

| Surface | Before | Now |
| --- | --- | --- |
| Dock hover magnification | first-order, τ 0.075 in / 0.13 out | critically damped springs, response 0.20 in / 0.30 out; reversal mid-ramp keeps velocity |
| Dock cursor follow | first-order, τ 0.035 | critically damped, response 0.08 |
| Launch bounce | `|sin|` with fixed 0.95 s and two fixed hops | ballistic hops, apex = `bounceHeight`, restitution 0.6, stops below 3 DIP; interruptible |
| Auto-hide slide | first-order τ 0.08 / 0.11 | critically damped, response 0.26 show / 0.34 hide; reversal keeps velocity |
| Reorder slide and post-drop settle | first-order τ 0.08 | critically damped springs per tile, response 0.24 |
| Unified menu open/close | fixed 130/110 ms timed curves on a 16 ms `WM_TIMER` (≈15.6 ms real tick) | spring, response 0.20 open / 0.14 close; closing mid-open continues from the live value; real-`dt` from QPC; 4 ms tick with `timeBeginPeriod(1)` held only while animating |

All tuning lives in `dockmotion::` (`src/dock_model.h`) and the menu constants (`src/unified_menu.cpp`). Reduced-motion (Windows animation setting) still snaps Dock springs and keeps menus to an opacity-only fade.

## Not done yet (honest list)

- Control Center open/close/hover/press, Launchpad (`Ease()` first-order), Downloads Fan (`FanEase` smoothstep), glass pointer smoothing (`UpdateGlassPointer`), running-indicator and add/remove tile transitions, submenu switching. Same migration pattern applies.
- Liquid Glass edge-highlight / shape-morph coupling: only the earlier `Materialize()` (blur, refraction and tone scaled by an appear amount) exists. No continuous shape morph between glass plates.
- Frame pacing: the Dock already runs on a waitable swap chain with real-time `dt`; menus and other popups still use `WM_TIMER`. Frame-time stability, input-to-photon latency and 144/165/240 Hz behaviour are **not measured**. A passing unit test says nothing about that.
- Window Genie effect: **not implemented**, see below.

## Window Genie / minimize feasibility (analysis, nothing built, none of it verified on this machine)

- Can be done: an overlay window owned by the Dock that warps a captured image of the window into the Dock icon with a mesh, using the existing D3D11 + DirectComposition path and the Windows Graphics Capture code already in the project.
- Needs a workaround: Windows Graphics Capture stops delivering frames for a minimized window, so the image must be captured *before* minimize. The Dock does not receive minimize requests for other processes' windows; a `SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART)` fires only as DWM's own minimize animation starts, so either keep a rolling capture of the foreground window (GPU/battery cost) or accept a capture taken at the event. The native DWM minimize/restore animation would play at the same time unless it is disabled per window, and whether `DWMWA_TRANSITIONS_FORCEDISABLED` can be applied to another process's window is **unconfirmed**.
- Cannot be promised: pixel-exact macOS Genie (curve shape and timing are not public), window-restore from the Dock icon without owning the target window's state, and anything that requires hooking or injecting into other processes (explicitly out of scope: it risks window-management stability).
- Recommended next step: a standalone prototype that captures one chosen window, runs the mesh warp into a Dock icon rect, and then minimizes it through the normal API, to measure capture latency and flicker before any integration.

## Tests

- `DockyMotionTest` (`tests/motion_test.cpp`): closed form vs brute-force in five damping regimes, frame-rate independence (1/10/60/240/1000 fps equivalents agree to 1e-8), retarget continuity, retarget spam, NaN/inf/negative/huge `dt`, extreme stiffness, rest snap, overshoot behaviour, reverse/pause/cancel/finish, parameter conversions, bounce apex/timing independent of `dt`, relaunch continuity.
- `LiquidDockVisualModelTest`: existing geometry/reduced-motion checks plus interrupted hover (leave and re-enter mid-ramp stays within 0..1 and below the physical per-frame slope bound) and bounce range/decay/relaunch continuity.
- Menu tests (`LiquidUnifiedMenuTest`, navigation, hit region, native menu) still pass; they verify behaviour and timing bounds, not how the animation looks.
