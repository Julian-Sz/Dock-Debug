# End-to-end smoke test: drives the running app through Windows UI Automation like a user would.
#
# What it does: starts Dock-Debug, records two sessions (both trackers, then the USB tracker alone), sets
# two problem markers (button with a note, Ctrl+M without), checks the log files it wrote and their
# content, visits the Topology, System and Live view pages, selects a device, refreshes, and closes the
# window (expecting exit code 0).
#
# Requirements: a real Windows desktop session (not a service or headless CI runner) with at least one
# display; no other Dock-Debug instance running. The test writes into the real log folder
# (Documents\Dock-Debug\tracking) and deletes exactly the files it created afterwards; existing files are
# left alone (the about file is rewritten, as by every run of the app). Takes about 30 seconds; do not use
# mouse or keyboard meanwhile (Ctrl+M is sent to the app window).
#
# Usage (from the repository root, after building the app unpackaged):
#   msbuild winui\DockDebug.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:WindowsPackageType=None
#   powershell -ExecutionPolicy Bypass -File tests\ui-test.ps1 [-Exe path\to\DockDebug.exe]
# Exit code: the number of failed checks.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\winui\x64\Debug\DockDebug\DockDebug.exe')
)
$ErrorActionPreference = 'Stop'
$Exe = [System.IO.Path]::GetFullPath($Exe)
if (-not (Test-Path $Exe)) { throw "App not found: $Exe (build it unpackaged first, see the header of this script)." }
if (Get-Process DockDebug -ErrorAction SilentlyContinue) { throw 'Close all running Dock-Debug instances first.' }

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms
Add-Type 'using System; using System.Runtime.InteropServices; public static class Foreground { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window); }'
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

$failures = 0
function Check($what, $ok) {
    "{0,-62} {1}" -f $what, $(if ($ok) { 'ok' } else { 'FAIL' })
    if (-not $ok) { $script:failures++ }
}
function WaitFor([scriptblock]$condition, [int]$seconds = 15) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $result = & $condition
        if ($result) { return $result }
        Start-Sleep -Milliseconds 250
    }
    return $null
}
function Condition($property, $value) { New-Object System.Windows.Automation.PropertyCondition($property, $value) }
function ByName($name) { $script:win.FindFirst($TS::Descendants, (Condition $AE::NameProperty $name)) }
function ById($id) { $script:win.FindFirst($TS::Descendants, (Condition $AE::AutomationIdProperty $id)) }
function Texts() { @($script:win.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition) | ForEach-Object { $_.Current.Name } | Where-Object { $_ }) }
function OpenPage($name) { (ByName $name).GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select() }
function Toggle($element) { $element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle() }
function Invoke($element) { $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
function Read($file) { Get-Content $file.FullName -Raw -Encoding UTF8 }

# The sessions this run will create: one higher than the highest existing s<N>_ prefix.
$dir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Dock-Debug\tracking'
$before = @(Get-ChildItem $dir -ErrorAction SilentlyContinue | ForEach-Object Name)
$first = 1 + (@($before | Where-Object { $_ -match '^s(\d+)_' } | ForEach-Object { [int]($_ -replace '^s(\d+)_.*', '$1') }) + 0 | Measure-Object -Maximum).Maximum
$second = $first + 1
function NewFiles($filter) { @(Get-ChildItem $dir -Filter $filter | Where-Object { $before -notcontains $_.Name } | Sort-Object Name) }
"expecting sessions s$first and s$second"

$process = Start-Process $Exe -PassThru
try {
    $hwnd = WaitFor { $process.Refresh(); if ($process.MainWindowHandle -ne 0) { $process.MainWindowHandle } }
    $win = $AE::FromHandle($hwnd)
    Check 'window opened' ($null -ne $win)

    # --- Session 1: both trackers, two markers.
    OpenPage 'Tracking'
    $toggles = WaitFor { $t = @($win.FindAll($TS::Descendants, (Condition $AE::IsTogglePatternAvailableProperty $true))); if ($t.Count -ge 2 -and $t[0].Current.IsEnabled) { ,$t } }
    Check 'two tracker switches, enabled after the first scan' ($toggles.Count -eq 2)
    Toggle $toggles[0]; Start-Sleep -Milliseconds 500; Toggle $toggles[1]
    Check "status shows session s$first" ([bool](WaitFor { (Texts) -match "session s$first" }))

    (ById 'MarkerNote').GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('Left monitor went black (test)')
    Invoke (ById 'MarkerButton')
    Check 'marker status shows "Marked at"' ([bool](WaitFor { (Texts) -match '^Marked at ' }))

    OpenPage 'Live view'
    Check 'Live view shows "Mark problem" while tracking' ([bool](WaitFor { -not (ById 'LiveMarkerButton').Current.IsOffscreen }))
    [Foreground]::SetForegroundWindow($process.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 500
    [System.Windows.Forms.SendKeys]::SendWait('^m')
    Start-Sleep 2

    OpenPage 'Tracking'
    Start-Sleep 1
    Toggle $toggles[0]; Start-Sleep -Milliseconds 500; Toggle $toggles[1]
    Check 'status shows "Tracking is off"' ([bool](WaitFor { (Texts) -contains 'Tracking is off' }))

    # --- Session 2: the USB tracker alone.
    Toggle $toggles[1]
    Check "status shows session s$second" ([bool](WaitFor { (Texts) -match "session s$second" }))
    Start-Sleep 1
    Toggle $toggles[1]
    Start-Sleep 1

    # --- The files written.
    $new = @(Get-ChildItem $dir | Where-Object { $before -notcontains $_.Name } | ForEach-Object Name)
    foreach ($pattern in "s${first}_*_session_info.log", "s${first}_*_monitor_started.log", "s${first}_*_usb-tree_started.log",
                         "s${first}_*_monitor_stopped.log", "s${first}_*_usb-tree_stopped.log", "s${second}_*_session_info.log",
                         "s${second}_*_usb-tree_started.log", "s${second}_*_usb-tree_stopped.log") {
        Check "one file $pattern" (@($new | Where-Object { $_ -like $pattern }).Count -eq 1)
    }
    Check "no files outside s$first / s$second" (@($new | Where-Object { $_ -notmatch "^s($first|$second)_" }).Count -eq 0)

    $markers = NewFiles "s${first}_*_marker.log"
    Check "two marker files in s$first (button and Ctrl+M)" ($markers.Count -eq 2)
    if ($markers.Count -eq 2) {
        $marker = Read $markers[0]
        Check 'marker 1 has the note' ($marker -match 'NOTE: Left monitor went black \(test\)')
        Check 'marker 1 has events and a snapshot' (($marker -match 'WINDOWS EVENTS from') -and ($marker -match 'USB DEVICE TREE'))
        Check 'marker 2 has "(no note)"' ((Read $markers[1]) -match 'NOTE: \(no note\)')
    }

    $started = (NewFiles "s${first}_*_usb-tree_started.log")[0]
    $text = Read $started
    Check "header line `"Session:  $first`"" ((Get-Content $started.FullName -TotalCount 2)[1] -eq "Session:  $first")
    Check 'started file has WINDOWS EVENTS' ($text -match 'WINDOWS EVENTS from')
    Check 'snapshot has a POWER line' ($text -match '(?m)^POWER: ')
    Check 'snapshot has MONITORS with connection and EDID' (($text -match '      connection: ') -and ($text -match '      EDID: '))
    Check 'monitors have EDID hex data' ($text -match 'EDID data \(\d+ bytes, hex\):\r?\n\s+00FFFFFFFFFFFF00')
    Check 'snapshot has USB DEVICE TREE with driver lines' (($text -match 'USB DEVICE TREE') -and ($text -match '  driver: '))
    Check 'snapshot has USB PORT PROBLEMS' ($text -match 'USB PORT PROBLEMS')

    $info = Read (NewFiles "s${first}_*_session_info.log")[0]
    Check 'session info has COMPUTER, POWER, RELIABILITY HISTORY, events' (($info -match 'COMPUTER') -and
        ($info -match 'USB selective suspend: ') -and ($info -match 'RELIABILITY HISTORY') -and ($info -match 'WINDOWS EVENTS from'))

    # --- Pages.
    OpenPage 'Topology'
    Check 'Topology page shows USB connections and live throughput' ([bool](WaitFor { $t = Texts; ($t -contains 'USB connections') -and ($t -contains 'Live throughput') }))

    OpenPage 'System'
    Check 'System page shows power settings and the computer model' ([bool](WaitFor { $t = Texts; ($t -contains 'USB selective suspend') -and ($t -contains 'Model') }))
    Start-Sleep 1

    OpenPage 'Live view'
    $items = WaitFor { $t = @($win.FindAll($TS::Descendants, (Condition $AE::ControlTypeProperty ([System.Windows.Automation.ControlType]::TreeItem)))); if ($t.Count -gt 0) { ,$t } }
    Check "device tree has items ($($items.Count))" ($items.Count -gt 0)
    $items[[Math]::Min(1, $items.Count - 1)].GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Check 'details panel shows "Instance ID"' ([bool](WaitFor { (Texts) -contains 'Instance ID' }))

    $refresh = ById 'RefreshButton'
    WaitFor { $refresh.Current.IsEnabled } | Out-Null  # disabled while a scan runs
    Invoke $refresh
    Start-Sleep 2
    Check 'still running after Refresh' (-not $process.HasExited)

    # --- Close normally.
    $win.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
    $exited = WaitFor { $process.Refresh(); $process.HasExited } 10
    Check 'process exits after closing the window' ([bool]$exited)
    if ($exited) { Check ('exit code 0 (got 0x{0:X})' -f $process.ExitCode) ($process.ExitCode -eq 0) }
} catch {
    Check "unexpected error: $($_.Exception.Message)" $false
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Get-ChildItem $dir | Where-Object { $before -notcontains $_.Name } | Remove-Item
}

"`n$failures failed"
exit $failures
