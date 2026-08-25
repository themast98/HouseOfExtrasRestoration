"""SLLZ round-trip check against a real shipped asset.

The v1 bitstream has an off-by-one trap (the flag byte is refilled before the
eighth token's payload, not after it), so a decoder that looks right can still
desynchronise at the first group boundary. This decodes a known asset and
checks the result is a plausible csb, which any desynchronised decoder fails.
"""

import pathlib
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import sllz  # noqa: E402

SAMPLE = ROOT / "refs" / "pjs_net_ranking.csb.sllz"


def test_sllz_decodes_a_shipped_layout():
    if not SAMPLE.exists():
        pytest.skip("refs/pjs_net_ranking.csb.sllz missing - see refs/README.md")
    raw = SAMPLE.read_bytes()
    assert raw[:4] == b"SLLZ", "sample is not SLLZ-compressed"
    out = sllz.decompress(raw)
    # csb files are big-endian 'NBSC' containers; a desynchronised decode
    # produces neither the magic nor the right length.
    assert out[:4] == b"NBSC", f"got magic {out[:4]!r}"
    import struct
    usize = struct.unpack_from("<I", raw, 8)[0]
    assert len(out) == usize, f"decoded {len(out)} bytes, header says {usize}"


def test_plain_data_passes_through():
    assert sllz.decompress(b"NBSC\x00\x00\x00\x02rest") == b"NBSC\x00\x00\x00\x02rest"
