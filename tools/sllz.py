"""SLLZ decompressor for Yakuza archives.

Transcribed directly from the game's own decompressor rather than guessed:
  sub_F724E0  header parse + version dispatch
  sub_F72AF0  version 1 (LZ77)
  sub_F76F70  version 2 (zlib)

Header (16 bytes; big-endian fields when the endian byte is nonzero):
    0x00 char[4] 'SLLZ'
    0x04 u8       endian  (0 = little)
    0x05 u8       version (1 = LZ77, 2 = zlib)
    0x06 u16      header size (must be 0x10)
    0x08 u32      uncompressed size
    0x0C u32      compressed size

The v1 bitstream has one non-obvious property that breaks naive readers: the
flag byte is refilled *after the eighth bit is consumed but before that eighth
token's payload is read*, so the next flag byte physically precedes the last
token of the previous group. Refilling at the top of the loop - the obvious
implementation - desynchronises at the very first group boundary.
"""

import struct
import zlib


def _decompress_v1(src, usize):
    """Mirrors sub_F72AF0 instruction for instruction."""
    out = bytearray()
    i = 0
    flag = src[i]
    i += 1
    bits = 8
    produced = 0

    while produced < usize:
        is_match = (flag & 0x80) != 0      # test r9b,r9b / jns -> literal
        flag = (flag << 1) & 0xFF          # add r9d, r9d
        bits -= 1
        if bits == 0:                      # refill BEFORE reading the payload
            if i >= len(src):
                raise ValueError("input exhausted refilling flags")
            flag = src[i]
            i += 1
            bits = 8

        if is_match:
            if i + 1 >= len(src):
                raise ValueError("input exhausted reading a match token")
            b1, b2 = src[i], src[i + 1]
            i += 1                         # inc r8 inside the match path
            dist = ((b2 << 4) | (b1 >> 4)) + 1
            length = (b1 & 0x0F) + 3
            start = len(out) - dist
            if start < 0:
                raise ValueError(
                    f"match distance {dist} precedes output start at {len(out)}")
            for k in range(length):        # byte-wise, so runs may overlap
                out.append(out[start + k])
            produced += length
        else:
            if i >= len(src):
                raise ValueError("input exhausted reading a literal")
            out.append(src[i])
            produced += 1

        i += 1                             # inc r8 at the bottom of the loop

    return bytes(out[:usize])


def decompress(data):
    if data[:4] != b"SLLZ":
        return data                        # already plain
    endian = data[4]
    version = data[5]
    fmt = ">" if endian else "<"
    hdr = struct.unpack_from(fmt + "H", data, 6)[0]
    usize = struct.unpack_from(fmt + "I", data, 8)[0]
    csize = struct.unpack_from(fmt + "I", data, 12)[0]
    if hdr != 0x10:
        raise ValueError(f"unexpected SLLZ header size 0x{hdr:X}")

    body = data[hdr:hdr + csize] if csize else data[hdr:]
    if version == 1:
        return _decompress_v1(body, usize)
    if version == 2:
        return zlib.decompress(body)
    raise NotImplementedError(f"SLLZ version {version} not supported")


if __name__ == "__main__":
    import sys
    blob = open(sys.argv[1], "rb").read()
    out = decompress(blob)
    if len(sys.argv) > 2:
        open(sys.argv[2], "wb").write(out)
    print(f"{len(blob)} -> {len(out)} bytes; first 16: {out[:16].hex(' ')}")
