# tools/texturas_hd.py: crea texturas HD con IA a partir de las volcadas por el juego (Menú > Vídeo > Imagen >
# «Volcar texturas»). Cada PNG de <datos>/texturas-volcadas que sea de color (sRGB: «...srgb.png») y no esté ya hecho
# se amplía x4 con Real-ESRGAN (el modelo realesr-animevideov3, el que mejor va con el estilo del juego: bordes
# limpios sin inventar detalle) y se reduce a la escala pedida (x2 por defecto: x4 gasta 4 veces más memoria de vídeo)
# en <datos>/texturas/<huella>_ia_x<N>.png, que el juego usa la próxima vez que carga esa textura («Texturas HD»).
# El canal alfa (recortes, transparencias) se amplía aparte, bicúbico. Se saltan las texturas planas (pocos colores:
# rellenos, paletas) y las de menos de 64 píxeles; las que no son de color (normales, máscaras: sin «srgb») no se tocan.
#
# Real-ESRGAN ncnn Vulkan (BSD-3, xinntao/Real-ESRGAN, versión portátil realesrgan-ncnn-vulkan-20220424-windows):
# la ruta de su .exe en la variable REALESRGAN o en --realesrgan.
# Uso: python tools/texturas_hd.py <carpeta de datos> [--escala 2|3|4] [--realesrgan ruta.exe] [--rehacer]
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image


def plana(im):
    cols = im.convert('RGB').resize((64, 64)).getcolors(4096)  # (None: más de 4096 colores)
    return cols is not None and len(cols) < 48


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('datos')
    ap.add_argument('--escala', type=int, default=2, choices=[2, 3, 4])
    ap.add_argument('--realesrgan', default=os.environ.get('REALESRGAN', ''))
    ap.add_argument('--rehacer', action='store_true')
    a = ap.parse_args()
    src = os.path.join(a.datos, 'texturas-volcadas')
    dst = os.path.join(a.datos, 'texturas')
    if not os.path.isdir(src):
        sys.exit(f'No está {src}: activa «Volcar texturas» en el menú del juego (Vídeo > Imagen) y juega un rato.')
    exe = a.realesrgan
    if not exe or not os.path.exists(exe):
        sys.exit('Falta Real-ESRGAN: pon la ruta de realesrgan-ncnn-vulkan.exe en la variable REALESRGAN o en --realesrgan.')
    exe = os.path.abspath(exe)
    os.makedirs(dst, exist_ok=True)
    hechas = {f[:16] for f in os.listdir(dst) if f.lower().endswith('.png')}
    todo = sorted(f for f in os.listdir(src) if f.lower().endswith('.png'))
    pendientes = []
    for f in todo:
        if 'srgb' not in f or (f[:16] in hechas and not a.rehacer):
            continue
        im = Image.open(os.path.join(src, f))
        if im.width < 64 or im.height < 64 or plana(im):
            continue
        pendientes.append(f)
    print(f'{len(todo)} volcadas, {len(pendientes)} por ampliar (x{a.escala}) en {dst}', flush=True)
    if not pendientes:
        return
    tmp = tempfile.mkdtemp(prefix='texturas_hd_')
    try:
        ent, sal = os.path.join(tmp, 'in'), os.path.join(tmp, 'out')
        os.makedirs(ent)
        os.makedirs(sal)
        for f in pendientes:
            Image.open(os.path.join(src, f)).convert('RGB').save(os.path.join(ent, f))
        modelos = os.path.join(os.path.dirname(exe), 'models')
        r = subprocess.run([exe, '-i', ent, '-o', sal, '-n', 'realesr-animevideov3', '-s', '4', '-f', 'png', '-m', modelos],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-800:])
            sys.exit('Real-ESRGAN falló')
        n = 0
        for f in pendientes:
            p = os.path.join(sal, f)
            if not os.path.exists(p):
                print('sin salida:', f)
                continue
            o = Image.open(os.path.join(src, f))
            w, h = o.width * a.escala, o.height * a.escala
            im = Image.open(p).convert('RGB')
            if im.size != (w, h):
                im = im.resize((w, h), Image.LANCZOS)
            im = im.convert('RGBA')
            if 'A' in o.getbands():
                im.putalpha(o.getchannel('A').resize((w, h), Image.BICUBIC))
            for old in [x for x in os.listdir(dst) if x.startswith(f[:16])]:
                os.remove(os.path.join(dst, old))
            im.save(os.path.join(dst, f'{f[:16]}_ia_x{a.escala}.png'))
            n += 1
            if n % 25 == 0:
                print(f'  {n} / {len(pendientes)}', flush=True)
        print(f'Hecho: {n} texturas HD. El juego las usa la próxima vez que carga cada una (mejor: reinícialo).')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


main()
