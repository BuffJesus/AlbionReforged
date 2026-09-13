import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def decode(blob):
    rows, offset = [], 0
    while offset < len(blob):
        ordinal, terminated, size = struct.unpack_from('>III', blob, offset)
        offset += 12
        assert size <= len(blob)-offset
        rows.append((ordinal, bool(terminated), blob[offset:offset+size]))
        offset += size
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    args = parser.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='tu1-manifest-') as folder:
        path = Path(folder)/'input'
        def run(blob, expected=None, error=None, chunk=131072, budget=16777216, records=1000000, line=2048):
            nonlocal count
            path.write_bytes(blob)
            result = subprocess.run([args.probe,str(path),str(chunk),str(budget),str(records),str(line)], capture_output=True)
            count += 1
            if error is not None:
                assert result.returncode == 20+error and not result.stdout, (count,result)
            else:
                assert result.returncode == 0, (count,result)
                assert decode(result.stdout) == expected, (count,decode(result.stdout),expected)
        for chunk in [1, 2, 3, 7, 131072]:
            run(b'',[],chunk=chunk)
            run(b'\r\n\n\r',[],chunk=chunk)
            run(b'A/B\r\na\\b\nA/B\r tail ',[(1,True,b'A/B'),(2,True,b'a\\b'),(3,True,b'A/B'),(4,False,b' tail ')],chunk=chunk)
            run(b'last',[(1,False,b'last')],chunk=chunk)
            run(b'a\n\0',error=2,chunk=chunk)
            run(b'a\n\xff',error=2,chunk=chunk)
        for tail in [b'', b'\n', b'\r\n']:
            run(b'a'*2048+tail,[(1,bool(tail),b'a'*2048)])
            run(b'a'*2049+tail,error=4)
        run(b'a\nb',error=4,records=1)
        run(b'a',error=4,records=0)
        run(b'',[],records=0,budget=0,line=0)
        run(b'\n',[],line=0)
        run(b'a',error=4,line=0)
        run(b'a\n',error=4,budget=1)
        run(b'a',error=4,chunk=0)
        run(b'a',error=4,chunk=131073)
        run(b'a',error=4,line=2049)
        blob = b'x\n'*65535 + b'ABC\r\nZ'
        expected = [(i+1,True,b'x') for i in range(65535)] + [(65536,True,b'ABC'),(65537,False,b'Z')]
        run(blob,expected)
    print(f'{count} manifest framing cases passed')


if __name__ == '__main__':
    main()
