"""BT3 menu archive tools: BPE unpack, packs, texture files -> RGBA (after upstream port/src/gs/menu_labels.c)."""
import struct, numpy as np

kBlock32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
kCol32 = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
          [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47], [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]
kCol8First = [[0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54], [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
              [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23], [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31]]
kMlBlock4 = [[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15], [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]]
kMlCol4 = [[0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110],
           [16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126],
           [65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47],
           [81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63]]
GS_BYTES = 512 * 512 * 4

def u32(d, at): return struct.unpack_from('<I', d, at)[0]
def u64(d, at): return struct.unpack_from('<Q', d, at)[0]

def section(d, i):
    off = u32(d, i * 4) & ~3
    return d[off:]

def unpack(data):
    size, packed = u32(data, 0), u32(data, 4)
    end = 8 + packed
    p = 8; out = bytearray()
    while p < end:
        left = list(range(256)); right = [0] * 256; ch = 0
        while ch < 256:
            count = data[p]; p += 1
            if count > 127: ch += count - 127; count = 0
            if ch == 256: break
            for i in range(count + 1):
                left[ch] = data[p]; p += 1
                if ch != left[ch]: right[ch] = data[p]; p += 1
                ch += 1
        block = data[p] * 256 + data[p + 1]; p += 2
        for k in range(block):
            stack = [data[p + k]]
            while stack:
                v = stack.pop()
                if left[v] == v: out.append(v)
                else: stack.append(right[v]); stack.append(left[v])
        p += block
    assert len(out) == size, (len(out), size)
    return bytes(out)

def tex_count(d):
    """entries of a texture file (table word offset at +4; entries of 64 bytes up to the first pixel data)"""
    table = u32(d, 4) * 4
    first = min(u32(d, table + i * 64) * 4 for i in range(1)) if table + 64 <= len(d) else 0
    n = 0
    while table + (n + 1) * 64 <= len(d) and table + n * 64 < first:
        n += 1
    return n

def entry(d, index):
    table = u32(d, 4) * 4
    e = table + index * 64
    return dict(pix=u32(d, e), pal=u32(d, e + 4), blt=u32(d, e + 24), tex0=u64(d, e + 48), raw=d[e:e + 64], at=e)

def texture(d, index, palette_index=None):
    """-> (w, h, psm, rgba ndarray h x w x 4, map info) or None"""
    if palette_index is None: palette_index = index
    en = entry(d, index)
    tex0, blt, pixAt, palAt = en['tex0'], en['blt'], en['pix'], en['pal']
    if palette_index != index: palAt = entry(d, palette_index)['pal']
    w = 1 << ((tex0 >> 26) & 15); h = 1 << ((tex0 >> 30) & 15); psm = (tex0 >> 20) & 63
    if w > 512 or h > 512 or psm not in (19, 20) or blt >> 8 != 0: return None
    pixels = pixAt * 4 + 96; palette = palAt * 4 + 96
    reg = u64(d, pixels - 48)
    uw = reg & 4095; uh = (reg >> 32) & 4095
    dbw = blt & 63; tbw = (tex0 >> 14) & 63
    gs = np.full(GS_BYTES, -1, dtype=np.int64)
    for y in range(uh):
        for x in range(uw):
            a = ((((y & ~31) * dbw + ((x >> 1) & ~31) + kBlock32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + kCol32[y & 7][x & 7])
            at = (y * uw + x) * 4
            gs[a * 4:a * 4 + 4] = [at, at + 1, at + 2, at + 3]
    count = 256 if psm == 19 else 16
    colors = []
    for i in range(count):
        j = ((i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)) if psm == 19 else i
        colors.append(d[palette + j * 4:palette + j * 4 + 4])
    colors = np.frombuffer(b''.join(colors), dtype=np.uint8).reshape(count, 4)
    idx = np.zeros((h, w), dtype=np.uint8)
    srcmap = np.zeros((h, w, 2), dtype=np.int64)
    for y in range(h):
        for x in range(w):
            col = (y & 15) >> 2; sx = x ^ (4 if (col & 1) else 0)
            if psm == 19:
                a = (((((y >> 1) & ~31) * (tbw >> 1) + ((x >> 2) & ~31) + kBlock32[(y >> 4) & 3][(x >> 4) & 7]) << 8) + kCol8First[y & 3][sx & 15] + col * 64)
                address, shift = a, 0
            else:
                a = (((((y >> 2) & ~31) * (tbw >> 1) + ((x >> 2) & ~31) + kMlBlock4[(y >> 4) & 7][(x >> 5) & 3]) << 9) + kMlCol4[y & 3][sx & 31] + col * 128)
                address, shift = a >> 1, (a & 1) * 4
            g = gs[address]
            if g < 0: return None
            v = (d[pixels + g] >> shift) & (count - 1)
            idx[y, x] = v
            srcmap[y, x] = (pixels + g, shift)
    rgba = colors[idx]
    return dict(w=w, h=h, psm=psm, rgba=rgba, idx=idx, colors=colors, srcmap=srcmap, pixels=pixels, palette=palette)
