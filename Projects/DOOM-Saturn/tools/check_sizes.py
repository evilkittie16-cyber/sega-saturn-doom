import os, glob

for i in range(1, 10):
    m = f"E1M{i}"
    files = glob.glob(f"cd/{m}.*")
    total = sum(os.path.getsize(f) for f in files)
    print(f"{m}: {len(files)} files, {total:,} bytes ({total/1024:.1f} KB)")
    for f in sorted(files):
        print(f"   {os.path.basename(f):10s}: {os.path.getsize(f):>10,} bytes")
