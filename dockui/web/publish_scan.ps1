# Publishes the phone scanner page (web\dockdrop-scan) to GitHub Pages: syncs it into the clone of
# Davizcbia/dockdrop-scan (C:\Dev\dockdrop-scan) and pushes. The repo's own README.md and .nojekyll are kept.
#   powershell -ExecutionPolicy Bypass -File publish_scan.ps1 [-Message "..."] [-NoPush]
param([string]$Message = 'Update scanner page', [switch]$NoPush, [string]$Clone = 'C:\Dev\dockdrop-scan')
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'dockdrop-scan'
if (-not (Test-Path (Join-Path $Clone '.git'))) { throw "No clone at $Clone (git clone https://github.com/Davizcbia/dockdrop-scan.git)" }
git -C $Clone pull --ff-only
robocopy $src $Clone /MIR /XF README.md .nojekyll /XD .git /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
git -C $Clone add -A
if (-not (git -C $Clone status --porcelain)) { 'Nothing to publish.'; exit 0 }
git -C $Clone commit -m $Message -m 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>'
if (-not $NoPush) { git -C $Clone push; 'Published: https://davizcbia.github.io/dockdrop-scan/' }
