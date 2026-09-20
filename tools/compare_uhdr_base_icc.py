"""Diagnostic only: replace a JPEG base ICC with the supplied HEIC's base P3 ICC.

Keeps entropy-coded image data and all gain-map values untouched, adjusts MPF
primary length, and refuses to overwrite output. Never used in product exports.
"""
from pathlib import Path
import struct
import sys


def boxes(data, start, end):
    while start + 8 <= end:
        size, kind = struct.unpack_from('>I4s', data, start)
        header = 8
        if size == 1:
            size = struct.unpack_from('>Q', data, start + 8)[0]
            header = 16
        if size == 0:
            size = end - start
        if size < header or start + size > end:
            raise ValueError('Invalid ISO BMFF box')
        yield kind, data[start + header:start + size]
        if kind in (b'meta', b'iprp', b'ipco'):
            yield from boxes(data, start + header + (4 if kind == b'meta' else 0), start + size)
        start += size


def jpeg_headers(data):
    assert data[:2] == b'\xff\xd8'
    at = 2
    while at + 4 <= len(data):
        assert data[at] == 255
        marker = data[at + 1]
        if marker in (0xda, 0xd9):
            break
        size = int.from_bytes(data[at + 2:at + 4], 'big')
        assert 2 <= size <= len(data) - at - 2
        yield marker, at, size, data[at + 4:at + size + 2]
        at += size + 2


def main():
    jpeg, heic, output = map(Path, sys.argv[1:4])
    src = jpeg.read_bytes()
    heif = heic.read_bytes()
    profile = heif if heic.suffix.lower() == '.icc' else next(
        payload[4:] for kind, payload in boxes(heif, 0, len(heif))
        if kind == b'colr' and payload[:4] == b'prof')
    assert len(profile) >= 132 and profile[16:20] == b'RGB '
    if len(sys.argv) == 5:
        # Diagnostic isolation: copy only selected standard numeric tags, not
        # the Apple description/header/HAGC. Never shipped as a product asset.
        mode = sys.argv[4]
        original = next(v[3][14:] for v in jpeg_headers(src)
                        if v[0] == 0xe2 and v[3].startswith(b'ICC_PROFILE\0'))
        def tags(p):
            return [(sig, p[o:o+n]) for sig, o, n in (
                struct.unpack_from('>4sII', p, 132 + 12*i)
                for i in range(int.from_bytes(p[128:132], 'big')))]
        replacements = dict(tags(profile))
        chosen = ({b'rXYZ', b'gXYZ', b'bXYZ'} if 'matrix' in mode else set()) | (
            {b'rTRC', b'gTRC', b'bTRC'} if 'trc' in mode else set())
        fields = [(s, replacements[s] if s in chosen else v) for s, v in tags(original)]
        at = 132 + len(fields)*12
        table = bytearray()
        data = bytearray()
        for sig, value in fields:
            table.extend(struct.pack('>4sII', sig, at, len(value)))
            value += b'\0' * (-len(value) % 4)
            data.extend(value)
            at += len(value)
        header = bytearray(original[:132])
        struct.pack_into('>I', header, 0, at)
        profile = bytes(header + table + data)
    # This fixture's first colr profile was independently identified as Display P3.
    payload = b'ICC_PROFILE\0\1\1' + profile
    icc = b'\xff\xe2' + struct.pack('>H', len(payload) + 2) + payload
    _, at, size, _ = next(v for v in jpeg_headers(src)
                         if v[0] == 0xe2 and v[3].startswith(b'ICC_PROFILE\0'))
    delta = len(icc) - (size + 2)
    result = bytearray(src[:at] + icc + src[at + size + 2:])
    _, mpf_at, _, mpf = next(v for v in jpeg_headers(result)
                            if v[0] == 0xe2 and v[3].startswith(b'MPF\0'))
    tiff = mpf[4:]
    endian = '>' if tiff[:2] == b'MM' else '<'
    ifd = struct.unpack_from(endian + 'I', tiff, 4)[0]
    count = struct.unpack_from(endian + 'H', tiff, ifd)[0]
    for i in range(count):
        tag, _, size, offset = struct.unpack_from(endian + 'HHII', tiff, ifd + 2 + i * 12)
        if tag == 0xb002:
            old_size = struct.unpack_from(endian + 'I', tiff, offset + 4)[0]
            struct.pack_into(endian + 'I', result, mpf_at + 8 + offset + 4, old_size + delta)
            # MPF itself moved by delta, so its relative secondary offset is unchanged.
            break
    else:
        raise ValueError('Missing MP entries')
    with output.open('xb') as target:
        target.write(result)
    print(f'base_icc_bytes={len(profile)} JPEG_delta={delta} bytes={len(result)} output={output}')


if __name__ == '__main__':
    main()
