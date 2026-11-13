param([Parameter(Mandatory=$true)][string]$Exe)
$ErrorActionPreference = 'Continue'
$root = Join-Path $env:TEMP ('docky-restore-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$active = "liquid-taskbar-windows 1`nphase active`n"
function RunCase([string]$name,[string]$content,[bool]$expectedChange,[int]$expectedExit,[bool]$tmp=$false) {
  $path = Join-Path $root ($name + '.txt')
  [IO.File]::WriteAllText($path,$content,[Text.UTF8Encoding]::new($false))
  if ($tmp) { [IO.File]::WriteAllText($path+'.tmp','preserve') }
  $before = (Get-FileHash -LiteralPath $path).Hash
  & $Exe --record $path --simulate 2>&1 | Out-Null
  $actual = $LASTEXITCODE
  $changed = $before -ne (Get-FileHash -LiteralPath $path).Hash
  if ($actual -ne $expectedExit -or $changed -ne $expectedChange) { throw "$name failed: exit=$actual changed=$changed" }
  if ($tmp -and [IO.File]::ReadAllText($path+'.tmp') -ne 'preserve') { throw 'existing temp record changed' }
  Write-Output "PASS $name"
}
RunCase 'hidden' ($active + "w Shell_TrayWnd 101 77 12345 1 1 1`n") $true 0
RunCase 'pending' ($active + "w Shell_TrayWnd 101 77 12345 1 0 0`n") $false 3
RunCase 'badclass' ($active + "w OtherWindow 101 77 12345 1 1 1`n") $false 3
RunCase 'corrupt' 'broken' $false 3
RunCase 'conflict' "liquid-taskbar-windows 1`nphase conflict`n" $false 3
RunCase 'temp' ($active + "w Shell_TrayWnd 101 77 12345 1 1 1`n") $false 3 $true
& $Exe --record (Join-Path $root 'missing') 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'missing record should be a safe no-op' }
& $Exe --simulate 2>&1 | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'simulation must require an explicit record' }
Write-Output 'PASS missing record and explicit simulation path'
