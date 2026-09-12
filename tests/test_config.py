"""The ini the plugin writes on a fresh install must equal the configuration
every mode was validated with.

WHY THIS EXISTS. The C++ defaults and the validated ini drifted: NetRankingPanel
stayed at its old value 0 in config.h (from when the panel still crashed) while
every test session ran with NetRankingPanel=1 in a hand-edited ini. The first
clean install then wrote a fresh ini FROM the defaults and Battle King /
Fastest Killer silently ended with no panel. Two rules stop that class of bug:

  1. every key Load() reads is written by WriteDefaults(), in the same section,
     so a fresh ini documents the whole configuration; and
  2. the value WriteDefaults() writes equals the C++ default of the field the
     key is read into, so the ini and the binary can never disagree.
"""

import pathlib
import re

import pytest

SRC = pathlib.Path(__file__).resolve().parents[1] / "src"


def _cpp_defaults():
    text = (SRC / "config.h").read_text(encoding="utf-8")
    return {m.group(1): int(m.group(2), 0)
            for m in re.finditer(r"^\s*int\s+(\w+)\s*=\s*(-?(?:0x[0-9A-Fa-f]+|\d+))\s*;", text, re.M)}


def _load_reads():
    """key -> (section, field) for every GetPrivateProfileIntA in Load()."""
    text = (SRC / "config.cpp").read_text(encoding="utf-8")
    body = text[text.index("Settings Load()"):]
    reads = {}
    for m in re.finditer(r'GetPrivateProfileIntA\("(\w+)",\s*"(\w+)",\s*s\.(\w+)', body):
        reads[m.group(2)] = (m.group(1), m.group(3))
    return reads


def _written_defaults():
    """key -> (section, value) as WriteDefaults() lays the file out."""
    text = (SRC / "config.cpp").read_text(encoding="utf-8")
    body = text[text.index("void WriteDefaults"):text.index("Settings Load()")]
    section, out = None, {}
    for m in re.finditer(r'"((?:\\n)?)(\[\w+\]|\w+=[^\\"]+)\\n"', body):
        tok = m.group(2)
        if tok.startswith("["):
            section = tok.strip("[]")
        else:
            k, v = tok.split("=", 1)
            assert k not in out, f"{k} written twice"
            out[k] = (section, int(v, 0))
    return out


def test_every_read_key_is_written_in_its_section():
    reads, written = _load_reads(), _written_defaults()
    missing = sorted(k for k in reads if k not in written)
    assert not missing, f"read by Load() but never written to the default ini: {missing}"
    wrong = sorted(f"{k}: read from [{reads[k][0]}], written under [{written[k][0]}]"
                   for k in reads if written[k][0] != reads[k][0])
    assert not wrong, wrong


def test_every_written_key_is_read():
    reads, written = _load_reads(), _written_defaults()
    unread = sorted(k for k in written if k not in reads)
    assert not unread, f"documented in the ini but never read (falls back silently): {unread}"


def test_written_value_equals_cpp_default():
    reads, written, defaults = _load_reads(), _written_defaults(), _cpp_defaults()
    bad = []
    for key, (_, field) in reads.items():
        if key not in written:
            continue
        assert field in defaults, f"{field} has no int default in config.h"
        if written[key][1] != defaults[field]:
            bad.append(f"{key}: ini writes {written[key][1]}, config.h default is {defaults[field]}")
    assert not bad, bad


@pytest.mark.parametrize("key,expected", [
    ("NetRankingPanel", 1),      # the regression that motivated this file
    ("ChaseRankPanel", 1),
    ("EndlessTagPanel", 1),
    ("UnlockAllModes", 1),
    ("AmebaFloor", 1),
    ("PostModeBobFlags", 1),
])
def test_ps3_parity_switches_default_on(key, expected):
    assert _written_defaults()[key][1] == expected
