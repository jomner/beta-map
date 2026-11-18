"""Feeds beta-map-draw damaged region files: random bytes flipped in the chunk table and in chunk data, files cut
short, chunk lengths and offsets pointing anywhere, and chunks of valid zlib around nonsense NBT. Build the drawer
with the sanitizers first; any crash, sanitizer report or hang is a failure. Exit code 0 only if there were none.

    cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o /tmp/bmd-asan beta-map-draw.c -lz -lm
    python3 tests/fuzz.py /tmp/bmd-asan path/to/a/real/r.0.0.mcr 150"""
import os, random, shutil, struct, subprocess, sys, tempfile, zlib

drawer, src, rounds = sys.argv[1], sys.argv[2], int(sys.argv[3])
base = open(src, 'rb').read()
rnd = random.Random(1)
work = tempfile.mkdtemp(prefix='bm-fuzz-')


def nbt_garbage():
    """valid zlib around NBT that's nearly right: a real-looking Level with wrong types, sizes and nesting"""
    parts = [b'\x0a\x00\x00', b'\x0a\x00\x05Level']
    for _ in range(rnd.randint(0, 6)):
        t = rnd.choice([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0, 13, 255])
        name = rnd.choice([b'Blocks', b'Data', b'x', b'Level', b''])
        parts.append(bytes([t]) + struct.pack('>H', len(name)) + name)
        parts.append(os.urandom(rnd.randint(0, 40)))
        if rnd.random() < 0.3: parts.append(struct.pack('>i', rnd.choice([32768, 16384, -1, 2 ** 31 - 1, 0, 5])))
    return zlib.compress(b''.join(parts))


fails = 0
for i in range(rounds):
    d = bytearray(base)
    kind = rnd.choice(['flip', 'table', 'cut', 'lengths', 'nbt'])
    if kind == 'flip':
        for _ in range(rnd.randint(1, 200)): d[rnd.randrange(len(d))] = rnd.randrange(256)
    elif kind == 'table':
        for _ in range(rnd.randint(1, 50)): d[rnd.randrange(8192)] = rnd.randrange(256)
    elif kind == 'cut':
        d = d[:rnd.randint(8193, len(d))]
    elif kind == 'lengths':
        for _ in range(rnd.randint(1, 40)):
            j = rnd.randrange(1024); loc = struct.unpack('>I', d[4 * j:4 * j + 4])[0]
            if loc:
                off = (loc >> 8) * 4096
                if off + 5 <= len(d): d[off:off + 4] = struct.pack('>I', rnd.choice([0, 1, 2, 0xffffffff, rnd.randrange(1 << 24)]))
    else:
        for _ in range(rnd.randint(1, 30)):
            j = rnd.randrange(1024); loc = struct.unpack('>I', d[4 * j:4 * j + 4])[0]
            if loc:
                off = (loc >> 8) * 4096; z = nbt_garbage()
                if off + 5 + len(z) <= len(d): d[off:off + 5 + len(z)] = struct.pack('>I', len(z) + 1) + b'\x02' + z
    shutil.rmtree(work, ignore_errors=True); os.makedirs(os.path.join(work, 'world', 'region'))
    open(os.path.join(work, 'world', 'region', 'r.0.0.mcr'), 'wb').write(bytes(d))
    try:
        r = subprocess.run([drawer, os.path.join(work, 'world'), os.path.join(work, 'cache'), os.path.join(work, 'store')],
                           capture_output=True, timeout=300)
        err = r.stderr.decode('utf-8', 'replace')
        if r.returncode or 'Sanitizer' in err or 'runtime error' in err:
            fails += 1; print('round %d (%s): exit %d\n%s' % (i, kind, r.returncode, err[-1500:]), flush=True)
            open('fuzzfail-%d.mcr' % i, 'wb').write(bytes(d))
    except subprocess.TimeoutExpired:
        fails += 1; print('round %d (%s): HANG' % (i, kind), flush=True)
shutil.rmtree(work, ignore_errors=True)
print('fuzz: %d rounds, %d failures' % (rounds, fails))
sys.exit(1 if fails else 0)
