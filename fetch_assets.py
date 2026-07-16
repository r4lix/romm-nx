import urllib.request
import json
import os
from svglib.svglib import svg2rlg
from reportlab.graphics import renderPM
import io

OUT_DIR = "temp_borealis/resources/img/systems"
os.makedirs(OUT_DIR, exist_ok=True)

def download_and_convert(theme):
    url = f"https://api.github.com/repos/rommapp/romm/contents/frontend/assets/console/{theme}/systems?ref=master"
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    
    try:
        with urllib.request.urlopen(req) as response:
            data = json.loads(response.read().decode())
    except Exception as e:
        print(f"Failed to fetch {theme} list: {e}")
        return
        
    for f in data:
        if f['name'].endswith('.svg'):
            slug = f['name'][:-4]
            dl_url = f['download_url']
            out_path = os.path.join(OUT_DIR, f"{slug}.png")
            print(f"Downloading {theme}/{slug}.svg ...")
            try:
                dl_req = urllib.request.Request(dl_url, headers={'User-Agent': 'Mozilla/5.0'})
                with urllib.request.urlopen(dl_req) as dl_resp:
                    svg_data = dl_resp.read()
                
                # Write temp svg
                temp_svg = f"temp_{slug}.svg"
                with open(temp_svg, "wb") as tf:
                    tf.write(svg_data)
                
                drawing = svg2rlg(temp_svg)
                if drawing:
                    renderPM.drawToFile(drawing, out_path, fmt='PNG')
                    print(f" -> Saved {out_path}")
                else:
                    print(f" -> Failed to parse SVG {slug}")
                
                os.remove(temp_svg)
            except Exception as e:
                print(f" -> Error converting {slug}: {e}")

download_and_convert("neon")
download_and_convert("default")
