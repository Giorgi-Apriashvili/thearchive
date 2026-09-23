# Test images for profiles.sh, built with the standard library only — so the suite does
# not depend on the vips command-line tool, which Debian ships separately from the
# library. `png` can embed an EXIF block carrying a marker string, which is how the
# suite proves a picture's metadata never reaches other members.
import struct, sys, zlib

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

def exif_with(text):
    # A minimal little-endian TIFF: one IFD entry, ImageDescription (0x010E), ASCII.
    t = text.encode() + b'\0'
    data_off = 8 + 2 + 12 + 4
    return (b'II*\x00' + struct.pack('<I', 8) + struct.pack('<H', 1)
            + struct.pack('<HHII', 0x010E, 2, len(t), data_off) + struct.pack('<I', 0) + t)

def png(width, height, exif=None, claim=None):
    w, h = claim or (width, height)
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)          # 8-bit RGB
    row = b'\x00' + b''.join(bytes((x * 7 % 256, (x * 3) % 256, 90)) for x in range(width))
    raw = row * height
    out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
    if exif:
        out += chunk(b'eXIf', exif_with(exif))
    return out + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')

def png_1bit(width, height):
    """A valid 1-bit greyscale PNG. All-black rows compress to almost nothing, so a
    genuinely huge image fits in a few kilobytes — which is what makes it a fair test of
    a pixel limit: this one decodes perfectly well if nothing stops it."""
    ihdr = struct.pack('>IIBBBBB', width, height, 1, 0, 0, 0, 0)
    row = b'\x00' + b'\x00' * ((width + 7) // 8)
    z = zlib.compressobj(9)
    body = b''.join(z.compress(row) for _ in range(height)) + z.flush()
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', body)
            + chunk(b'IEND', b''))

def webp_info(data):
    """(width, height, has_exif_chunk) of a WebP file."""
    assert data[:4] == b'RIFF' and data[8:12] == b'WEBP', 'not webp'
    has_exif, w, h = False, None, None
    pos = 12
    while pos + 8 <= len(data):
        kind, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if kind == b'EXIF':
            has_exif = True
        elif kind == b'VP8X':
            w = 1 + int.from_bytes(body[4:7], 'little'); h = 1 + int.from_bytes(body[7:10], 'little')
        elif kind == b'VP8 ' and w is None:
            w = struct.unpack('<H', body[6:8])[0] & 0x3fff; h = struct.unpack('<H', body[8:10])[0] & 0x3fff
        elif kind == b'VP8L' and w is None:
            bits = int.from_bytes(body[1:5], 'little'); w = (bits & 0x3fff) + 1; h = ((bits >> 14) & 0x3fff) + 1
        pos += 8 + size + (size & 1)
    return w, h, has_exif

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'png':           # png OUT W H [EXIF-TEXT]
        sys.stdout.buffer.write(png(int(sys.argv[2]), int(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else None))
    elif cmd == 'bomb':        # a tiny file whose header claims 100,000 x 100,000
        sys.stdout.buffer.write(png(4, 4, claim=(100000, 100000)))
    elif cmd == 'png1bit':     # png1bit W H
        sys.stdout.buffer.write(png_1bit(int(sys.argv[2]), int(sys.argv[3])))
    elif cmd == 'info':
        w, h, e = webp_info(sys.stdin.buffer.read()); print(w, h, 'exif' if e else 'no-exif')
