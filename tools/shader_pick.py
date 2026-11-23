# tools/shader_pick.py: saca de los paquetes de shaders (los de nxnative dm/gfx/tools/shader_split.py) los registros de
# las claves que fallan en un barrido (las líneas "[fallo] <paquete> <clave>: ..." de tools/shader_sweep.c) y los
# escribe en otra carpeta con el mismo formato, para repetir solo esos (con LARECOMP_LOG_BINDLESS=1, por ejemplo).
# Uso: python tools/shader_pick.py <carpeta de paquetes> <errores del barrido> <carpeta de salida> [texto del motivo]
import os
import re
import struct
import sys


def main():
    pkg, err, out = sys.argv[1:4]
    want_why = sys.argv[4] if len(sys.argv) > 4 else ''
    want = {}
    for line in open(err, encoding='utf-8', errors='replace'):
        m = re.match(r'\[fallo\] (\S+) ([0-9a-f]{16}): (.*)', line)
        if m and want_why in m.group(3):
            want.setdefault(m.group(1), set()).add(m.group(2))
    os.makedirs(out, exist_ok=True)
    total = 0
    for name, keys in sorted(want.items()):
        d = open(os.path.join(pkg, name), 'rb').read()
        o, recs = 0, []
        while o + 0x38 <= len(d):
            if d[o:o + 8] != b'BTCDBTCD':
                o += 1
                continue
            ctl, cl = struct.unpack_from('<2I', d, o + 0x28)
            end = o + 0x38 + cl + ctl
            if d[o + 8:o + 16].hex() in keys:
                recs.append(d[o:end])
            o = end
        with open(os.path.join(out, name), 'wb') as f:
            f.write(b''.join(recs))
        total += len(recs)
        print(f'{name}: {len(recs)} de {len(keys)}')
    print(f'{total} shaders en {out}')


main()
