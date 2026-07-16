Add-Type -AssemblyName System.Drawing
$imagesFolder = "temp_borealis\resources\img\systems\"
$files = Get-ChildItem -Path $imagesFolder -Filter "*.png"

foreach ($file in $files) {
    try {
        $img = [System.Drawing.Image]::FromFile($file.FullName)
        
        # Calculate new size (e.g. max width 600)
        $maxWidth = 600
        $newWidth = $img.Width
        $newHeight = $img.Height
        
        if ($newWidth -gt $maxWidth) {
            $ratio = $maxWidth / $newWidth
            $newWidth = $maxWidth
            $newHeight = [math]::Round($img.Height * $ratio)
        }
        
        # If no change needed, skip or still re-encode? Re-encoding might still save space.
        $bitmap = New-Object System.Drawing.Bitmap($newWidth, $newHeight)
        $graph = [System.Drawing.Graphics]::FromImage($bitmap)
        $graph.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graph.DrawImage($img, 0, 0, $newWidth, $newHeight)
        
        $img.Dispose()
        $graph.Dispose()
        
        # Overwrite file
        $bitmap.Save($file.FullName, [System.Drawing.Imaging.ImageFormat]::Png)
        $bitmap.Dispose()
        
        Write-Host "Resized: $($file.Name) to ${newWidth}x${newHeight}"
    } catch {
        Write-Host "Failed to resize $($file.Name): $_"
    }
}
