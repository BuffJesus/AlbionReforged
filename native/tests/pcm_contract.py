"""Device-free PCM contract and optional parity with Python's WAVE reader."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import wave


def chunk(tag, payload):
    return tag + struct.pack('<I', len(payload)) + payload + b'\0' * (len(payload) & 1)


def riff(chunks):
    body = b'WAVE' + b''.join(chunks)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def format_bytes(channels=2, rate=48000, bits=16, tag=1, alignment=None, byte_rate=None):
    alignment = channels * bits // 8 if alignment is None else alignment
    byte_rate = rate * alignment if byte_rate is None else byte_rate
    return struct.pack('<HHIIHH', tag, channels, rate, byte_rate, alignment, bits)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--corpus', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    samples = struct.pack('<8h', -32768, 32767, -1, 0, 1, 127, 2048, -2048)
    fmt = format_bytes()
    guid = bytes.fromhex('0100000000001000800000aa00389b71')
    ext = format_bytes(tag=0xfffe) + struct.pack('<HHI', 22, 16, 3) + guid
    base_chunks = [chunk(b'fmt ', fmt), chunk(b'data', samples)]
    valid = riff(base_chunks)
    records = []

    def check(label, data, code=None, expected=samples, channels=2, rate=48000, mask=0, budget=None, reference_data=None):
        path = args.output / (label + '.wav')
        path.write_bytes(data)
        command = [str(args.probe), str(path)] + ([] if budget is None else [str(budget)])
        result = subprocess.run(command, capture_output=True, timeout=30)
        assert result.returncode == (0 if code is None else 20 + code), (label, result.returncode, result.stderr)
        assert result.stdout == (expected if code is None else b''), label
        if code is None:
            metadata = tuple(map(int, result.stderr.decode().split()))
            assert metadata == (channels, rate, mask, len(expected) // (2 * channels)), (label, metadata)
            with wave.open(io.BytesIO(data if reference_data is None else reference_data), 'rb') as reference:
                assert reference.readframes(reference.getnframes()) == expected
        records.append({'case': label, 'exit_code': result.returncode, 'passed': True})

    check('pcm16_stereo', valid)
    check('pcm16_mono', riff([chunk(b'fmt ', format_bytes(1, 22050)), chunk(b'data', samples)]), channels=1, rate=22050)
    check('extensible_pcm16', riff([chunk(b'fmt ', ext), chunk(b'data', samples)]), mask=3)
    check('empty_pcm', riff([chunk(b'fmt ', fmt), chunk(b'data', b'')]), expected=b'')
    check('unknown_odd_chunk', riff([chunk(b'JUNK', b'abc')] + base_chunks))
    check('fmt_extension_zero', riff([chunk(b'fmt ', fmt + b'\0\0'), chunk(b'data', samples)]))
    # Native accepts data-before-format, which Python wave intentionally rejects.
    # Compare its samples against the normal-order reference.
    check('data_before_format', riff(list(reversed(base_chunks))), reference_data=valid)
    check('wrong_signature', b'RIFX' + valid[4:], code=2)
    check('short_header', valid[:11], code=2)
    check('riff_size_large', valid[:4] + struct.pack('<I', 0xffffffff) + valid[8:], code=3)
    check('riff_size_small', valid[:4] + struct.pack('<I', 4) + valid[8:], code=3)
    check('trailing_input', valid + b'garbage', code=3)
    check('short_chunk_header', riff(base_chunks + [b'JUNK']), code=3)
    check('truncated_payload', riff([b'fmt ' + struct.pack('<I', 0xffffffff) + fmt]), code=3)
    check('missing_odd_pad', riff(base_chunks + [b'JUNK' + struct.pack('<I', 1) + b'x']), code=3)
    check('missing_format', riff([base_chunks[1]]), code=3)
    check('missing_data', riff([base_chunks[0]]), code=3)
    check('duplicate_format', riff([base_chunks[0]] + base_chunks), code=8)
    check('duplicate_data', riff(base_chunks + [base_chunks[1]]), code=8)
    check('short_format', riff([chunk(b'fmt ', fmt[:15]), base_chunks[1]]), code=3)
    check('zero_rate', riff([chunk(b'fmt ', format_bytes(rate=0)), base_chunks[1]]), code=3)
    check('zero_channels', riff([chunk(b'fmt ', format_bytes(channels=0)), base_chunks[1]]), code=3)
    check('wrong_alignment', riff([chunk(b'fmt ', format_bytes(alignment=2)), base_chunks[1]]), code=3)
    check('wrong_byte_rate', riff([chunk(b'fmt ', format_bytes(byte_rate=48000)), base_chunks[1]]), code=3)
    check('rate_overflow', riff([chunk(b'fmt ', format_bytes(rate=0xffffffff, byte_rate=0xfffffffc)), base_chunks[1]]), code=3)
    check('partial_frame', riff([base_chunks[0], chunk(b'data', samples[:-2])]), code=6)
    check('pcm_budget', valid, code=4, budget=len(samples) - 1)
    check('pcm_budget_exact', valid, budget=len(samples))
    check('non_pcm', riff([chunk(b'fmt ', format_bytes(tag=3)), base_chunks[1]]), code=2)
    check('pcm8', riff([chunk(b'fmt ', format_bytes(bits=8)), base_chunks[1]]), code=2)
    check('extension_length_short', riff([chunk(b'fmt ', fmt + b'\0'), base_chunks[1]]), code=3)
    check('extension_length_large', riff([chunk(b'fmt ', fmt + b'\xff\xff'), base_chunks[1]]), code=3)
    check('extensible_missing', riff([chunk(b'fmt ', format_bytes(tag=0xfffe)), base_chunks[1]]), code=3)
    check('extensible_float', riff([chunk(b'fmt ', ext[:24] + b'\3' + ext[25:]), base_chunks[1]]), code=2)
    check('extensible_valid_bits', riff([chunk(b'fmt ', ext[:18] + b'\x0c\0' + ext[20:]), base_chunks[1]]), code=2)

    corpus = []
    if args.corpus:
        for path in sorted(args.corpus.rglob('*.wav')):
            data = path.read_bytes()
            with wave.open(io.BytesIO(data), 'rb') as reference:
                assert reference.getsampwidth() == 2
                expected = reference.readframes(reference.getnframes())
                channels, rate = reference.getnchannels(), reference.getframerate()
            result = subprocess.run([str(args.probe), str(path)], capture_output=True, timeout=30)
            assert result.returncode == 0, (path, result.stderr)
            assert result.stdout == expected, path
            meta = tuple(map(int, result.stderr.decode().split()))
            assert meta[:2] == (channels, rate) and meta[3] == len(expected) // (channels * 2), path
            corpus.append({'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(),
                           'pcm_sha256': hashlib.sha256(expected).hexdigest(),
                           'channels': channels, 'sample_rate': rate, 'frames': meta[3], 'passed': True})
    report = {'contract': records, 'corpus': corpus, 'passed': True}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS {len(records)} PCM contract cases; {len(corpus)} cooked WAVs matched')


if __name__ == '__main__':
    main()
