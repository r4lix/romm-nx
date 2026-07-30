<#
.SYNOPSIS
    Normalizes the platform banner source art into the uniform 16:9 PNGs that
    ship in romfs:/assets/platform_banners/.

.DESCRIPTION
    The banner sources arrive at whatever resolution and aspect ratio the artist
    produced (the shipped set mixes 1120x640 logo art with 1672x941 photography).
    The sidebar renders every banner at exactly one size, so the odd ratios are
    reconciled here, once, at authoring time -- never at runtime, where a crop or
    a rescale would cost frames on every navigation input.

    For each source image:
      * centre-crop to exactly 16:9 (crop, never stretch -- the console wordmark
        sits in the middle of every one of these, so a centred crop is the one
        that cannot cut it off)
      * rescale to -Width x -Height, identical for every platform
      * write to <OutputDir>/<canonical-id>.png as 32bpp PNG

    Deterministic: the same sources and the same parameters produce byte-stable
    output, so re-running it is a no-op in git.

    Sources are only ever read. Nothing is written back to -SourceDir.

.PARAMETER SourceDir
    Folder of source PNGs, named by platform. Not a build input -- the generated
    PNGs under romfs/ are committed, so this only has to exist when the art
    itself changes.

.PARAMETER OutputDir
    Where the normalized banners land. Must be the folder PlatformBanner.cpp
    reads from.

.PARAMETER Width / -Height
    Output size, 16:9. 512x288 gives the 336x189 the sidebar draws today a
    little headroom to grow without resampling artefacts.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\tools\normalize-platform-banners.ps1 -SourceDir C:\path\to\banner-art
#>
param(
    [string]$SourceDir = "assets/platform_banners_src",
    [string]$OutputDir = "romfs/assets/platform_banners",
    [int]$Width = 512,
    [int]$Height = 288
)

Add-Type -AssemblyName System.Drawing

# Source file stem -> canonical platform id.
#
# Output file names MUST match what romm::model::NormalizePlatformId() returns
# for the platform (source/model/PlatformCatalog.cpp), because that is the key
# the sidebar looks a banner up by. Art named after something else -- a console
# codename, a marketing name -- gets its one mapping here rather than a second
# alias table on the C++ side.
$CanonicalIdOverrides = @{
    "nx" = "switch"   # "NX" was the Switch's codename; the catalogue id is "switch"
}

# Catalogue ids romm-nx ships knowledge of, mirrored from GetPlatformCatalog().
# Only used to report which platforms will fall back to their text row -- a
# missing banner is a supported state, not an error.
$CatalogIds = @(
    "gb", "gbc", "gba", "nes", "snes", "n64", "nds", "3ds", "psx", "ps2", "psp",
    "arcade", "atari2600", "genesis", "ps3", "ps4", "saturn", "switch", "wii", "wiiu"
)

if (-not (Test-Path $SourceDir)) {
    Write-Error "Source folder not found: $SourceDir`nPass -SourceDir <path to the banner art>."
    exit 1
}

if ($Width -le 0 -or $Height -le 0) {
    Write-Error "-Width and -Height must both be positive."
    exit 1
}

if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
}

$targetAspect = $Width / $Height
$sourceFiles = Get-ChildItem -Path $SourceDir -Filter "*.png" | Sort-Object Name
if ($sourceFiles.Count -eq 0) {
    Write-Error "No .png files in $SourceDir."
    exit 1
}

$written = @()
$failed = @()

foreach ($file in $sourceFiles) {
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($file.Name).ToLowerInvariant()
    $id = if ($CanonicalIdOverrides.ContainsKey($stem)) { $CanonicalIdOverrides[$stem] } else { $stem }

    $img = $null; $bitmap = $null; $graphics = $null; $attrs = $null
    try {
        $img = [System.Drawing.Image]::FromFile($file.FullName)

        # Centre crop to 16:9. Whichever axis is proportionally long gets
        # trimmed equally at both ends; the other is taken whole.
        $srcAspect = $img.Width / $img.Height
        if ($srcAspect -gt $targetAspect) {
            $cropH = $img.Height
            $cropW = [int][math]::Round($img.Height * $targetAspect)
        } else {
            $cropW = $img.Width
            $cropH = [int][math]::Round($img.Width / $targetAspect)
        }
        if ($cropW -gt $img.Width)  { $cropW = $img.Width }
        if ($cropH -gt $img.Height) { $cropH = $img.Height }
        $cropX = [int](($img.Width - $cropW) / 2)
        $cropY = [int](($img.Height - $cropH) / 2)

        $bitmap = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

        # TileFlipXY stops the resampler sampling past the crop rect and
        # bleeding a translucent seam down the banner edges.
        $attrs = New-Object System.Drawing.Imaging.ImageAttributes
        $attrs.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)

        $destRect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
        $graphics.DrawImage($img, $destRect, $cropX, $cropY, $cropW, $cropH,
                            [System.Drawing.GraphicsUnit]::Pixel, $attrs)

        $outPath = Join-Path $OutputDir "$id.png"
        $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)

        $note = if ($id -ne $stem) { " (from $stem)" } else { "" }
        Write-Host ("  {0,-12} {1}x{2} -> crop {3}x{4} -> {5}x{6}{7}" -f `
            "$id.png", $img.Width, $img.Height, $cropW, $cropH, $Width, $Height, $note)
        $written += $id
    } catch {
        Write-Host "  FAILED $($file.Name): $_"
        $failed += $file.Name
    } finally {
        if ($attrs)    { $attrs.Dispose() }
        if ($graphics) { $graphics.Dispose() }
        if ($bitmap)   { $bitmap.Dispose() }
        if ($img)      { $img.Dispose() }
    }
}

Write-Host ""
Write-Host "Normalized $($written.Count) banner(s) into $OutputDir at ${Width}x${Height}."
if ($failed.Count -gt 0) {
    Write-Host "Failed: $($failed -join ', ')"
}

# A platform with no banner is not a failure: the sidebar renders its text row
# in the slot instead. Say which ones so the gap is a decision, not a surprise.
$missing = $CatalogIds | Where-Object { $written -notcontains $_ }
if ($missing.Count -gt 0) {
    Write-Host "No banner art for: $($missing -join ', ') -- these fall back to their text row."
}
$extra = $written | Where-Object { $CatalogIds -notcontains $_ }
if ($extra.Count -gt 0) {
    Write-Host "Beyond the catalogue (used if the server reports them): $($extra -join ', ')"
}
