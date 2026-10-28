# Starts Far in a conhost window with only FarNitroView loaded (scratch profile), sends keys, takes
# screenshots. Usage: fartest.ps1 -Keys "{F3}","{PGDN}" -Out shots\name
param([string[]]$Keys = @(), [string]$Out = "shot", [string]$Dir = "$PSScriptRoot\images", [int]$Wait = 1500,
      [switch]$Keep, [switch]$Attach, [switch]$Wt)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool r);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, int e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, int f, int e);
  public struct RECT { public int L, T, R, B; }
}
"@
[W]::SetProcessDPIAware() | Out-Null
$root = Split-Path $PSScriptRoot
$prof = "$root\build\farprofile"
New-Item -ItemType Directory -Force $prof | Out-Null
$title = "NVTEST"
if (-not $Attach) {
  Get-Process Far -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "*$title*" } | Stop-Process -Force
  $farArgs = "`"C:\Program Files\Far Manager\Far.exe`" -p`"$root\dist\FarNitroView`" -s `"$prof`" -title:$title `"$Dir`""
  if ($Wt) { Start-Process wt.exe -ArgumentList "-w new --title $title --suppressApplicationTitle $farArgs" }
  else { Start-Process conhost.exe -ArgumentList $farArgs }
  Start-Sleep -Milliseconds 2500
}
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 20 -and $h -eq [IntPtr]::Zero; $i++) { $h = [W]::FindWindow($(if ($Wt) { "CASCADIA_HOSTING_WINDOW_CLASS" } else { "ConsoleWindowClass" }), $title); if ($h -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 } }
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; exit 1 }
if (-not $Attach) { [W]::MoveWindow($h, 40, 40, 1200, 800, $true) | Out-Null; Start-Sleep -Milliseconds 700 }
$n = 0
function Shot($tag) {
  [W]::SetForegroundWindow($h) | Out-Null
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $b = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
  $p = "$Out-$tag.png"; New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
  $b.Save($p); $g.Dispose(); $b.Dispose(); Write-Output $p
}
$ws = New-Object -ComObject WScript.Shell
foreach ($k in $Keys) {
  [W]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 200
  if ($k -like "sleep:*") { Start-Sleep -Milliseconds ([int]$k.Substring(6)); continue }
  if ($k -eq "shot") { Shot ("{0:d2}" -f $n); $n++; continue }
  if ($k -match '^(click|dbl|rclick|wheel|cwheel|drag):(.*)$') {
    # mouse, coordinates relative to the window: click:x,y dbl:x,y rclick:x,y wheel:x,y,delta drag:x1,y1,x2,y2
    $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
    $a = $Matches[2].Split(',') | ForEach-Object { [int]$_ }
    [W]::SetCursorPos($r.L + $a[0], $r.T + $a[1]) | Out-Null; Start-Sleep -Milliseconds 100
    switch ($Matches[1]) {
      "click" { [W]::mouse_event(2, 0, 0, 0, 0); [W]::mouse_event(4, 0, 0, 0, 0) }
      "dbl" { 1..2 | ForEach-Object { [W]::mouse_event(2, 0, 0, 0, 0); [W]::mouse_event(4, 0, 0, 0, 0); Start-Sleep -Milliseconds 60 } }
      "rclick" { [W]::mouse_event(8, 0, 0, 0, 0); [W]::mouse_event(16, 0, 0, 0, 0) }
      "wheel" { [W]::mouse_event(0x800, 0, 0, $a[2], 0) }
      "cwheel" { [W]::keybd_event(0x11, 0, 0, 0); [W]::mouse_event(0x800, 0, 0, $a[2], 0); Start-Sleep -Milliseconds 50; [W]::keybd_event(0x11, 0, 2, 0) }
      "drag" { [W]::mouse_event(2, 0, 0, 0, 0); for ($i = 1; $i -le 10; $i++) { [W]::SetCursorPos($r.L + $a[0] + ($a[2] - $a[0]) * $i / 10, $r.T + $a[1] + ($a[3] - $a[1]) * $i / 10) | Out-Null; Start-Sleep -Milliseconds 30 }; [W]::mouse_event(4, 0, 0, 0, 0) }
    }
    Start-Sleep -Milliseconds $Wait
    continue
  }
  [System.Windows.Forms.SendKeys]::SendWait($k)
  Start-Sleep -Milliseconds $Wait
}
if (-not $Keep) { Get-Process Far -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "*$title*" -or $_.MainWindowTitle -like "FarNitroView*" } | Stop-Process -Force }
