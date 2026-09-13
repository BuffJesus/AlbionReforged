import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('capture', Path(__file__).resolve().parents[2] / 'tools/read_tu1_table_capture.py')
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureFileContract(unittest.TestCase):
    def fixture(self):
        mount = struct.pack('>I', 0x1234) + bytes(28) + struct.pack('>i', -9)
        records = struct.pack('>9I', 7, 0, 9, 7, 0, 4, 0xffffffff, 0, 2)
        return b'TU1TAB01' + struct.pack('>4I', 0x100, 7, len(mount), len(records)) + mount + records

    def test_selection(self):
        report = capture.decode(self.fixture())
        self.assertEqual(report['first_equal_selection'], {'mount': 0, 'entry': 9})
        self.assertEqual(report['mounts'][0]['priority'], -9)
        self.assertEqual(report['adjacent_equal_hash_pairs'], 1)

    def test_every_truncation(self):
        blob = self.fixture()
        for end in range(len(blob)):
            with self.subTest(end=end), self.assertRaises(ValueError):
                capture.decode(blob[:end])

    def test_invalid(self):
        blob = self.fixture()
        variants = [blob + b'x', b'X' + blob[1:]]
        for offset, value in [(8, 0), (16, 37), (16, 36 * 257), (20, 12 * 65537),
                              (64, 1), (72, 6)]:
            bad = bytearray(blob)
            struct.pack_into('>I', bad, offset, value)
            variants.append(bad)
        for bad in variants:
            with self.subTest(blob=bad), self.assertRaises(ValueError):
                capture.decode(bad)

    def test_v2_names(self):
        v1 = self.fixture()
        raw_name = 'data\\test\U0001f43a.bnk'.encode('utf-16-be')
        v2 = b'TU1TAB02' + v1[8:] + struct.pack('>III', 3, 0x8200dea0, len(raw_name)//2) + raw_name
        report = capture.decode(v2)
        self.assertEqual(report['mounts'][0]['observed_open_name'], 'data\\test\U0001f43a.bnk')
        for end in range(len(v1), len(v2)):
            with self.subTest(end=end), self.assertRaises(ValueError):
                capture.decode(v2[:end])
        for status, vtable, units, name in [(4, 0, 0, b''), (0, 0, 1, b'\0a'),
                (3, 0, 0, b''), (3, 0x8200dea0, 513, b''),
                (3, 0x8200dea0, 1, b'\xd8\0'), (3, 0x8200dea0, 1, b'\0\0')]:
            with self.subTest(status=status, name=name), self.assertRaises(ValueError):
                capture.decode(b'TU1TAB02' + v1[8:] + struct.pack('>III',status,vtable,units) + name)


if __name__ == '__main__':
    unittest.main()
