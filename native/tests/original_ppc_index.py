"""Bounded instruction oracle for TU1 lookup and the mount-priority branch.

Executes original bytes, not generated C++ or reconstructed binary-search code.
This is intentionally not an emulator for the rest of the game: every unhandled
opcode, memory access, branch target and excessive step count is an error.
The independent linear/bisect specification in the caller also checks this oracle.
"""
import hashlib

TEXT_BASE = 0x82170000
TEXT_SHA256 = '1b9f2e80280637fe2287286ed3fe18b367f378e24a695a2b8ab50b9acd8fc724'
MASK64 = (1 << 64) - 1


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class OriginalIndex:
    def __init__(self, text):
        if hashlib.sha256(text).hexdigest() != TEXT_SHA256:
            raise ValueError('original TU1 text identity mismatch')
        self.text = text
        self.steps = 0

    def execute(self, start, end, registers, memory, exits=()):
        pc, cr, carry = start, [False] * 32, 0

        def read(address, size):
            if address < 0 or address + size > len(memory):
                raise ValueError('oracle memory read out of range')
            return int.from_bytes(memory[address:address + size], 'big')

        def write(address, size, value):
            if address < 0 or address + size > len(memory):
                raise ValueError('oracle memory write out of range')
            memory[address:address + size] = (value & ((1 << (size * 8)) - 1)).to_bytes(size, 'big')

        def compare(field, a, b):
            cr[field * 4:field * 4 + 4] = [a < b, a > b, a == b, False]

        for _ in range(2000):
            if pc in exits:
                return pc
            if not start <= pc < end or pc & 3:
                raise ValueError(f'oracle escaped supported block: {pc:08X}')
            word = int.from_bytes(self.text[pc - TEXT_BASE:pc - TEXT_BASE + 4], 'big')
            self.steps += 1
            op, rt, ra, rb = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
            displacement = signed(word, 16)
            base = registers[ra] if ra else 0
            next_pc = pc + 4
            if word == 0x4e800020:  # blr terminates the standalone leaf
                return next_pc
            elif op == 19 and (word >> 1) & 1023 == 16 and not word & 1:
                # Conditional bclr in a standalone leaf, no CTR decrement.
                if rt not in (4, 12):
                    raise ValueError('unsupported conditional return')
                if cr[ra] == (rt == 12):
                    return next_pc
            elif op in (32, 36):  # lwz, stw
                address = base + displacement
                if op == 32:
                    registers[rt] = read(address, 4)
                else:
                    write(address, 4, registers[rt])
            elif op in (58, 62) and word & 3 == 0:  # ld, std (DS form)
                address = base + signed(word & 0xfffc, 16)
                if op == 58:
                    registers[rt] = read(address, 8)
                else:
                    write(address, 8, registers[rt])
            elif op == 14:  # addi
                registers[rt] = (base + displacement) & MASK64
            elif op == 11 and not word & (1 << 21):  # cmpwi
                compare((word >> 23) & 7, signed(registers[ra], 32), displacement)
            elif op == 10 and not word & (1 << 21):  # cmplwi
                compare((word >> 23) & 7, registers[ra] & 0xffffffff, word & 0xffff)
            elif op == 16:  # bc: only CR tests, no CTR, relative, no link
                bo, bi = rt, ra
                if bo not in (4, 12) or word & 3:
                    raise ValueError('unsupported conditional branch')
                if cr[bi] == (bo == 12):
                    next_pc = pc + signed(word & 0xfffc, 16)
            elif op == 18 and word & 3 == 0:  # b relative, no link
                next_pc = pc + signed(word & 0x03fffffc, 26)
            elif op == 21:  # rlwinm (used as slwi); mask follows PPC bit numbering
                shift, mb, me = rb, (word >> 6) & 31, (word >> 1) & 31
                value = registers[rt] & 0xffffffff
                rotated = ((value << shift) | (value >> ((32 - shift) % 32))) & 0xffffffff
                bits = [i for i in range(32) if (mb <= i <= me if mb <= me else i >= mb or i <= me)]
                mask = sum(1 << (31 - i) for i in bits)
                registers[ra] = rotated & mask
                if word & 1:
                    raise ValueError('unsupported recording rotate')
            elif op == 31:
                xo = (word >> 1) & 1023
                if xo in (0, 32) and not word & (1 << 21):  # cmpw, cmplw
                    a, b = registers[ra], registers[rb]
                    compare((word >> 23) & 7, signed(a, 32) if xo == 0 else a & 0xffffffff,
                            signed(b, 32) if xo == 0 else b & 0xffffffff)
                elif xo == 40:  # subf
                    registers[rt] = (registers[rb] - registers[ra]) & MASK64
                elif xo == 266:  # add
                    registers[rt] = (registers[ra] + registers[rb]) & MASK64
                elif xo == 491:  # divw[.]
                    a, b = signed(registers[ra], 32), signed(registers[rb], 32)
                    if b == 0 or (a == -(1 << 31) and b == -1):
                        raise ValueError('undefined division in oracle')
                    result = abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
                    registers[rt] = result & MASK64
                    if word & 1:
                        compare(0, result, 0)
                elif xo == 824:  # srawi
                    value = signed(registers[rt], 32)
                    carry = int(value < 0 and (value & ((1 << rb) - 1)) != 0)
                    registers[ra] = (value >> rb) & MASK64
                elif xo == 202:  # addze
                    value = registers[ra] + carry
                    registers[rt], carry = value & MASK64, int(value > MASK64)
                elif xo == 444:  # or (mr alias)
                    registers[ra] = registers[rt] | registers[rb]
                else:
                    raise ValueError(f'unsupported oracle instruction {word:08X} at {pc:08X}')
            else:
                raise ValueError(f'unsupported oracle instruction {word:08X} at {pc:08X}')
            pc = next_pc
        raise ValueError('oracle instruction budget exceeded')

    def lower_bound(self, keys, query):
        # Packed iterators in r4/r5 = {container address, element address}.
        data = 0x400
        memory = bytearray(data + 12 * (len(keys) + 1))
        for i, key in enumerate(keys):
            memory[data + i * 12:data + i * 12 + 4] = key.to_bytes(4, 'big')
        memory[0x300:0x304] = query.to_bytes(4, 'big')
        r = [0] * 32
        r[1], r[3], r[6] = 0x100, 0x280, 0x300
        r[4], r[5] = (0x200 << 32) | data, (0x200 << 32) | (data + len(keys) * 12)
        self.execute(0x82B47240, 0x82B472B4, r, memory)
        container = int.from_bytes(memory[0x280:0x284], 'big')
        pointer = int.from_bytes(memory[0x284:0x288], 'big')
        if container != 0x200 or not data <= pointer <= data + len(keys) * 12 or (pointer-data) % 12:
            raise ValueError('invalid original lookup result')
        return (pointer - data) // 12

    def replaces(self, incoming, existing):
        memory = bytearray(0x200)
        memory[0x120:0x124] = (existing & 0xffffffff).to_bytes(4, 'big')
        r = [0] * 32
        r[10], r[20] = 0x100, incoming & MASK64
        result = self.execute(0x82B461F4, 0x82B46200, r, memory, exits=(0x82B461FC + 4, 0x82B462C4))
        return result == 0x82B46200
