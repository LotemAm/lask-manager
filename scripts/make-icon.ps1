# Rebuilds res\lask.ico from res\lask.svg (20 px and up) and res\lask-16.svg (16 px, drawn on the
# pixel grid). Headless Edge rasterizes; each size is stored as a PNG entry, which Windows reads
# since Vista. Run after editing either SVG and commit the .ico: the build does not run this.
#
#   .\scripts\make-icon.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$res = Join-Path $root "res"

$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
          "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw "msedge.exe not found" }

function DataUrl($file) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $res $file))
    "data:image/svg+xml;base64," + [Convert]::ToBase64String($bytes)
}
$large = DataUrl "lask.svg"
$small = DataUrl "lask-16.svg"
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

# Data URLs keep the canvas untainted, so toDataURL works from a file:// page.
$page = Join-Path ([IO.Path]::GetTempPath()) "lask-icon-$PID.html"
@"
<!doctype html><pre id="out"></pre><script>
const jobs = [$(($sizes | ForEach-Object { "[$_, '$(if ($_ -lt 20) { $small } else { $large })']" }) -join ',')];
Promise.all(jobs.map(([n, src]) => new Promise(done => {
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas'); c.width = c.height = n;
    c.getContext('2d').drawImage(img, 0, 0, n, n);
    done(n + ' ' + c.toDataURL('image/png'));
  };
  img.src = src;
}))).then(lines => document.getElementById('out').textContent = lines.join('\n'));
</script>
"@ | Set-Content -Encoding utf8 $page

try {
    $dom = & $edge --headless=new --disable-gpu --virtual-time-budget=5000 --dump-dom "file:///$($page -replace '\\', '/')" | Out-String
} finally {
    Remove-Item $page
}
$images = [regex]::Matches($dom, '(\d+) data:image/png;base64,([A-Za-z0-9+/=]+)') | ForEach-Object {
    [pscustomobject]@{ Size = [int]$_.Groups[1].Value; Png = [Convert]::FromBase64String($_.Groups[2].Value) }
}
if ($images.Count -ne $sizes.Count) { throw "rendered $($images.Count) of $($sizes.Count) sizes" }

# ICONDIR, one ICONDIRENTRY per size, then the PNGs.
$out = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($i in $images) {
    $dim = if ($i.Size -ge 256) { 0 } else { $i.Size }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$i.Png.Length); $w.Write([uint32]$offset)
    $offset += $i.Png.Length
}
foreach ($i in $images) { $w.Write($i.Png) }
$w.Flush()
$ico = Join-Path $res "lask.ico"
[IO.File]::WriteAllBytes($ico, $out.ToArray())
Write-Host "wrote $ico ($($images.Count) sizes, $([math]::Round($out.Length / 1KB)) KB)"
