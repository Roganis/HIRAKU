# SPDX-License-Identifier: GPL-3.0-or-later
"""Konami's IFS packs, read: their members' names and bytes (no images decoded).

A pack: a header (0x6CAD8F89, its version, the end of its file list), a binary XML (kbin) file list
whose files are three s32 (where from the list's end, the size, a time) and folders their nodes,
then the files. Names are written with _E for '.', __ for '_' and a '_' before a leading digit.
The kbin reading follows runtime/python/kbin.cpp's.
"""
import struct

SIX = "0123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz"
# kbin types: id -> (size of one, count; -1 a u32 length first)
TYPES = {1: (0, 0), 2: (1, 1), 3: (1, 1), 4: (2, 1), 5: (2, 1), 6: (4, 1), 7: (4, 1), 8: (8, 1), 9: (8, 1), 10: (1, -1), 11: (1, -1),
         12: (4, 1), 13: (4, 1), 14: (4, 1), 15: (8, 1), 52: (1, 1)}
for i, sz in enumerate((1, 1, 2, 2, 4, 4, 8, 8, 4, 8)):   # 2..4 of each: ids 16-25, 26-35, 36-45
    for n in (2, 3, 4):
        TYPES[16 + (n - 2) * 10 + i] = (sz, n)


def kbin(b):
    """the binary XML as nested (name, type, values, children) tuples; the root node"""
    if len(b) < 12 or b[0] != 0xA0 or b[1] not in (0x42, 0x45) or b[2] ^ b[3] != 0xFF:
        raise ValueError("not a binary XML")
    six = b[1] == 0x42
    node_end = 8 + struct.unpack_from(">I", b, 4)[0]
    data = node_end + 4
    st = {"off": 0, "byte": 0, "word": 0}

    def align(o):
        return (o + 3) & ~3

    def aligned(sz):
        if st["byte"] % 4 == 0:
            st["byte"] = st["off"]
        if st["word"] % 4 == 0:
            st["word"] = st["off"]
        if sz == 1:
            at = st["byte"]; st["byte"] += 1
        elif sz == 2:
            at = st["word"]; st["word"] += 2
        else:
            at = st["off"]; st["off"] = align(st["off"] + sz)
        trailing = max(st["byte"], st["word"])
        if st["off"] < trailing:
            st["off"] = align(trailing)
        return data + at

    def sized():
        n = struct.unpack_from(">I", b, data + st["off"])[0]
        v = b[data + st["off"] + 4:data + st["off"] + 4 + n]
        st["off"] = align(st["off"] + 4 + n)
        return v

    root = ["", "void", None, []]
    stack = [root]
    i = 8
    while i < node_end:
        t = b[i]
        i += 1
        if t == 0:
            continue
        arr, t = t & 64, t & ~64
        if t == 190:
            if len(stack) > 1:
                stack.pop()
            continue
        if t == 191:
            break
        if six:
            n = b[i]; nb = (n * 6 + 7) // 8
            bits = int.from_bytes(b[i + 1:i + 1 + nb], "big") >> (nb * 8 - n * 6) if n else 0
            name = "".join(SIX[(bits >> (6 * (n - 1 - k))) & 63] for k in range(n))
            i += 1 + nb
        else:
            n = (b[i] & ~64) + 1
            name = b[i + 1:i + 1 + n].decode("cp932", "replace")
            i += 1 + n
        if t == 46:   # an attribute
            sized()
            continue
        if t not in TYPES:
            raise ValueError("kbin type %d" % t)
        sz, count = TYPES[t]
        node = [name, t, None, []]
        stack[-1][3].append(node)
        stack.append(node)
        if t == 1:
            continue
        if count < 0:
            node[2] = sized()
            continue
        if arr:
            n = struct.unpack_from(">I", b, data + st["off"])[0]
            at = data + st["off"] + 4
            st["off"] = align(st["off"] + 4 + n)
            total = n // sz
        else:
            total = count
            at = aligned(sz * count)
        fmt = {1: "b", 2: "h", 4: "i", 8: "q"}[sz]
        node[2] = struct.unpack_from(">%d%s" % (total, fmt), b, at)
    return root[3][0] if root[3] else None


def plain(name):
    if len(name) > 1 and name[0] == "_" and name[1].isdigit():
        name = name[1:]
    return name.replace("_E", ".").replace("__", "_")


class Pack:
    """a pack's members: path ('a/b.ifs') -> (where in the file, size)"""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            head = f.read(0x24)
            sig, ver, nver, _, tree, data_at = struct.unpack_from(">IHHIII", head)
            if sig != 0x6CAD8F89 or ver ^ nver != 0xFFFF:
                raise ValueError(path + ": not an IFS pack")
            start = 0x24 if ver > 1 else 0x14
            f.seek(start)
            listing = f.read(data_at - start)
        self.members = {}

        def walk(node, prefix):
            for k in node[3]:
                if k[0] in ("_info_", "_super_"):
                    continue
                if k[3]:   # a folder (its value a time)
                    walk(k, prefix + plain(k[0]) + "/")
                elif k[1] == 30 and k[2]:   # 3s32: where, size, time
                    self.members[prefix + plain(k[0])] = (data_at + k[2][0], k[2][1])
        walk(kbin(listing), "")

    def read(self, name):
        at, size = self.members[name]
        with open(self.path, "rb") as f:
            f.seek(at)
            return f.read(size)
