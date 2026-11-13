# Apple resource inventory — 2026-11-08

This inventory records local references; it does not grant permission to use or redistribute them. The exact license accompanying the macOS 27 exports has not been verified. Apple's [2023 Design Resources License](https://developer.apple.com/support/downloads/terms/apple-design-resources/Apple-Design-Resources-License-20230621-English.pdf) limits covered resources to specified Apple-platform mock-ups and restricts embedding, non-Apple mock-ups and redistribution. Do not assume that publicly accessible exports or a separate package confer Windows usage rights. Verify the applicable current terms and any separate written permission before use.

| Local source / asset | Format and intended mapping | Status |
| --- | --- | --- |
| `C:/Dev/dock/docky/assets/apple-reference/Pointers/` | 22 SVG cursor exports; reference only | Rights for Windows use and redistribution not established |
| `official-menubar-v2/` | Apple Logo, Wi-Fi, Control Center and selection SVG exports; pack contains raster PNG mappings for `system.logo`, `wifi.connected`, `control-center` | No release authorization established |
| `official-dock-v3/` | Finder and Apps @4x PNG; `dock.finder`, `dock.launchpad`, plus menu mappings | No release authorization established |
| `official-dock-v4/` | Empty/full Trash @4x PNG plus v3 mappings | No release authorization established |
| `user-downloads/` and `apple-menu-bar-local-01.dockypack` | User-provided PNGs; cropped Wi-Fi and Control Center mappings | Provenance recorded locally; rights not established; no release authorization |
| Wi-Fi alternate states, volume, battery | No corresponding official assets confirmed in this local inventory | docky defaults used where a mapping is absent; no official pack prepared |
| SF Symbols and San Francisco fonts | Apple resources with separate terms | Not bundled or newly downloaded in this work; applicable rights not evaluated |

The four existing Apple-reference `.dockypack` files remain local; none is designated a releasable package. An earlier handoff recorded local activation of v4; current user configuration was not modified or assumed. Workshop's current archive parser accepts PNG entries, while separate custom Dock icons support a Direct2D SVG subset. PNG fallback and old packs must remain compatible if archive SVG support is added.

Root `.gitignore` excludes the local `ref/` and `docky/assets/apple-reference/` paths from normal staging. This is not an installer, CI, update, or archive exclusion guarantee. No production packaging route was established in the limited search; inspect actual build and release artifacts before distributing. The main program must continue to run without Apple assets.

Sources: [Apple Design Resources](https://developer.apple.com/design/resources/), [macOS UI Kit on Sketch](https://www.sketch.com/s/57153a31-3379-4737-8ac6-dbfd6525f052/symbols?g=Menu%2520Bar), the local `manifest.json`, `official-menubar-v2/provenance.json`, v3/v4 manifests and `user-downloads-manifest.json`.
