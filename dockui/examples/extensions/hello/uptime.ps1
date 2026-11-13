# Data source for the "Uptime" widget: print one JSON object (the template data) and exit.
$up = (Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
@{ headline = ('{0}d {1}h' -f $up.Days, $up.Hours); body = 'since the last restart'; footnote = 'Updated ' + (Get-Date -Format t) } | ConvertTo-Json -Compress
