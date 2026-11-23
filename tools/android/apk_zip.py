# apk_zip.py <base.apk> <dex folder> <lib folder> <output.apk>: the APK, neither aligned nor signed. It copies what aapt2 made
# (the manifest, resources.arsc UNCOMPRESSED -required since API 30-, the resources and the assets) as is, with its
# compression, and adds the Java code (classes.dex) and the native libraries (lib/<abi>/*.so, compressed: the manifest asks
# for extractNativeLibs). Next: zipalign and apksigner (tools/android/apk.sh).
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
