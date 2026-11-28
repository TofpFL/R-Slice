import zipfile, os, time, sys

root = r'C:\Users\YalishannaZhou\Downloads\R-Slice-Main'
out = sys.argv[1] if len(sys.argv) > 1 else r'F:\assets-vslice.zip'
t0 = time.time()
n = 0
with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED, allowZip64=True) as z:
    for top in ('assets', 'art'):
        for dirpath, dirs, files in os.walk(os.path.join(root, top)):
            dirs.sort()
            for f in sorted(files):
                full = os.path.join(dirpath, f)
                rel = os.path.relpath(full, root).replace(os.sep, '/')
                z.write(full, rel)
                n += 1
                if n % 500 == 0:
                    print(n, 'files', round(time.time() - t0), 's', flush=True)
print('DONE', n, 'files', round(os.path.getsize(out) / 1e9, 2), 'GB in', round(time.time() - t0), 's')
