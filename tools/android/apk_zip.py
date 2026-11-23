# apk_zip.py <base.apk> <carpeta dex> <carpeta lib> <salida.apk>: el APK sin alinear ni firmar. Copia lo que hizo aapt2
# (el manifiesto, resources.arsc SIN comprimir —desde la API 30 se exige así—, los recursos y los assets) tal cual, con
# su compresión, y añade el código Java (classes.dex) y las bibliotecas nativas (lib/<abi>/*.so, comprimidas: el
# manifiesto pide extractNativeLibs). Después: zipalign y apksigner (tools/android/apk.sh).
import os
import sys
import zipfile

base, dexdir, libdir, out = sys.argv[1:5]
with zipfile.ZipFile(base) as zin, zipfile.ZipFile(out, 'w') as zout:
    for item in zin.infolist():
        zout.writestr(item, zin.read(item.filename), compress_type=item.compress_type)
    for f in sorted(os.listdir(dexdir)):
        zout.write(os.path.join(dexdir, f), f, compress_type=zipfile.ZIP_DEFLATED)
    top = os.path.dirname(os.path.abspath(libdir))
    for root, _, files in sorted(os.walk(libdir)):
        for f in sorted(files):
            p = os.path.join(root, f)
            zout.write(p, os.path.relpath(p, top).replace(os.sep, '/'), compress_type=zipfile.ZIP_DEFLATED)
print(f'apk_zip: {out}')
