"""Small deterministic SOF0/SOF2 JPEG fixture generator.

The entropy stream represents an all-zero DCT image.  Keeping the encoder
minimal makes scan topology, restart placement, padding and malformed variants
fully controllable without storing binary fixtures in the repository.
"""

from __future__ import annotations

from dataclasses import dataclass


SOI = 0xD8
EOI = 0xD9
SOF0 = 0xC0
SOF2 = 0xC2
DHT = 0xC4
RST0 = 0xD0
SOS = 0xDA
DQT = 0xDB
DRI = 0xDD


@dataclass(frozen=True)
class Component:
    ident: int
    h: int
    v: int
    quant: int = 0


SAMPLING = {
    "gray": (Component(1, 1, 1),),
    "444": (Component(1, 1, 1), Component(2, 1, 1), Component(3, 1, 1)),
    "422": (Component(1, 2, 1), Component(2, 1, 1), Component(3, 1, 1)),
    "420": (Component(1, 2, 2), Component(2, 1, 1), Component(3, 1, 1)),
    "440": (Component(1, 1, 2), Component(2, 1, 1), Component(3, 1, 1)),
    "411": (Component(1, 4, 1), Component(2, 1, 1), Component(3, 1, 1)),
    "max10": (Component(1, 4, 2), Component(2, 1, 1), Component(3, 1, 1)),
}


def _marker(code: int, payload: bytes = b"", *, fill: bool = False) -> bytes:
    prefix = b"\xff\xff" if fill else b"\xff"
    if code in (SOI, EOI) or RST0 <= code <= RST0 + 7:
        return prefix + bytes((code,))
    return prefix + bytes((code,)) + (len(payload) + 2).to_bytes(2, "big") + payload


def _pack_bits(bits: str) -> bytes:
    bits += "1" * ((-len(bits)) & 7)
    raw = bytes(int(bits[pos : pos + 8], 2) for pos in range(0, len(bits), 8))
    return raw.replace(b"\xff", b"\xff\x00")


def _entropy(groups: list[str], restart: int) -> bytes:
    if restart <= 0:
        return _pack_bits("".join(groups))
    encoded = bytearray()
    rst = 0
    for start in range(0, len(groups), restart):
        encoded += _pack_bits("".join(groups[start : start + restart]))
        if start + restart < len(groups):
            encoded += _marker(RST0 + rst)
            rst = (rst + 1) & 7
    return bytes(encoded)


def _geometry(width: int, height: int, components: tuple[Component, ...]):
    max_h = max(component.h for component in components)
    max_v = max(component.v for component in components)
    mcu_x = (width + max_h * 8 - 1) // (max_h * 8)
    mcu_y = (height + max_v * 8 - 1) // (max_v * 8)
    return max_h, max_v, mcu_x, mcu_y


def _scan_units(width: int, height: int, components: tuple[Component, ...],
                selected: tuple[int, ...], bits_per_block: int) -> list[str]:
    max_h, max_v, mcu_x, mcu_y = _geometry(width, height, components)
    if len(selected) > 1:
        blocks = sum(components[index].h * components[index].v for index in selected)
        return ["0" * (blocks * bits_per_block)] * (mcu_x * mcu_y)

    component = components[selected[0]]
    block_cols = (width * component.h + max_h * 8 - 1) // (max_h * 8)
    block_rows = (height * component.v + max_v * 8 - 1) // (max_v * 8)
    return ["0" * bits_per_block] * (block_cols * block_rows)


def _sos(components: tuple[Component, ...], selected: tuple[int, ...],
         ss: int, se: int, ah: int, al: int) -> bytes:
    payload = bytearray((len(selected),))
    for index in selected:
        payload += bytes((components[index].ident, 0x00))
    payload += bytes((ss, se, (ah << 4) | al))
    return _marker(SOS, bytes(payload))


def make_jpeg(*, width: int = 320, height: int = 240,
              sampling: str = "420", progressive: bool = False,
              separate: bool = False, restart: int = 0,
              refine: bool = False, extras: bool = False,
              multiple_tables: bool = False, marker_fill: bool = False,
              repeat_tables: bool = False) -> bytes:
    components = SAMPLING[sampling]
    _, _, _, _ = _geometry(width, height, components)

    quant_table = bytes((0,)) + bytes((1,)) * 64
    if multiple_tables:
        quant_table += bytes((1,)) + bytes((2,)) * 64

    # One-symbol canonical tables: DC category 0 and AC EOB both use code 0.
    counts = bytes((1,)) + bytes(15)
    huffman = bytes((0x00,)) + counts + bytes((0,))
    huffman += bytes((0x10,)) + counts + bytes((0x00,))
    if multiple_tables:
        huffman += bytes((0x01,)) + counts + bytes((0,))
        huffman += bytes((0x11,)) + counts + bytes((0x00,))

    frame = bytearray((8,))
    frame += height.to_bytes(2, "big") + width.to_bytes(2, "big")
    frame += bytes((len(components),))
    for component in components:
        frame += bytes((component.ident, (component.h << 4) | component.v,
                        component.quant))

    result = bytearray(_marker(SOI))
    if extras:
        result += _marker(0xE0, b"JFIF\x00\x01\x02\x00\x00\x01\x00\x01\x00\x00")
        result += _marker(0xE2, b"synthetic-app2")
        result += _marker(0xFE, b"synthetic comment", fill=marker_fill)
    result += _marker(DQT, quant_table)
    result += _marker(SOF2 if progressive else SOF0, bytes(frame))
    result += _marker(DHT, huffman)
    if restart:
        result += _marker(DRI, restart.to_bytes(2, "big"))

    all_components = tuple(range(len(components)))

    scan_number = 0

    def add_scan(selected: tuple[int, ...], ss: int, se: int,
                 ah: int, al: int, bits_per_block: int) -> None:
        nonlocal result, scan_number
        if repeat_tables and scan_number:
            result += _marker(DQT, quant_table)
            result += _marker(DHT, huffman)
        result += _sos(components, selected, ss, se, ah, al)
        groups = _scan_units(width, height, components, selected, bits_per_block)
        result += _entropy(groups, restart)
        scan_number += 1

    if not progressive:
        if separate and len(components) > 1:
            for component_index in all_components:
                add_scan((component_index,), 0, 63, 0, 0, 2)
        else:
            add_scan(all_components, 0, 63, 0, 0, 2)
    else:
        first_al = 1 if refine else 0
        add_scan(all_components, 0, 0, 0, first_al, 1)
        for component_index in all_components:
            add_scan((component_index,), 1, 5, 0, first_al, 1)
            add_scan((component_index,), 6, 63, 0, first_al, 1)
        if refine:
            add_scan(all_components, 0, 0, 1, 0, 1)
            for component_index in all_components:
                add_scan((component_index,), 1, 5, 1, 0, 1)
                add_scan((component_index,), 6, 63, 1, 0, 1)

    result += _marker(EOI, fill=marker_fill)
    return bytes(result)


def scan_count(jpeg: bytes) -> int:
    """Count SOS markers while respecting stuffed bytes and restart markers."""
    pos = 2
    scans = 0
    in_entropy = False
    while pos < len(jpeg):
        if jpeg[pos] != 0xFF:
            if not in_entropy:
                raise ValueError("data outside entropy segment")
            pos += 1
            continue
        while pos < len(jpeg) and jpeg[pos] == 0xFF:
            pos += 1
        if pos >= len(jpeg):
            raise ValueError("truncated marker")
        marker = jpeg[pos]
        pos += 1
        if in_entropy and marker == 0x00:
            continue
        if RST0 <= marker <= RST0 + 7:
            if not in_entropy:
                raise ValueError("restart outside entropy segment")
            continue
        in_entropy = False
        if marker == EOI:
            return scans
        if marker == SOI or marker == 0x01:
            continue
        if pos + 2 > len(jpeg):
            raise ValueError("truncated segment length")
        length = int.from_bytes(jpeg[pos : pos + 2], "big")
        if length < 2 or pos + length > len(jpeg):
            raise ValueError("invalid segment length")
        pos += length
        if marker == SOS:
            scans += 1
            in_entropy = True
    raise ValueError("missing EOI")


def remove_header_marker(jpeg: bytes, marker_to_remove: int) -> bytes:
    """Remove matching length-coded segments before the first SOS."""
    if jpeg[:2] != b"\xff\xd8":
        raise ValueError("missing SOI")
    output = bytearray(jpeg[:2])
    pos = 2
    while pos < len(jpeg):
        start = pos
        if jpeg[pos] != 0xFF:
            raise ValueError("data before SOS")
        while pos < len(jpeg) and jpeg[pos] == 0xFF:
            pos += 1
        if pos >= len(jpeg):
            raise ValueError("truncated marker")
        marker = jpeg[pos]
        pos += 1
        if marker == SOS:
            output += jpeg[start:]
            return bytes(output)
        if marker == SOI or marker == 0x01:
            output += jpeg[start:pos]
            continue
        if pos + 2 > len(jpeg):
            raise ValueError("truncated segment")
        length = int.from_bytes(jpeg[pos : pos + 2], "big")
        end = pos + length
        if length < 2 or end > len(jpeg):
            raise ValueError("invalid segment")
        if marker != marker_to_remove:
            output += jpeg[start:end]
        pos = end
    raise ValueError("missing SOS")
