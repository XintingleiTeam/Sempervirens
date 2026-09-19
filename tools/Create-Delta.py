"""Create Sempervirens' bounded copy/literal binary delta."""
import argparse
import hashlib
import struct
import zlib
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('old', type=Path)
parser.add_argument('new', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
old = args.old.read_bytes()
new = args.new.read_bytes()
window = 64
stride = 16
index = {}
for offset in range(0, max(0, len(old)-window+1), stride):
    index.setdefault(zlib.adler32(old[offset:offset+window]), []).append(offset)

stream = bytearray()
literal = bytearray()
def flush_literal():
    if literal:
        stream.append(1)
        stream.extend(struct.pack('<I', len(literal)))
        stream.extend(literal)
        literal.clear()

position = 0
while position < len(new):
    best_offset = best_length = 0
    if position + window <= len(new):
        key = zlib.adler32(new[position:position+window])
        for candidate in index.get(key, ())[:64]:
            if old[candidate:candidate+window] != new[position:position+window]:
                continue
            length = window
            maximum = min(len(old)-candidate, len(new)-position)
            while length < maximum and old[candidate+length] == new[position+length]:
                length += 1
            if length > best_length:
                best_offset, best_length = candidate, length
    if best_length >= window:
        flush_literal()
        stream.append(0)
        stream.extend(struct.pack('<QI', best_offset, best_length))
        position += best_length
    else:
        literal.append(new[position])
        position += 1
        if len(literal) == 1024*1024:
            flush_literal()
flush_literal()
stream.append(255)
compressed = zlib.compress(bytes(stream), 9)
header = (b'SVDIFF1\0' + struct.pack('<QQ', len(old), len(new)) +
          hashlib.sha256(old).hexdigest().encode('ascii') +
          hashlib.sha256(new).hexdigest().encode('ascii') +
          struct.pack('<QQ', len(stream), len(compressed)))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(header + compressed)
ratio = 100 * len(header+compressed) / max(1, len(new))
print(f'{args.output}: {len(header)+len(compressed)} bytes ({ratio:.1f}% of full file)')
