# Apple Design review — partial code review

Status: **incomplete; no real desktop visual acceptance**. Artifact: native Win32/D3D DockOver source and isolated model/input tests, not a macOS application. Apple HIG is used as a design reference, not a claim of native-platform compliance. No pixel dimensions are asserted as Apple requirements. The design purpose is to make app launching and desktop controls discoverable while leaving the desktop visible.

## Critical — accessibility

- **System transparency and contrast:** Settings and Dock now gate glass on Windows transparency and high-contrast state; Dock has an opaque system-color fallback, but other surfaces and real desktop composition remain unverified (`src/glass_params.h`, `src/liquid_glass.cpp`). The absence of a search match is a lead, not proof of all runtime behavior. Verify actual OS settings and implement opaque/high-contrast fallback without changing material presets. Reference: Apple Design Skill `accessibility.md › Vision`, `liquid-glass.md › Review checklist`.
- **Keyboard access:** Settings search accepts arrows and Escape, and Dock switches now have keyboard activation; Workshop and Launchpad density controls now have keyboard paths, but accessible names and complete focus behavior remain unverified (`src/settings_window.cpp`, `WM_LBUTTONUP`). Give all controls focus, names, visible focus and keyboard activation. Reference: `accessibility.md › Mobility`; actual keyboard and assistive-technology acceptance pending.

## Improvements

- **High — motion:** Dock observes Windows animation preference for magnification, bounce, sliding, reorder and pointer easing (`src/main.cpp`, `src/dock_model.cpp`); Control Center also snaps its open/close and glass transition (`src/control_center.cpp`). Dock model test passes; live OS changes and other surfaces remain unverified. Reference: `accessibility.md › Cognitive`.
- **High — Settings coverage:** search, Dock and Launchpad controls, Workshop, Migration backup and About exist; other categories still contain placeholder content. Search is category/keyword based, not a complete settings index. Complete genuine controls and ensure keyboard navigation. Reference: `settings.md › Best practices`; visual review pending.
- **High — materials:** Settings navigation uses the existing GPU material and opaque failure fallback (`src/settings_window.cpp`); existing local fixed material values predate this review and were not changed. Audit each popup and system accessibility state; no claim of a complete material placement audit. Reference: `liquid-glass.md › Where the material belongs`.
- **Medium — power information:** Control Center now uses shared Windows power decoding and distinguishes unknown, absent, charging, external and battery power (`src/system_controls.cpp`, `src/control_center.cpp`). The source-state model and read-only query pass; actual AC and battery transitions remain untested.

## Scope still to review

Dock placement and 144/240 Hz motion; Menu Bar layout, menu keyboard/focus and contrast; Wi-Fi/audio device interactions; Control Center and popups; Launchpad, Workshop, icons, typography, corners and mixed DPI. Compare with the macOS 27 UI kit and local references using actual desktop composition. Resource licensing and publish isolation are separate unresolved work. No screenshot or UI frame was treated as desktop acceptance.

## Window capture attempt

An isolated Settings window was located through the Windows accessibility tree, but the captured image showed background rather than the target content. No visual acceptance or real desktop composition conclusion can be drawn from that capture.

## Measured opaque palette pairs

Using the WCAG relative-luminance formula on `src/design_tokens.h` light tokens: primary #20232A on #FFFFFF is 15.73:1; secondary #595D69 on #FFFFFF is 6.57:1; secondary #595D69 on #F4F4F7 is 5.99:1. These pair calculations meet 4.5:1 for ordinary text, but do not establish actual composited contrast over glass, disabled text, other surfaces, or user-selected high contrast. Reference: `accessibility.md › Vision`.

Settings opacity gating uses Windows `UISettings.AdvancedEffectsEnabled` and `SPI_GETHIGHCONTRAST`; actual disabled/high-contrast visual behavior and system-color adaptation remain unverified. [Microsoft API documentation](https://learn.microsoft.com/en-us/uwp/api/windows.ui.viewmanagement.uisettings.advancedeffectsenabled).

Dock opaque fallback: isolated GPU buffer checks passed for dark and light plate colors, running indicator and transparent exterior. These checks are not desktop composition or high-contrast visual acceptance.

## Surface coverage and unresolved acceptance

This table is a code-oriented triage, not a complete visual audit. “Reference” names a principle in the installed Apple Design Skill; it does not assert pixel specifications for this Windows application.

| Surface | Observed implementation | Reference and difference / action | Status |
| --- | --- | --- | --- |
| Dock | `src/dock_model.cpp` uses time-driven hover and bounce; `src/main.cpp` reads Windows motion and transparency preferences. | `accessibility.md › Cognitive`: reduce motion. Real preference toggles, edge behavior, contrast and 144/240 Hz display remain to test. | Partial; model/offscreen checks only |
| Menu Bar | `src/menu_bar.cpp` includes status icons, menus and display geometry. | `the-menu-bar.md › Best practices`: predictable commands and keyboard support. Layout, full-screen behavior and mixed DPI need desktop comparison. | Partial; visual acceptance pending |
| Control Center | `src/control_center.cpp` includes radio, volume and battery surfaces; state comes from Windows. | `accessibility.md › Mobility` and `liquid-glass.md › Review checklist`: usable keyboard flow and accessibility fallbacks. Error and reduced-motion changes need real UI and device testing. | Partial; no real radio test this round |
| Settings | `src/settings_window.cpp` provides category search, opaque fallback and selected keyboard actions. | `settings.md › Best practices`: respect system settings. Many pages remain incomplete; UI Automation names, high-contrast colors and full keyboard flow are not established. | Partial; target window capture unavailable |
| Launchpad | `src/launchpad.cpp` includes search, grid, folder and motion preference handling. | `accessibility.md › Cognitive` and `layout.md › Adaptability`: reduce motion and adapt to space. Real visual, high DPI and physical input remain to verify. | Partial; model only |
| Workshop | `src/workshop.cpp` handles local package import/export; Settings has mouse and partial keyboard paths. | `accessibility.md › Mobility`: keyboard/assistive access. Confirm focus order, accessible names, selection and failure feedback on real UI. | Partial; isolated package and synthetic key tests |
| Menus/popups | `src/unified_menu.cpp` and `src/menu_surface.cpp` provide menu surfaces and fallback. | `the-menu-bar.md › Best practices` and `liquid-glass.md › Review checklist`: keyboard, legibility and appropriate material. Test actual composition and reduced transparency per surface. | Partial; no final composite |
| Icons, type, corners, color | Local Windows glyphs and existing design tokens are used; a few opaque color pairs measured above. | `typography.md › Ensuring legibility`, `color.md › Best practices`: verify rendered contrast and scaling. Font, glyph fallback, corners and Apple-reference comparison are not visually accepted. | Open |

The existing local reference packs are not approved release assets. See `apple-resource-inventory-2026-11-08.md`.
