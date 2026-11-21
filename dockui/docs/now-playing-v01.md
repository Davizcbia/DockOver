# Now Playing v0.1: full-screen still and motion artwork

`docky-now-playing.exe` is a borderless full-screen Now Playing view. It follows the Apple Music lock screen in macOS and iPadOS 26. Click the media card in Control Center to open it. Launching it again closes it.

## Layout (one layout for video and stills)
- **Extend** (the default): the whole cover is sharp at full screen height. The rest of the screen is filled by background extension: the cover is mirrored past its edges, scaled down to 1/8, blurred (σ ≈ 72 px), and saturated ×1.25. The sharp edges fade into the extension over 7.5 % of the cover width, so no seam is visible. Portrait screens extend vertically.
- **Fill** (`F`): the cover is cropped to fill the screen.
- At the top: the date, plus a lock-screen clock in the user's 12/24 h format (`C` turns it on or off). Scrims stay subtle so the text is readable on bright covers.
- At the bottom: a glass platter. It shows the blurred cover behind it, darkened, with a hairline rim. It holds the title, the artist and album, the progress bar (drag to seek), elapsed and remaining time, Previous / Play-Pause / Next, and the system volume. The platter and the cursor fade out after 3.2 s idle. Moving the mouse or pressing a key brings them back. Clicking the artwork hides the platter.
- Motion: the window fades in over 0.42 s and fades out over 0.26 s, both over the desktop. Track changes and still → motion upgrades cross-fade over 0.7 s. While the music is paused, the motion cover pauses too.
- Keys: `Esc` closes. `Space` plays or pauses. `←`/`→` skip to the previous or next track. `↑`/`↓` change the volume.
- Settings are saved in `%LOCALAPPDATA%\LiquidDesktop\now-playing.ini` (`extend`, `clock`, `online`).

## Artwork sources (in order)
1. The Windows media session thumbnail. It shows at once and works with any player, offline.
2. The iTunes Search API. It is public and needs no account. Search order: by album, or by song if there is no album, in the user's storefront first, then in `us`. The artist must match. The cover comes from `artworkUrl100` rewritten to 2000×2000.
3. The album's public Apple Music page. The server-rendered JSON holds `videoArtwork.dictionary.motionDetailSquare` and `motionDetailTall`, each with an HLS `video` URL. `motionDetailTall` is used on portrait screens.
   - The fetcher picks the H.264 variant with the highest resolution up to 2160 and the highest bitrate. HEVC is used only as a fallback, because it needs the HEVC Store extension.
   - Apple serves it as one fMP4 file read in byte ranges. Adjacent ranges are merged and downloaded with one request.
   - The fragment `tfdt` timestamps start at 10 s, because they continue the HLS stream. `RebaseFragments` subtracts that start so a local file shows its first frame at once.
   - A typical file is 1080×1080 at 24 fps, 10–25 MB.

- The cache is in `%LOCALAPPDATA%\LiquidDesktop\artwork`: `<key>.jpg`, `.mp4`, `.page`, and the `.novideo` / `.nomatch` markers. Markers are retried after 3 days. Motion files are kept up to 1 GB, newest first. The key is FNV-64 of the normalised `artist|album`.
- Apple Music for Windows reports `Artist — Album` in the artist field. The fetcher splits it.
- Only the artist, album and title are sent, to `itunes.apple.com`, `music.apple.com` and `mvod.itunes.apple.com`. Set `online=0` to keep to the session thumbnail only.

## Rendering
- D3D11, DXGI flip swap chain for composition, and DirectComposition. The window is `WS_EX_NOREDIRECTIONBITMAP`, so it can fade over the desktop. Direct2D draws everything.
- The extension is a GPU effect graph: Border (mirror) → 2D affine (to 1/8 scale) → Gaussian blur → Saturation → Crop, then drawn ×8 with linear filtering.
- Motion artwork: an `IMFMediaEngine` in frame-server mode with the DXGI device manager. `TransferVideoFrame` writes into a BGRA texture, which is wrapped as a D2D bitmap, so frames never pass through the CPU.
- Pacing: a frame is presented when a new video frame arrives or an animation is running. Otherwise the loop waits on `DwmFlush` (video) or on messages (stills), with a 500 ms timer for the clock and the progress bar.

## Diagnostics
- `docky-now-playing.exe --snapshot out.png --image art.jpg [--video art.mp4] [--size 2560x1440] [--fill] [--no-clock]` renders one frame offscreen with fixture text.
- `docky-now-playing.exe --fetch "Artist" "Album" ["Title"]` runs one lookup. The log is `%LOCALAPPDATA%\LiquidDesktop\artwork\now-playing.log`.
- `DockyNowPlayingHlsTest` covers URL resolution, master and media playlists, range coalescing, page scanning and tfdt rebasing.

## Verified (2026-11-20)
- The HLS test passes.
- A live fetch of Olivia Rodrigo / GUTS cached the 2000 px still and 1080p H.264 motion artwork.
- Snapshots checked: still Extend at 1920×1080, motion Fill at 1920×1080, and motion Extend at 2560×1440.
- The real window opened and closed (exit 0) at the current DPI.

## Not verified / open
- Real players one by one: Spotify, Apple Music, NetEase, QQ Music, YouTube Music.
- Catalog matching for Chinese titles in the `cn` storefront.
- Multi-monitor and DPI changes; HDR displays.
- Long-run resource use, and whether the motion cover loops seamlessly over many hours.
- Lyrics.
- A Settings page toggle for `online` and the layout.
- The Apple Music page format can change. The fetcher then falls back to stills.

## Music wallpaper (`--wallpaper`, 2026-11-20)
- What it is: Settings > Control Center > **Music wallpaper** (`controlCenter.musicWallpaper`, off by default). While Apple Music plays, the desktop background shows the album artwork, the same Extend/Fill scene (motion when the album has it). There is no clock, no controls and no scrims. The user's wallpaper returns when the music stops; nothing in Windows settings is changed.
- Placement: a child window between the wallpaper and the icons.
  - Windows 11 24H2 and later: inside `Progman`, directly below `SHELLDLL_DefView`, above Explorer's wallpaper `WorkerW`.
  - Earlier builds: inside the `WorkerW` behind the icons (`0x052C` to Progman first).
  - One scene is drawn per monitor. The Dock's WGC capture of the wallpaper layer includes it.
- Source: only Apple Music for Windows. The session app id must contain `AppleMusic`. Other players are ignored, even when they are Windows' current session.
- Shown and hidden:
  - Fades in about 0.6 s after Apple Music plays and artwork is ready.
  - Fades out 2.5 s after it pauses, stops or closes; the grace period covers track changes. When faded out the window is hidden.
  - Motion pauses while hidden, and while a maximized or full-screen window is in front (single monitor).
- Lifetime:
  - The Manager starts `docky-now-playing.exe --wallpaper` while the setting is on (normal sessions only) and checks it every second. It therefore comes back after an Explorer restart, which destroys the window with its host.
  - On setting off and on DockOver exit it posts `WM_CLOSE` to the message-only window `DockyNowPlayingWallpaperCtl`.
  - Single instance: `Local\DockyNowPlayingWallpaper`.
- Diagnostics: `--wallpaper --test-image art.jpg` shows a fixed image as if music were playing.

## Matching, music apps, keep-clear (2026-11-20, later)
- **Cover check:** a catalog result is used only when its cover looks like the cover the player reports.
  - Method: 64-bit dHash of the session thumbnail (centred square) against each candidate's 200 px cover. Accepted at 14/64 bits or closer; measured 2 for the same cover and 28 for a wrong one.
  - Up to 10 candidates are checked, best name score first. When none passes, the player's own cover stays up, and a `.nomatch` marker skips that album for 3 days.
  - With no or a flat thumbnail, only exact name matches (score ≥ 6) are used.
- **Waiting for the cover:** the lookup waits until the player's cover has settled for 1.2 s (players briefly publish the previous track's cover), or 3.5 s without one.
- **Cache:** keys are now versioned (`v2|`), so earlier unchecked matches are not reused.
- **Search order, per storefront:** first the iTunes Search API (album; artist + song; song only), then the public Apple Music web search page (`ParseWebSearch`), which also lists brand-new singles the Search index lacks.
  - Storefronts: the user's own; for Chinese/Japanese/Korean names also `tw`, `hk`, `jp`; then `us`.
  - Traditional and Simplified Chinese compare equal (`LCMapStringEx`).
  - A different artist spelling (the US store shows "JJ Lin" for 林俊杰) is allowed only when the cover check decides.
- **Music apps (wallpaper):** Apple Music/iTunes, Spotify, NetEase Cloud Music, QQ Music, YouTube Music (th-ch app or Chrome/Edge install), TIDAL, Deezer, Amazon Music, KuGou, Kuwo, foobar2000, MusicBee, AIMP, Winamp and Windows Media Player.
  - Browsers are not included: their sessions are mostly video.
  - Session choice: the current session if it is one of these and playing, else a playing one, else the current or first one.
  - The full-screen view still follows Windows' current session (any app).
- **Settings > Control Center > Keep artwork clear of the Dock and menu bar** (`controlCenter.wallpaperAvoidBars`, on by default):
  - The wallpaper fits the sharp artwork inside each monitor's work area, which already excludes the Dock band and the menu bar. The extension fills behind the bars, and the sharp layer fades into it on every edge that does not reach the screen edge.
  - Changes apply within 0.5 s.
- **Motion covers:** they come from the matched Apple Music album page, whichever player is playing.

## Exact versions and lyrics (2026-11-20, later)
- **No remixes or other songs:**
  - Version words (remix, mix/mixed, VIP, live, acoustic, instrumental, edit, extended, sped up/slowed, demo, cover, 现场, 伴奏, 纯音乐, 重混, 翻唱, ...) must be the same in the player's name and the catalog name. Packaging words are ignored (remaster, deluxe, explicit, version, edition).
  - Song results must also be within 4 s of the player's track length (GSMTC timeline). Another artist spelling is accepted only within 2 s, and only after the cover check.
  - Web-search results must name the same artist. The cache key moved to `v3|`.
- **Lyrics sources** (`lyrics_source.cpp`):
  1. The user's files in `%LOCALAPPDATA%\LiquidDesktop\lyrics` (Settings > Control Center > Custom lyrics > Open folder...). Names: `Artist - Title.lrc`, `Title - Artist.lrc` or `Title.lrc`; `.txt` for plain text; UTF-8, UTF-16 or GBK. Version words must agree. Always preferred, never cached.
  2. The cache, `artwork\lyrics\<key>.lrc` (with a `.none` marker, retried after 3 days).
  3. LRCLIB (lrclib.net, open database, no account). First `/api/get` by artist, title, album and length (±3 s). Then `/api/search`, which must match the same artist, title, version and length. Synced lyrics are preferred.
  - Traditional Chinese lyrics are converted to Simplified when the player names the track in Simplified.
  - Windows media sessions carry no lyrics, so the lyrics shown inside Apple Music, Spotify, NetEase or QQ Music cannot be read from them.
- **LRC support** (`lyrics.h`): multiple time tags, `[offset:]`, enhanced word tags (stripped), metadata, gaps (shown as • • •), and same-time translation lines (shown smaller under the line).
- **Lyrics view** (full-screen Now Playing, Apple Music style), on when lyrics exist and `lyrics=1` (`L` key or the Lyrics pill, top right):
  - Backdrop: the artwork enlarged 1.4×, blurred (σ 16 at 1/8 scale), drifting slowly, darkened 30 %.
  - Left: the artwork card (rounded, soft shadow; motion artwork plays in it), with title and artist under it and the transport controls (no platter).
  - Right: lyrics in bold Segoe UI Variable Display (4.3 % of screen height). The sung line is white at 38 % of the screen height, the others 34 % white and fading at the edges. Scrolling eases toward the sung line (0.2 s ahead). Clicking a line seeks to it. Unsynced lyrics scroll with the progress.
- **Diagnostics:** `--lyrics-fetch "Artist" "Title" ["Album"] [--duration s]`; `--snapshot ... --lyrics song.lrc`.

## Desktop lyrics, borderless lyrics layout, timing, open app (2026-11-20, later)
- **Settings > Control Center:**
  - **Lyrics on the desktop** (`controlCenter.wallpaperLyrics`, on): the music wallpaper shows the lyrics. Lyrics are fetched only while it is on.
  - **Keep the borderless artwork with lyrics** (`controlCenter.lyricsFullArtwork`, on): with lyrics, the borderless artwork moves into the left 56 % of the screen (animated) and the lyrics go on the right, over the extension, with a soft dark gradient. Off: the card layout (blurred drifting backdrop, artwork card).
  - Both apply live, in the full-screen view and on the desktop.
- **Desktop lyrics:** no controls and no backdrop drift. Frames are drawn only while the lyrics scroll; the idle wait wakes at the next line. Nothing is drawn while covered.
- **Playback clock:** the position is anchored when the player's reported sample changes. It is extrapolated from LastUpdatedTime only when that stamp is under 30 s old, otherwise it runs on a local clock and freezes on pause. Apple Music for Windows leaves stale samples (seen: position 0, LastUpdatedTime 5 min old), which made the lyrics run ahead.
- **Lyrics sources** (cache key `l2|`):
  1. User files.
  2. Cache.
  3. NetEase Cloud Music public web API: search; same artist, title, version and length within 3 s; LRC plus translation (`tlyric`, merged by time). Credit lines ("作词 : …") are dropped; "纯音乐，请欣赏" counts as none.
  4. LRCLIB.
  - Translations are kept in the cache as same-time lines.
- **Control Center media card:** the card brings the playing app forward.
  - Package apps through IApplicationActivationManager.
  - Desktop apps (app id = exe name): their main window is restored and focused. A tray-only app is started again, which shows it. Otherwise the shell opens `shell:AppsFolder\<id>`.
  - The artwork on the card still opens full-screen Now Playing.
- **Diagnostics:** `--wallpaper --test-image art.jpg --lyrics song.lrc`; `--snapshot ... --lyrics song.lrc [--card]`.

## Lyrics clock, more sources, scrolling, distance blur (2026-11-20, later)
- **What Apple Music for Windows reports** (sampled during playback):
  - About one timeline sample per second, in whole seconds cut off (33, 34, 35, ...), sometimes skipping one (38 → 40). LastUpdatedTime is when the sample was posted.
  - Right after resuming, the paused sample is still reported (age 41 s) while the status is already Playing.
- **Clock:** the clock runs locally. Each new sample gives an observation:
  - position + age when the stamp is ≤ 3 s old, otherwise the position alone;
  - + 0.5 s for whole-second positions.
  - More than 1.5 s off (track change, seek, resume): the clock is set to it. Otherwise it moves a quarter of the way, which averages out the cut-off and jitter (before, the clock snapped back to each cut-off value, about 0.5 s late on average).
  - Paused: the clock stands still.
- **Sources, in order:** user files → cache (`l3|`) → NetEase Cloud Music → QQ Music (`c.y.qq.com`, with a Referer) → Kugou (`lyrics.kugou.com`, base64 LRC) → LRCLIB.
  - Every result must have the same title and version words, the same artist, and a length within 3 s.
  - A leading "Title - Artist" line and credit lines are removed. Original lyrics only: translations are dropped for now.
- **Scrolling** (full-screen view):
  - The mouse wheel, or dragging the lyrics column, scrolls the lyrics. Lines are then evenly dimmed and not blurred.
  - Clicking a line (without dragging) plays from it and follows the song again.
  - Settings > Control Center > **Return to the current line after scrolling** (`controlCenter.lyricsAutoReturn`, on): back to the sung line after 1.2 s without scrolling. Off: the lyrics stay until a line is clicked or the track changes.
  - The desktop wallpaper is behind the icons and gets no input, so it always follows the song.
- **Distance blur:** each line is recorded once as a command list and drawn through Gaussian blur and opacity effects. The blur grows with its distance from the eased scroll position: 1.7 px per line, up to 9 px (× DPI). The sung line is sharp. This applies on the desktop too.

## Shortcuts, desktop lyric input, Settings fix (2026-11-20, later)
- **Settings crash:** `switchMotion[32]` was indexed by switch id, and switches 32/33 wrote past it, corrupting memory (access violation later). It is now 64 entries, with a bounds-checked accessor.
- **While scrolling:** the sung line stays white and the others grey; only the blur goes away.
- **Desktop lyrics input:**
  - The wallpaper registers Raw Input (RIDEV_INPUTSINK on its message-only window), only while lyrics are on the desktop.
  - When the pointer is on the desktop itself (root window Progman/WorkerW) over the lyrics: the wheel scrolls them, and a click without movement (< 0.6 s, ≤ 4 px) on a line plays from it. Dragging is not used on the desktop: it is Explorer's selection rectangle.
  - No hooks, nothing injected.
- **Global shortcuts** (Settings > Control Center, last card; stored as `controlCenter.shortcutLyrics/Artwork/Desktop/NowPlaying` = (MOD_* << 8) | vk, 0 = none):
  - Ctrl+Alt+L: desktop lyrics on/off.
  - Ctrl+Alt+M: music wallpaper on/off.
  - Ctrl+Alt+D: hide/show DockOver Desktop. The `DockyDesktopToggleVisible` message to `DockyDesktopHostV1` hides all of its visible windows except the host, and shows them again.
  - Ctrl+Alt+N: full-screen Now Playing open/close.
  - Recording: click the button, press a key with Ctrl/Alt/Shift. Esc cancels, Backspace/Delete removes the shortcut.
  - The Manager registers them (ids 0x4460..0x4463) and re-registers when the settings change.

## Beat pulse, lyrics timing, desktop wheel fix (2026-11-20, later)
- **Desktop wheel fix:** DockOver Desktop's Stacks puts a transparent `DockyDesktopStacksCatcher` window over the desktop, so the pointer was never "on Progman/WorkerW". That class now counts as the desktop.
- **Beat pulse** (Settings > Control Center > "Move lyrics with the beat", `controlCenter.lyricsBeat`, on):
  - `audio_beat.cpp` reads the default output through WASAPI loopback. That is the standard capture path visualisers use: no hooks, nothing injected, nothing stored or sent.
  - It low-passes the signal twice at 150 Hz and sums the energy per 10 ms block. `OnsetDetector` reports a beat on a rising block more than 1.55× the 1.2 s average, at most one per 0.2 s, with strength (ratio − 1.55)/1.2 + 0.2 (beats under 0.3 are ignored). The pulse decays with a 0.2 s time constant.
  - Effect: the sung line grows up to 5 % and lifts 5 px; the lyrics side of the background brightens up to 5.5 % white. Nothing pulses while scrolling.
  - It runs only while lyrics are shown, music plays and (desktop) the wallpaper is visible and not covered. The desktop draws at most 60 frames/s for it.
  - Background: Apple does not document a beat feature or API. The closest is the album-art background of Live Lyrics shown as beat-reactive in the iOS 13 beta, and support pages describing lyrics "line-by-line and beat-by-beat".
  - `docky-beat-probe [seconds]` prints the detected pulses. On a ~140 BPM EDM track: ~0.43 s spacing in the main section, the strongest pulse at the drop.
- **Lyrics timing** (`controlCenter.lyricsOffsetMs`, default +300 ms = earlier; −3000..3000):
  - Settings row with − / + (0.1 s steps). In full-screen Now Playing, `[` / `]` change it while listening, with an on-screen note; the value is saved for the desktop too.
  - Added because players' media-session timelines can be off (Lyricify documents the same problem and offers a manual correction). Apple Music for Windows reports whole seconds.

## Colour flow background, Apple Music seek (2026-11-20, later)
- **Reference:** the user's handheld video of iPadOS Apple Music (ref/IMG_4180.MOV; frames read with a Media Foundation grabber). The card is on the left and the lyrics on the right; the background drifts as large soft shapes.
- **Colour flow** (replaces the white flash and line bounce of the previous round; setting renamed "Flow with the music", same key `lyricsBeat`):
  - `match::Palette`: k-means, 4 colours, on a ≤48-px sample; ranked by size × (0.55 + saturation) × (0.35 + brightness). Computed from the session thumbnail or the catalog still (64×64 via WIC), copied to motion art.
  - Colours are made vivid: brightness ≥ 0.62, saturation × 1.25; greys stay grey.
  - `DrawFlow`: a base of the average colour × 0.7; 6 radial-gradient blobs (palette colours) on Lissajous paths, radius about 0.34 of the long side. Beat pulses swell them by up to 10 %. A 16 % black veil keeps white lyrics readable.
  - The flow clock advances at 0.25 + 1.1 × bass level (AudioBeat::Level, √(energy/peak), smoothed), so loud passages move faster.
  - Card layout: the flow is the background. Borderless layout: the flow fades in over the extension as lyrics appear, and the artwork feathers into it.
  - Runs while lyrics show and music plays; the desktop draws at most 30 frames/s.
- **Apple Music seek:**
  - Apple Music for Windows reports IsPlaybackPositionEnabled = false, and TryChangePlaybackPosition returns true but does nothing (tested).
  - DockOver instead sets Apple Music's own progress slider through UI Automation (accessibility): AutomationId "LCDScrubber", RangeValue in seconds. Every Apple Music window of class WinUIDesktopWin32WindowClass is tried, visible first (the first one found can be a hidden helper).
  - Lyric-line clicks and progress-bar drags therefore work with Apple Music. Other players use the media session as before.

## Liquid silk on the lyrics side (2026-11-20, later)
- **Reference:** a stock "glossy liquid silk" loop supplied by the user: satin folds, bright specular sheen, dark creases.
- **shaders/silk.hlsl** (fxc at build time, like the glass shaders): a full-screen-triangle pixel shader.
  - Shape: a domain-warped wave height field (three warp layers, three wave terms incl. fine ripples). Normal from central differences (× 0.3).
  - Lighting: diffuse 0.32 + 0.8·N·L; a sharp specular (pow 90) whose strength rises with the beat (0.5 + 0.5·beat), plus a soft one (pow 12); a satin rim sheen.
  - Colour: a gradient across up to 6 palette colours, indexed by height; creases darkened.
- **silk_renderer.h:** draws it on the shared D3D11 device into a B8G8R8A8 texture, wrapped as a Direct2D bitmap, before Direct2D's BeginDraw. Size: half of 56 % of the width × half the height of the widest screen. Time = the flow clock (faster when loud).
- **Placement:**
  - Card layout: left = blurred artwork backdrop, right = silk from 44 % of the width.
  - Borderless layout: silk from 50 %, fading in with the lyrics.
  - The silk fades in over 16 % of the width at its left edge, with a 30 % black veil for the lyrics.
  - Without the shader (no palette, setting off, init failure) the earlier colour blobs or blur are used.
- **Palette:** now 6 colours.

## Silk v2, Music settings page, frame rate (2026-11-20, later)
- **Silk shape:** only two broad folds now (one gently warped long wave + one slower cross wave, scale 1.7), no fine ripples.
- **Soft focus:** Direct2D Border (clamp) -> Gaussian blur (σ 4.5 at half resolution) -> Crop, then scaled onto the lyrics side.
- **Blend with the artwork:** silk starts at 40 % of the width (card layout) / 44 % (borderless) and fades in over 25 % of the width; 30 % black veil for the lyrics.
- **Stronger rhythm:** on the beat the folds swell (relief × (1 + 0.6·beat)), the specular grows (0.45 + 0.9·beat), the whole cloth brightens (× 1 + 0.35·beat), and each new pulse kicks the flow clock forward (0.35 × pulse), a visible surge.
- **Frame rate:** `controlCenter.musicFps` (30/60/120/240, 0 = unlimited; default 60). Caps continuous animation in full-screen Now Playing (input and track changes draw at once) and the music wallpaper (unlimited = DwmFlush pacing).
- **Settings:** new **Music** page (after Control Center, music-note icon, `--page music`) with music wallpaper, keep clear, desktop lyrics, borderless artwork, auto-return, flow, lyrics timing, shortcuts and Frame rate. Control Center keeps its own blur and switch.

## Silk v3: an always-visible twisting fold, beat = twist, colours by share (2026-11-20, later)
- **Main fold:** a twisted fold (cross-section rotates between a drape and a ridge along its length) whose centre is kept inside the lyrics area (x 0.55..0.80, y 0.30..0.70 of the silk texture), near-diagonal, bending along its length. A softer second fold drifts over the rest, plus a broad undulation.
- **Beat:** no brightening and no specular boost any more. The beat ("twist", the pulse with ~50 ms attack and ~0.4 s release, plus 0.25 × loudness) raises the fold's bend, its twist rate and the domain warp. The flow-clock surge on each beat stays.
- **Colours by share:** `match::Palette` can report each colour's share of the artwork's pixels; `ToColors` stores it in alpha. The silk sorts colours largest first, lifts shares toward the main colour (share^1.25, floor 0.04) and walks a triangle-wave colour field through cumulative shares, so each colour covers about its share of the area and the main colour the most.
- **Test hooks:** snapshots read `DOCKY_SILK_TIME` and `DOCKY_SILK_TWIST`.
- Also fixed: Settings `ApplicationsWindowSizePath` searched for `/` only (`L"\/"`), now `\` or `/`.

## Silk v4: colours swirl together, gentler twist (2026-11-20, later)
- **Colours:** no more banded gradient. Each palette colour is a soft pool drifting on its own slow path (radius 0.2 + 0.4·√share, pull share^0.6, the main colour also fills the gaps). The colour lookup position is dragged by a two-layer swirl and pushed along the fold slope, so the pools smear and mix into each other like paint while the main colour still covers most of the area.
- **Twist:** about half as strong: bend 0.06 + 0.06·twist, twist rate 0.9 + 0.7·twist, warp 0.03 + 0.03·twist; twist capped at 1.0. Relief 0.17 and specular 0.5 keep the main fold readable.

## Slower flow, beat tracker (2026-11-20, later)
- **Flow speed:** flow clock rate 0.12 + 0.3 x loudness (was 0.25 + 1.1 x), beat surge 0.1 x pulse (was 0.35): about a third of the speed.
- **Beat detection (audio_beat.h):**
  - BandSplitter: 35 Hz high-pass + two 130 Hz Butterworth low-passes (bass), 2 kHz high-pass (treble); 10 ms blocks.
  - BeatTracker: onset strength = log-energy rise over the previous 30 ms (bass + 0.25 treble); onsets = its peaks over mean + 1.4 x deviation (1.5 s), 0.15 s apart.
  - Tempo every 0.25 s: autocorrelation of the last 6 s of onset strength, 75-200 BPM, score = ac(l) + 0.5 ac(2l) + 0.33 ac(3l) with a soft 120 BPM prior; phase = the grid offset that collects the most onset strength. Lock at normalised autocorrelation >= 0.2, release below 0.12; a locked tempo only changes when another scores 1.3x better three times in a row.
  - Locked: pulses come from the grid 30 ms early (capture delay); onsets near it nudge the grid; off-beat onsets are ignored; three grid beats without hits silence the grid until hits return. Unlocked: only clear onsets pulse.
- **docky-beat-probe --file <audio>** runs the tracker offline on a file (Media Foundation) and prints pulses, locked share, tempo and pulse regularity. On the user's library it locked 40-76 % of the time (breakdowns and intros stay unlocked); live loopback locked within ~3 s on a 146 BPM track.

## Silk colours from what is on screen; silk clear of the artwork (2026-11-20, later)
- **Bug:** motion artwork inherited the palette of the art it replaced (session thumbnail or catalog still, which can be another edition), so a black/orange motion cover could get a purple/blue silk.
- **Fix:** `Scene::PaletteOfBitmap` draws a frame down to 64 x 64 and reads it back; `App::VideoPalette` takes the palette from the motion artwork's first frame and again after ~45 frames (covers that open dark). Snapshots with `--video` do the same.
- **Dark covers stay dark:** `ToColors` lifts near-black colours to 0.3 brightness (was 0.62, which turned black into grey).
- **Artwork no longer covered:** in the borderless layout `DrawArt` records the sharp artwork's right edge; the silk and the reading veil start there (clamped to 30-56 % of the width) instead of at a fixed 44 %.
- **Follow-up (same day):** a black cover still showed purple: near-black k-means clusters with a faint blue cast were lifted to 0.3 brightness *with their relative saturation*, becoming purple. `ToColors` now takes saturation from the real chroma over the new brightness (dark greys stay grey). Silk highlights are tinted by the cloth colour and scaled by its brightness (no grey-white streaks on dark covers). The silk now starts 5 % of the width inside the artwork's own feathered edge, so no seam shows between them.
- **Black + accent:** a black cover with an orange figure gave a grey silk (black lifted to 0.3, orange share tiny). `ToColors` now lifts dark colours only to 0.1 (black stays black), and `SilkRenderer` weights colours by share^0.7 x (0.35 + 1.3 x chroma), so a small vivid accent gets real room next to the dominant dark.

## More twist, beat timing and stability (2026-11-20, later)
- **Silk:** main fold bend 0.095 + 0.08·twist, twist rate 1.5 + 0.9·twist, warp 0.045 + 0.04·twist (between v3 and v4).
- **Beat timing:** pulses carry their own time. `AudioBeat::Post(strength, at)`, with `at` = packet QPC time + block offset + `BeatTracker::PulseOffset()`. Grid beats are posted 60 ms ahead, so capture batching no longer shifts them; `Pulse()` keeps the previous pulse decaying until a future one starts.
- **Stability:**
  - While locked, only the grid pulses (hits just before a beat count for it and nudge it).
  - Gentler phase and period corrections, plus a PLL period trim.
  - Phase jumps need three agreeing estimates.
  - Tempo memory carries through breaks (re-lock prefers the old tempo when it scores ≥ 0.7 of the best); silence forgets it.
  - Estimate buffers are reused.
- **Probe:** `--lock <lock> <release>`, locked jitter, and lock/tempo-change/phase-jump counts. Library result (12 tracks): about 55 % locked, locked jitter about 67 → 51 ms RMS. Thresholds 0.16/0.10 lock more (68 %) with more jitter, so the defaults stay 0.20/0.12.
