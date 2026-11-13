# Isolated visual capture mode

`LiquidMenuBar.exe --dev-visual-capture` is a diagnostic-only, process-scoped mode. It is rejected unless all of the following are supplied: an explicit non-default `--settings-path`, an explicit non-default `--workshop-root`, an exclusive new `--dev-diag` file, and `--exit-after` between 1 and 120 seconds. Paths are compared case-insensitively after GetFullPathNameW normalization; this is an accidental-use guard, not a security boundary against reparse-point aliases.

In this mode Control Center and unified popups do not request WGC backdrop capture and do not exclude their own windows from screen capture. With no sampled backdrop, existing non-glass fallbacks are used. Capture selection is never written to user configuration; it ends when the diagnostic process and windows exit. The normal path is unchanged: WGC requires `WDA_EXCLUDEFROMCAPTURE` before starting, preventing recursive sampling. Do not compare test-mode fallback material with production liquid glass as if they were equivalent.

Example (use freshly created isolated paths and never the user's existing settings or Workshop root):

```
LiquidMenuBar.exe --dev-visual-capture --settings-path <isolated-settings.json> --workshop-root <isolated-workshop> --dev-diag <new-trace-file> --exit-after 15 --control-center
```

Real desktop captures can contain user data, network names and other applications; handle them as local private artifacts. Do not use render-target snapshots as proof of final desktop composition.