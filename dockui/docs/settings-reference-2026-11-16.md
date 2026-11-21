# Settings reference analysis and implementation

Reference: `C:/Dev/dock/ref/Screen Recording 2026-11-16 at 6.21.21 PM.mov` (the original filename contains a narrow no-break space before PM).

## Observed behavior

The 23.11 s recording is 1704 x 1474 H.264, with variable timestamps (reported average 54.12 fps and a nominal 60 fps time base). Overview images were extracted at 1 s intervals. The sidebar interval 1.25–2.25 s and right-pane interval 12.8–13.8 s were additionally sampled at 60 timestamps per second; this sampling does not claim that the source contains 60 unique frames per second.

- The sidebar and content pane scroll independently. The sidebar profile and category rows pass behind the fixed search pill. Scrolling does not move the window controls.
- The right pane opens with a centered category icon, heading and short description in an introduction card, followed by grouped controls.
- As the introduction scrolls out, a compact title becomes visible in the fixed toolbar beside the navigation pill.
- The search field and back/forward pill have glass edges and sample the underlying surface.
- The recording does not demonstrate category-change choreography. No speculative horizontal page-slide animation was added.

Evidence: `outputs/settings-reference/contact-0.png`, `contact-1.png`, `sidebar-frames.png`, `content-frames.png`. Private account information from the recording is not transcribed here.

## Motion goal and stack

Make scrolling explain the relationship between the stationary toolbar and moving content. Retain the existing native Win32/D2D/D3D11 stack and display-paced ticker. Independent per-page and sidebar scroll springs retain their position and velocity when input reverses. The 0.18 s critically damped response is an implementation tuning choice, not a recovered Apple constant. Wheel input retains fractional deltas. Keyboard navigation ensures the target control is visible; hit coordinates invert the same content translation used for drawing.

The 160 DIP introduction offset makes room for the General overview card. Following the later Apple-design review, detail pages use a compact fixed toolbar and their first group begins at 80 DIP; they do not repeat the overview introduction. The compact title fades in over scroll positions 100–160 DIP as the large heading leaves. Back/forward history retains each category's scroll position. Sidebar scrolling is available only when its actual content exceeds the viewport.

Reduced motion uses the same layout, reading order and state, with immediate scroll positioning. Native search editing remains in place for normal selection, caret and IME behavior. While focused, its text interior uses an opaque matching color to avoid the GDI/DComp transparent-child artifact; the surrounding search pill remains rendered by the glass pipeline.

## Separate Studio surfaces

Glass Studio's Category list now includes `Settings: search field` (`settingsSearch`) and `Settings: back & forward buttons` (`settingsNavigation`). They use the existing complete set of material controls, save/reset/default behavior and independent persistent surface parameters. Settings reloads saved Studio changes while idle. The existing Sliders & switches category remains independent.

Search, navigation and pressed switch lenses have separate GPU scene/material caches, avoiding repeated texture allocation when drawing multiple differently sized glass controls in one frame. The background input is the Settings window's own content; desktop capture is unnecessary.

## Validation

- Release builds: Settings, Dock (including Glass Studio), Menu Bar, Manager and Desktop.
- `LiquidDesktopSettingsTest`: settings persistence, independent scrolling, reversal continuity, resize bounds, reduced motion and drawing/hit-coordinate inversion.
- `DockySettingsGlassTest`: legacy JSON load, independent search/navigation parameters, round-trip save, separate defaults and read-only protection.
- Existing Dock visual-model and motion-engine tests passed.
- Isolated rendered snapshots at minimum window height show the introduction, scrolled controls, fixed toolbar title, sidebar scroll and reversal. Diagnostic captures use explicit test settings/workshop paths and close automatically.
- Computer Use confirmed search focus and filtering, Escape clearing, independent pane scrolling, a scrolled switch hit, category selection and back navigation restoring the prior scroll position. Only isolated test settings were changed. Independently changing the search glass radius/brightness left navigation glass unchanged.
- The transparent native-edit artifact found during interactive testing was corrected and rechecked.

Real multi-DPI transitions, touch input, IME composition on a physical keyboard, hardware frame-rate acceptance and reduced-motion visual acceptance remain unverified. Build/render success alone does not establish those properties.

Motion rubric: purpose 3, hierarchy 3, brand fit 2, accessibility 2, implementation 3. Timing and performance remain unscored pending measured presentation evidence; no overall acceptance score is claimed.

## Source snapshot

Baseline HEAD: `42a2da15b42dd9bfc1bfe8b0c5ac7efec18e4e5c` plus this uncommitted working-tree change.

- `dockui/src/settings_window.cpp`: SHA-256 `64b4ed4366e8efb4f2c99db7553d090ce64923d94ece10584d7646c2c00ef0fa`
- `dockui/src/settings_motion.h`: SHA-256 `b62a156445ee25c506522c70fbf3ed6b8db53c5d15bd3af322991af46b8df28b`
- `dockui/src/renderer.cpp`: SHA-256 `0ac5ca7d9e563f1d57c42d2e660df9c446273c0077170343d9579e27bb932c7c`
- `dockui/src/renderer.h`: SHA-256 `f17432fae630eeb3b26287bee407133fdecddc95123e791dcda00679407a159f`
- `dockui/src/glass_params.cpp`: SHA-256 `e99dbe2be0f839ad6a17a37430c4df995147a8accf78fc2d82ff6d4f7f86ea35`
- `dockui/src/glass_params.h`: SHA-256 `1b3f97cf7b1d1fda38ce1ebf50f942200d8d1b51079024df567a0fe17e68f895`

## Follow-up Apple-design and capture review

The subsequent Software Update reference shows a detail page: toolbar heading alongside the navigation pill, a small gap to the first content group, leading labels and trailing actions. The previous universal introduction card was inappropriate for detail pages. Detail pages now use the compact toolbar immediately, with 24 DIP side insets, bounded content width, clipped/ellipsized headings and notes confined to their row bounds. The General overview retains the introduction demonstrated in the recording. Update actions share their trailing-aligned drawing/hit rectangles; related automatic-update preferences are grouped with one inset separator. Scroll limits now derive from the actual page layout.

Capture review: Microsoft documents that DXGI duplication may return a desktop image containing an already-composited mouse pointer, whereas WGC exposes an explicit cursor-exclusion property. Identity-authorized builds now prefer cursor-disabled WGC, with strict permission, cursor and border checks before every WGC session starts (including Genie captures). Portable builds without borderless permission retain DXGI; its embedded-pointer behavior is still a limitation of that fallback. The default local registered build was actually run with isolated configuration and logged `clean session verified (border=0 cursor=0)` followed by successful monitor capture and clean timed exit. No live user capture settings or permission state were changed by the tests.

Sources: [Microsoft desktop duplication pointer semantics](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api), [WGC border requirements](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired), [WGC cursor capture](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.iscursorcaptureenabled).

Evidence for this follow-up: `outputs/capture-layout-check/capture.log`, `outputs/capture-layout-check/dock/settings-1.bmp` and `outputs/capture-layout-check/about/settings-1.bmp`. Physical absence of the yellow border and cursor exclusion on the user's own desktop still require visual acceptance; tests used self-rendered captures to avoid adding another screenshot-capture session. At investigation time no Dock/Menu Bar process was running; a computer-use capture helper was observed and had subsequently exited, so the reported earlier border could not conclusively be attributed to Dock.

## Layout repair after screenshot feedback

The earlier review missed real defects: the MyDockFinder action buttons extended below their card, and multiline Dock notes had insufficient space above separators. That review did not establish layout acceptance.

Replaced fixed-height preference rows with DirectWrite-measured note heights, consistent label/detail spacing and vertically centered trailing controls. Drawing, hit testing, keyboard reveal and scroll bounds share the same geometry. Downloads has one group caption instead of repeating a prefix in each row. Migration cards now measure their descriptions and reserve a full button row plus bottom padding. General frame-rate labels have their own rows; Taskbar actions and shortcut recording fit the minimum width; Launchpad default/custom cards and update-status cards size to their contents. Existing options and actions remain functional.

Evidence: `outputs/layout-repair/v2-contact-900.png` and `v2-contact-760.png` cover all ten pages in dark appearance at 900 × 760 and 760 × 560 window sizes. `final-light-contact.png` covers General, Dock, Migration, Taskbar, Launchpad and About at minimum size in light appearance. `final-scroll-contact.png` shows those six pages after scrolling, with Launchpad Custom selected. The BMP inputs are app-rendered snapshots from explicitly isolated settings/workshop paths; no desktop capture helper is used. The two named window sizes were verified at the current Windows DPI; other DPI settings remain unverified. Update download/error layouts are measured in source but were not visually exercised in every state.

Release Settings build, existing settings and independent-glass tests, and `git diff --check` passed. The matching Settings executable was deployed to `build-borderless`; Settings/Dock/Menu Bar/Manager build markers match. No settings were changed by the review, and no import, taskbar change or update install was executed.

Repair source SHA-256 (settings_window.cpp): 2a27902f3354dd340ed75051a8e422cd46814c6f2864e4e707c5a8db37afb437.
