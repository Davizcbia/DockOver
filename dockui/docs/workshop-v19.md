# docky Workshop V0.19

Workshop stores local static resources in `%LOCALAPPDATA%\docky\workshop`, separately from existing Liquid Desktop settings and program files. Settings > Workshop supports import, export, enable/disable, delete and Restore Default. Multiple installed packs can be selected; imports with an existing ID and exports to an existing file are refused. Selection survives restart. The Menu Bar polls selection changes and loads images in the background; missing, invalid or damaged resources fall back to built-in icons. It checks for damage periodically. No scripts, executables, external URLs or online service are supported.

## Pack format

`.dockypack` is a ZIP archive using **stored/uncompressed** entries. It contains `manifest.json`, `icons/*.png` and optionally `previews/*.png`. See `dockui/examples/replica-v19/manifest.json` and `replica-v19.dockypack`. Manifest requires format `dockypack`, numeric `schemaVersion: 1`, a unique lowercase ID, name, version, and an `icons` map. PNG paths use one directory level and portable lowercase names. The optional `preview` points to a PNG in `previews/`. Supported icon keys include `wifi.connected`, `wifi.disconnected`, `wifi.unavailable`, `sound.muted`, `sound.low`, `sound.medium`, `sound.high`, `sound.unavailable`, `battery.charging`, `battery.discharging`, `control-center`, `bluetooth.on`, `user`, and `input.pinyin`. Only states actually exposed by the Menu Bar are rendered; the reference avatar, Bluetooth and input-source assets are reserved for future slots.

Archives are limited to 8 MiB, 64 entries, 2 MiB per entry, single-frame PNGs, 1024x1024 icons and 2048x2048 previews. The reader validates structure, CRC, manifest version, allowed mappings and paths, rejects traversal, duplicate entries, Windows device names, links and reparse-point pack paths, and never extracts arbitrary files. These checks are not a cryptographic signature. Stored-only ZIP is an intentional V0.19 compatibility limitation; other compression methods are rejected. Restore Default is explicit; a damaged selection is backed up before explicit restoration. Damaged installed icons fall back to defaults and can be removed when their directory contains only known files. A pack with an invalid original archive is ignored rather than deleted.

Installation validates before writing, uses a separate staging directory, and publishes without replacing an installed pack. Operations share a cross-process mutex. Failure may leave a `.stage-*` or `.remove-*` directory for manual inspection; the program does not blindly remove unrecognized files. Do not put personal files in pack directories.

## Reference and verification limits

The included six transparent 256px icons are manually traced approximations of the supplied 2026-11-07 screenshots, not pixel-exact original assets. The reference shows a charging battery with 100%, while the local machine reported a different state; that state uses the built-in fallback. See `examples/replica-v19/reference-notes.md` and `previews/comparison.png`. The latter compares a reference crop with an enabled Menu Bar self-rendered frame, not a capture of the final DWM composition. Actual import/export and selection were exercised with Settings' opaque fallback and isolated user data; full GPU-glass desktop interaction and other monitor/DPI combinations remain unverified.

## Dock icons and SVG custom images

Workshop PNG mappings now also support `dock.finder`, `dock.launchpad`, `dock.trash.empty`, and `dock.trash.full`. Finder applies to Explorer and the This PC shell item; an explicit custom icon wins. The recycle-bin image follows the known empty/full state; unknown state falls back to Windows. The Dock reloads the selected pack when Workshop selection changes.

Custom image selectors accept `.svg` in addition to PNG and ICO. SVG is rasterized to a 512 px premultiplied canvas with Windows Direct2D and WARP at load time; source files are limited to 2 MiB. Direct2D implements a subset of SVG, so unsupported effects may be omitted. Workshop package entries remain stored PNG, not SVG.

The optional `dock.finder.dark` and `dock.launchpad.dark` PNG mappings provide dark appearance variants. When absent, the existing personal dark icon fallback remains available. These keys require the updated Workshop and Dock binaries; older builds reject packages containing them. The global and Settings slots are independent: select a combined package in both slots to use both sets of icons.
