"""Compares two beta-map sites pixel for pixel: every tile PNG (whatever folder map.json "tiles" names) and map.json's
layers. Exit code 0 only if they're the same.

    python3 tests/compare.py SITE_A SITE_B"""
import json, os, struct, sys, zlib


def pixels(path):
    """(width, height, bytes per pixel, pixels) of one of our own PNGs (8 bit RGB or RGBA, filter 0)"""
    d = open(path, 'rb').read(); pos, idat, w, h, ct = 8, b'', 0, 0, 2
    while pos < len(d):
        n = struct.unpack('>I', d[pos:pos + 4])[0]; t = d[pos + 4:pos + 8]
        if t == b'IHDR': w, h, _, ct = struct.unpack('>IIBB', d[pos + 8:pos + 18])
        elif t == b'IDAT': idat += d[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat); n = 4 if ct == 6 else 3; row = w * n + 1
    assert all(raw[y * row] == 0 for y in range(h)), path + ': not filter 0'
    return w, h, n, b''.join(raw[y * row + 1:(y + 1) * row] for y in range(h))


def tiles_of(site):
    m = json.load(open(os.path.join(site, 'map.json')))
    base = os.path.join(site, m.get('tiles') or 'tiles/' + m['id'])
    files = {}
    for dp, dirs, fs in os.walk(base):
        dirs[:] = [d for d in dirs if not d.startswith('.')]   # (beta-map-draw's staging folder)
        for f in fs:
            if f.endswith('.png'): files[os.path.relpath(os.path.join(dp, f), base).replace(os.sep, '/')] = os.path.join(dp, f)
    return m, files


def main():
    ma, fa = tiles_of(sys.argv[1]); mb, fb = tiles_of(sys.argv[2])
    print('tiles: %d vs %d' % (len(fa), len(fb)))
    only_a, only_b = sorted(set(fa) - set(fb)), sorted(set(fb) - set(fa))
    if only_a: print('only in A (%d):' % len(only_a), only_a[:10])
    if only_b: print('only in B (%d):' % len(only_b), only_b[:10])
    bad = 0
    for k in sorted(set(fa) & set(fb)):
        a, b = pixels(fa[k]), pixels(fb[k])
        if a == b: continue
        bad += 1
        if bad <= 10:
            if a[:3] != b[:3]: print('DIFF %s: size or type %s vs %s' % (k, a[:3], b[:3])); continue
            pa, pb, n = a[3], b[3], a[2]
            diff = [i for i in range(len(pa)) if pa[i] != pb[i]]; px = diff[0] // n
            print('DIFF %s: %d bytes differ, first at pixel (%d, %d): %s vs %s' % (k, len(diff), px % a[0], px // a[0],
                  tuple(pa[px * n:(px + 1) * n]), tuple(pb[px * n:(px + 1) * n])))
    print('pixel differences in %d of %d tiles both have' % (bad, len(set(fa) & set(fb))))
    same_layers = ma['layers'] == mb['layers']
    if not same_layers:
        for k in sorted(set(ma['layers']) | set(mb['layers'])):
            if ma['layers'].get(k) != mb['layers'].get(k):
                print('layer %s: %s vs %s' % (k, str(ma['layers'].get(k))[:100], str(mb['layers'].get(k))[:100]))
    print('map.json layers: %s' % ('the same' if same_layers else 'DIFFERENT'))
    sys.exit(0 if not bad and not only_a and not only_b and same_layers else 1)


if __name__ == '__main__':
    main()
