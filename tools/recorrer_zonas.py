# tools/recorrer_zonas.py: prueba zonas del juego sin jugar hasta ellas. El guardado de SMRPG es JSON en UTF-16 sin
# firma: su planta de reaparición (_respawn_floor_name, el nombre de la escena: d01f1b el castillo de Bowser, d02out
# Mushroom Way...) decide dónde aparece Mario al cargar. Para cada escena pedida, copia el guardado de base de una
# carpeta de datos aislada, cambia la planta, arranca tools/probar.sh (entrada programada: saltar el vídeo, el título,
# el menú y cargar el autoguardado) y guarda las capturas en build/zonas/<escena>_<fotograma>.png y una hoja de
# contacto build/zonas/hoja.png. Los registros quedan en build/zonas/<escena>.log.
# Uso (MSYS2, en la carpeta del repositorio):
#   python tools/recorrer_zonas.py <carpeta de datos aislada> <guardado de base> escena [escena ...]
# Ejemplo: python tools/recorrer_zonas.py <carpeta aislada> <carpeta aislada>/<guardado de base> d02out d04b1
import json
import os
import re
import shutil
import subprocess
import sys

FRAMES = [2700, 3400]
INPUT = '1300:A:8,1600:A:8,1900:A:8,2200:A:8'


def set_floor(src, dst, floor):
    raw = open(src, 'rb').read()
    text = raw.decode('utf-16-le')
    body = text.rstrip('\x00')
    pad = len(text) - len(body)
    j = json.loads(body)
    j['_respawn_floor_name'] = floor
    out = json.dumps(j, ensure_ascii=False, separators=(',', ':'))
    open(dst, 'wb').write(out.encode('utf-16-le') + b'\x00\x00' * pad)


def wait_closed(secs=30):  # (la prueba anterior tarda en cerrarse: probar.sh no arranca con el juego abierto)
    import time
    for _ in range(secs * 2):
        r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq smrpg-recomp.exe'], capture_output=True, text=True)
        if 'smrpg-recomp' not in r.stdout.lower():
            return
        time.sleep(0.5)


def main():
    root, base, scenes = sys.argv[1], sys.argv[2], sys.argv[3:]
    root_win = subprocess.run(['cygpath', '-w', os.path.abspath(root)], capture_output=True, text=True).stdout.strip()
    os.makedirs('build/zonas', exist_ok=True)
    shots = []
    for sc in scenes:
        save_dir = os.path.join(root, 'save', 'save_0')
        shutil.rmtree(os.path.join(root, 'save'), ignore_errors=True)
        shutil.copytree(base, os.path.join(root, 'save'))
        set_floor(os.path.join(base, 'save_0', 'SMR_Save_Ver0011000'), os.path.join(save_dir, 'SMR_Save_Ver0011000'), sc)
        for f in os.listdir('build/frames') if os.path.isdir('build/frames') else []:
            os.remove(os.path.join('build/frames', f))
        log = f'build/zonas/{sc}.log'
        env_args = [f'LARECOMP_DATA_ROOT={root_win}', 'LARECOMP_HIDDEN=1', 'LARECOMP_NO_AUDIO=1', f'LARECOMP_INPUT={INPUT}',
                    'LARECOMP_SHOT_AT=' + ','.join(str(x) for x in FRAMES)]
        wait_closed()
        r = subprocess.run(['sh', 'tools/probar.sh', '75', log] + env_args, capture_output=True, text=True)
        crash = ''
        try:
            txt = open(log, 'rb').read().decode('utf-8', 'replace')
            m = re.search(r'\[crash\][^\n]*', txt)
            crash = m.group(0)[:120] if m else ''
            skipped = re.findall(r'(\d+) skipped', txt)
            skipped = skipped[-1] if skipped else '?'
        except OSError:
            skipped = '?'
        got = []
        for fr in FRAMES:
            p = f'build/frames/frame_{fr:05d}.png'
            if os.path.exists(p):
                q = f'build/zonas/{sc}_{fr}.png'
                shutil.copy(p, q)
                got.append(q)
                shots.append((f'{sc} {fr}', q))
        print(f'{sc}: {len(got)} capturas, saltados {skipped} {crash} {r.stdout.strip()[-60:]}', flush=True)
    try:
        from PIL import Image, ImageDraw
        cols = 4
        rows = (len(shots) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * 400, rows * 225), (20, 20, 20))
        d = ImageDraw.Draw(sheet)
        for i, (name, p) in enumerate(shots):
            im = Image.open(p).convert('RGB').resize((400, 225))
            x, y = (i % cols) * 400, (i // cols) * 225
            sheet.paste(im, (x, y))
            d.text((x + 4, y + 4), name, fill=(255, 255, 0))
        sheet.save('build/zonas/hoja.png')
    except ImportError:
        pass


main()
