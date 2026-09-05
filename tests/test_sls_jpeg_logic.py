"""Host-side regression checks for the SLS collector and JPEG bit boundary rules.

These tests intentionally do not run on the radio and add no diagnostic path to
the firmware.  The small models mirror the production state transitions so the
protocol edge cases can be exercised with Python's standard library.
"""

from __future__ import annotations

import unittest
from io import BytesIO
from pathlib import Path

from jpeg_fixture_matrix import DHT, make_jpeg, remove_header_marker, scan_count

try:
    from PIL import Image
except ImportError:  # Optional reference decoder used by the extended matrix.
    Image = None


ROOT = Path(__file__).resolve().parents[1]


class MotCollectorModel:
    CAPACITY = 50 * 1024
    MAX_SEGMENTS = 256
    MAX_SEGMENT_SIZE = 2048

    def __init__(self) -> None:
        self.last_completed: int | None = None
        self.published_pending = False
        self.update = False
        self.published_data = b""
        self.reset()

    def reset(self) -> None:
        self.transport_id: int | None = None
        self.body_size: int | None = None
        self.last_segment: int | None = None
        self.lengths = [0] * self.MAX_SEGMENTS
        self.data = bytearray()

    @property
    def byte_count(self) -> int:
        return len(self.data)

    def _lock(self, transport_id: int) -> None:
        self.reset()
        self.transport_id = transport_id

    def header(self, transport_id: int, body_size: int) -> bool:
        if not 0 < body_size <= self.CAPACITY:
            return False
        if transport_id == self.last_completed:
            return False
        if self.published_pending or self.update:
            return False
        if self.transport_id is not None and transport_id != self.transport_id:
            self._lock(transport_id)
        if self.transport_id is None:
            self._lock(transport_id)
        if self.body_size is not None and self.body_size != body_size:
            self._lock(transport_id)
        self.body_size = body_size
        return self.complete

    def segment(
        self, transport_id: int, number: int, payload: bytes, *, last: bool = False
    ) -> str:
        if not 0 <= number < self.MAX_SEGMENTS:
            return "invalid"
        if not 0 < len(payload) <= self.MAX_SEGMENT_SIZE:
            return "invalid"
        if transport_id == self.last_completed:
            return "completed"
        if self.published_pending or self.update:
            return "published"
        if (
            self.transport_id is not None
            and transport_id != self.transport_id
            and number == 0
        ):
            self._lock(transport_id)
        if self.transport_id is None:
            self._lock(transport_id)
        if transport_id != self.transport_id:
            return "foreign"
        if self.lengths[number]:
            return "duplicate"
        if (
            (last and self.last_segment is not None and number != self.last_segment)
            or (self.last_segment is not None and number > self.last_segment)
            or (last and self.byte_count and number < self.highest_segment)
            or self.byte_count + len(payload) > self.CAPACITY
        ):
            self.reset()
            return "invalid"

        insert_at = sum(self.lengths[:number])
        self.data[insert_at:insert_at] = payload
        self.lengths[number] = len(payload)
        if last:
            self.last_segment = number
        if self.body_size is not None and self.byte_count > self.body_size:
            self.reset()
            return "invalid"
        return "complete" if self.complete else "stored"

    def publish(self) -> None:
        if not self.complete or self.transport_id is None:
            raise AssertionError("cannot publish incomplete object")
        self.last_completed = self.transport_id
        self.published_data = bytes(self.data)
        self.published_pending = True
        self.update = True

    def acknowledge(self) -> None:
        self.update = False
        self.published_pending = False

    @property
    def highest_segment(self) -> int:
        return max((i for i, length in enumerate(self.lengths) if length), default=0)

    @property
    def complete(self) -> bool:
        if self.last_segment is not None:
            count = self.last_segment + 1
        elif self.byte_count:
            count = self.highest_segment + 1
        else:
            return False
        if any(not self.lengths[i] for i in range(count)):
            return False
        if self.body_size is not None:
            return self.byte_count == self.body_size
        return self.last_segment is not None


class EntropyReaderModel:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0
        self.buf = 0
        self.bits = 0
        self.pending: int | None = None
        self.eof = False

    def _read(self) -> int | None:
        if self.pos == len(self.data):
            return None
        value = self.data[self.pos]
        self.pos += 1
        return value

    def entropy_byte(self) -> int | None:
        if self.pending is not None or self.eof:
            return None
        value = self._read()
        if value is None:
            self.eof = True
            return None
        if value != 0xFF:
            return value
        marker = self._read()
        while marker == 0xFF:
            marker = self._read()
        if marker is None:
            self.eof = True
            return None
        if marker == 0:
            return 0xFF
        self.pending = marker
        return None

    def fill(self, required: int) -> bool:
        while self.bits < required:
            value = self.entropy_byte()
            if value is None:
                return False
            self.buf = (self.buf << 8) | value
            self.bits += 8
        return True

    def get_bits(self, count: int) -> int | None:
        if not self.fill(count):
            return None
        self.bits -= count
        return (self.buf >> self.bits) & ((1 << count) - 1)

    def boundary_marker(self) -> int | None:
        if self.bits > 7:
            return None
        self.bits = 0
        self.buf = 0
        if self.pending is not None:
            marker, self.pending = self.pending, None
            return marker
        prefix = self._read()
        if prefix != 0xFF:
            return None
        marker = self._read()
        while marker == 0xFF:
            marker = self._read()
        return marker if marker not in (None, 0) else None


class MotCollectorTests(unittest.TestCase):
    def test_start_in_middle_of_carousel(self) -> None:
        c = MotCollectorModel()
        c.header(0x1234, 80)
        for number in range(37, 80):
            c.segment(0x1234, number, bytes([number]), last=number == 79)
        for number in range(37):
            result = c.segment(0x1234, number, bytes([number]))
        self.assertEqual(result, "complete")
        self.assertEqual(c.data, bytes(range(80)))

    def test_out_of_order_segments_are_assembled_by_number(self) -> None:
        c = MotCollectorModel()
        c.header(7, 10)
        for number in (5, 2, 9, 0, 1, 8, 3, 4, 6, 7):
            c.segment(7, number, bytes([number]), last=number == 9)
        self.assertTrue(c.complete)
        self.assertEqual(c.data, bytes(range(10)))

    def test_duplicate_does_not_increment_byte_count(self) -> None:
        c = MotCollectorModel()
        self.assertEqual(c.segment(1, 0, b"abc"), "stored")
        self.assertEqual(c.segment(1, 0, b"abc"), "duplicate")
        self.assertEqual(c.byte_count, 3)

    def test_foreign_header_switches_without_mixing(self) -> None:
        c = MotCollectorModel()
        c.header(10, 4)
        c.segment(10, 0, b"AA")
        self.assertFalse(c.header(11, 4))
        self.assertEqual(c.transport_id, 11)
        self.assertEqual(c.segment(11, 0, b"BB"), "stored")
        self.assertEqual(c.segment(11, 1, b"bb", last=True), "complete")
        self.assertEqual(c.data, b"BBbb")

    def test_late_header_preserves_segments(self) -> None:
        c = MotCollectorModel()
        c.segment(22, 1, b"B")
        c.segment(22, 0, b"A")
        self.assertTrue(c.header(22, 2))
        self.assertEqual(c.data, b"AB")

    def test_tune_mid_object_switches_at_next_header(self) -> None:
        c = MotCollectorModel()
        c.segment(31, 5, b"F")
        c.segment(31, 6, b"G")
        c.segment(31, 7, b"H", last=True)
        self.assertFalse(c.header(32, 4))
        self.assertEqual(c.transport_id, 32)
        self.assertEqual(c.data, b"")
        self.assertEqual(c.segment(32, 0, b"A"), "stored")
        self.assertEqual(c.segment(32, 1, b"B"), "stored")
        self.assertEqual(c.segment(32, 2, b"C"), "stored")
        self.assertEqual(c.segment(32, 3, b"D", last=True), "complete")

    def test_foreign_segment_zero_switches_but_later_segment_does_not(self) -> None:
        c = MotCollectorModel()
        c.segment(100, 2, b"C")
        self.assertEqual(c.segment(202, 8, b"x"), "foreign")
        self.assertEqual(c.transport_id, 100)
        self.assertEqual(c.segment(101, 0, b"A"), "stored")
        self.assertEqual(c.transport_id, 101)
        self.assertEqual(c.data, b"A")

    def test_published_buffer_waits_for_ui_and_completed_tid_is_ignored(self) -> None:
        c = MotCollectorModel()
        c.header(101, 2)
        c.segment(101, 0, b"A")
        self.assertEqual(c.segment(101, 1, b"B", last=True), "complete")
        c.publish()
        original = c.published_data
        self.assertFalse(c.header(102, 2))
        self.assertEqual(c.segment(102, 0, b"X"), "published")
        self.assertEqual(c.published_data, original)
        self.assertEqual(c.segment(101, 0, b"A"), "completed")
        c.acknowledge()
        self.assertFalse(c.header(102, 2))
        self.assertEqual(c.segment(102, 0, b"X"), "stored")

    def test_transport_id_zero_is_valid(self) -> None:
        c = MotCollectorModel()
        c.header(0, 2)
        c.segment(0, 0, b"A")
        self.assertEqual(c.segment(0, 1, b"B", last=True), "complete")
        self.assertEqual(c.transport_id, 0)

    def test_full_capacity_with_variable_segment_lengths(self) -> None:
        c = MotCollectorModel()
        lengths = [500 + (i % 5) * 6 for i in range(100)]
        self.assertEqual(sum(lengths), c.CAPACITY)
        parts = [bytes([i % 251]) * length for i, length in enumerate(lengths)]
        c.header(99, c.CAPACITY)
        for number in reversed(range(len(parts))):
            result = c.segment(99, number, parts[number], last=number == 99)
        self.assertEqual(result, "complete")
        self.assertEqual(c.byte_count, c.CAPACITY)
        self.assertEqual(c.data, b"".join(parts))

    def test_ui_duplicate_short_circuits_render_and_fade(self) -> None:
        displayed = (4, hash(b"same"))
        calls: list[str] = []

        def process(image: bytes) -> None:
            fingerprint = (len(image), hash(image))
            if fingerprint == displayed:
                return
            calls.extend(("fadeDown", "ShowSlideShow", "fadeUp"))

        process(b"same")
        self.assertEqual(calls, [])


class JpegBitBoundaryTests(unittest.TestCase):
    def test_pending_eoi_keeps_valid_read_ahead_bits(self) -> None:
        reader = EntropyReaderModel(bytes((0b01111111, 0xFF, 0xD9)))
        self.assertFalse(reader.fill(16))
        self.assertEqual(reader.pending, 0xD9)
        self.assertEqual(reader.get_bits(1), 0)
        self.assertEqual(reader.boundary_marker(), 0xD9)

    def test_pending_eoi_cannot_supply_synthetic_zero_bits(self) -> None:
        reader = EntropyReaderModel(bytes((0b10111111, 0xFF, 0xD9)))
        self.assertFalse(reader.fill(16))
        self.assertIsNone(reader.get_bits(9))

    def test_stuffed_ff_is_entropy_data(self) -> None:
        reader = EntropyReaderModel(bytes((0xFF, 0x00, 0xFF, 0xD9)))
        self.assertFalse(reader.fill(16))
        self.assertEqual(reader.get_bits(8), 0xFF)
        self.assertEqual(reader.boundary_marker(), 0xD9)

    def test_restart_fill_bytes_and_sequence_boundary(self) -> None:
        reader = EntropyReaderModel(
            bytes((0b01111111, 0xFF, 0xFF, 0xD0, 0b01111111, 0xFF, 0xD9))
        )
        self.assertFalse(reader.fill(16))
        self.assertEqual(reader.get_bits(1), 0)
        self.assertEqual(reader.boundary_marker(), 0xD0)
        self.assertFalse(reader.fill(16))
        self.assertEqual(reader.get_bits(1), 0)
        self.assertEqual(reader.boundary_marker(), 0xD9)

    def test_boundary_rejects_unconsumed_byte_but_tolerates_padding_value(self) -> None:
        full_byte = EntropyReaderModel(bytes((0xFF, 0x00, 0xFF, 0xD9)))
        self.assertFalse(full_byte.fill(16))
        self.assertIsNone(full_byte.boundary_marker())

        zero_padding = EntropyReaderModel(bytes((0b10000000, 0xFF, 0xD9)))
        self.assertFalse(zero_padding.fill(16))
        self.assertEqual(zero_padding.get_bits(1), 1)
        self.assertEqual(zero_padding.boundary_marker(), 0xD9)

    def test_production_sources_keep_required_states(self) -> None:
        jpeg = (ROOT / "src" / "JPEGdecoder.cpp").read_text(encoding="utf-8")
        mot = (ROOT / "src" / "si4684.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "si4684.h").read_text(encoding="utf-8")
        self.assertIn("pendingMarker", jpeg)
        self.assertNotIn("hitMarker", jpeg)
        self.assertIn("pjDecodeBaselineMultiPass", jpeg)
        self.assertIn("pjInstallStandardHuffman", jpeg)
        self.assertIn("uint8_t* nzBitmap = static_cast<uint8_t*>(calloc", jpeg)
        self.assertIn("storeSlideshowSegment", mot)
        self.assertIn("acknowledgeSlideshow", mot + header)
        self.assertIn("lastCompletedTransportId", mot + header)
        self.assertNotIn("slideshowSlotSize", mot + header)
        self.assertIn("SlideShowTransportIDValid", header)
        self.assertIn("50U * 1024U", header)


class SyntheticJpegMatrixTests(unittest.TestCase):
    CASES = {
        "baseline_444": dict(sampling="444"),
        "baseline_422": dict(sampling="422"),
        "baseline_420": dict(sampling="420"),
        "baseline_440": dict(sampling="440"),
        "baseline_411": dict(sampling="411"),
        "baseline_max10_blocks": dict(sampling="max10"),
        "baseline_gray": dict(sampling="gray"),
        "baseline_odd_size": dict(width=319, height=237, sampling="420"),
        "baseline_restart": dict(sampling="420", restart=7),
        "baseline_app_com_fill": dict(sampling="444", extras=True,
                                       marker_fill=True),
        "baseline_multi_tables": dict(sampling="420", multiple_tables=True),
        "baseline_separate_scans": dict(sampling="420", separate=True),
        "baseline_repeated_tables": dict(sampling="420", separate=True,
                                          repeat_tables=True),
        "progressive_444": dict(sampling="444", progressive=True),
        "progressive_420": dict(sampling="420", progressive=True),
        "progressive_gray": dict(sampling="gray", progressive=True),
        "progressive_refine": dict(sampling="420", progressive=True,
                                   refine=True),
        "progressive_repeated_tables": dict(sampling="420", progressive=True,
                                             refine=True, repeat_tables=True),
        "progressive_restart": dict(sampling="420", progressive=True,
                                    refine=True, restart=11),
    }

    def test_generated_scan_matrix_has_complete_marker_structure(self) -> None:
        for name, options in self.CASES.items():
            with self.subTest(name=name):
                jpeg = make_jpeg(**options)
                self.assertEqual(jpeg[:2], b"\xff\xd8")
                self.assertEqual(jpeg[-2:], b"\xff\xd9")
                scans = scan_count(jpeg)
                self.assertGreaterEqual(scans, 1)
                if options.get("separate"):
                    self.assertEqual(scans, 3)
                if options.get("progressive"):
                    self.assertGreaterEqual(scans, 3)

    @unittest.skipIf(Image is None, "Pillow reference decoder not installed")
    def test_reference_decoder_accepts_supported_matrix(self) -> None:
        for name, options in self.CASES.items():
            with self.subTest(name=name):
                jpeg = make_jpeg(**options)
                with Image.open(BytesIO(jpeg)) as image:
                    image.load()
                    self.assertEqual(image.size,
                                     (options.get("width", 320),
                                      options.get("height", 240)))

    @unittest.skipIf(Image is None, "Pillow reference decoder not installed")
    def test_reference_decoder_rejects_truncated_entropy(self) -> None:
        jpeg = make_jpeg(sampling="420", progressive=True, refine=True)
        with self.assertRaises(Exception):
            with Image.open(BytesIO(jpeg[:-17])) as image:
                image.load()

    @unittest.skipIf(Image is None, "Pillow reference decoder not installed")
    def test_reference_decoder_accepts_annex_k_defaults_without_dht(self) -> None:
        image = Image.new("RGB", (31, 29), (20, 90, 170))
        encoded = BytesIO()
        image.save(encoded, "JPEG", quality=83, subsampling=2,
                   optimize=False)
        without_dht = remove_header_marker(encoded.getvalue(), DHT)
        with Image.open(BytesIO(without_dht)) as decoded:
            decoded.load()
            self.assertEqual(decoded.size, (31, 29))

    def test_progressive_444_workspace_is_split_below_observed_limit(self) -> None:
        blocks_per_row = 40 * 3
        row_workspace = blocks_per_row * 64 * (2 + 1)
        nonzero_bitmap = (40 * 30 * 3) * 8
        old_combined = row_workspace + nonzero_bitmap
        self.assertEqual(row_workspace, 23040)
        self.assertEqual(nonzero_bitmap, 28800)
        self.assertGreater(old_combined, 51188)
        self.assertLess(row_workspace, 51188)
        self.assertLess(nonzero_bitmap, 51188)


if __name__ == "__main__":
    unittest.main()
