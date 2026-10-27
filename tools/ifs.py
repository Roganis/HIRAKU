# SPDX-License-Identifier: GPL-3.0-or-later
"""Konami's IFS packs: their members' names and bytes, and a pack written again with some members
replaced (rebuild); their binary XML (kbin) read and written; LZ77 (avslz) undone and done.

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
    """the binary XML as nested [name, type, values, children, attributes, array] lists; the root
    node (kbin.six, kbin.encoding: how it was written)"""
    if len(b) < 12 or b[0] != 0xA0 or b[1] not in (0x42, 0x45) or b[2] ^ b[3] != 0xFF:
        raise ValueError("not a binary XML")
    six = b[1] == 0x42
    kbin.six, kbin.encoding = six, b[2]
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

    root = ["", 1, None, [], [], 0]
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
        if t == 46:   # an attribute: its bytes as written
            stack[-1][4].append((name, sized()))
            continue
        if t not in TYPES:
            raise ValueError("kbin type %d" % t)
        sz, count = TYPES[t]
        node = [name, t, None, [], [], arr]
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
        node[2] = struct.unpack_from(">%d%s" % (total, FMT[t]), b, at)
    return root[3][0] if root[3] else None


# the values' struct formats (signed for s*, unsigned for u*: written back as read)
FMT = {}
for _t, (_sz, _n) in TYPES.items():
    if _sz:
        _signed = _t in (2, 4, 6, 8, 52) or (_t >= 16 and (_t - 16) % 10 % 2 == 0 and (_t - 16) % 10 < 8)
        FMT[_t] = {1: "b", 2: "h", 4: "i", 8: "q"}[_sz] if _signed else {1: "B", 2: "H", 4: "I", 8: "Q"}[_sz]
for _t in (14, 24, 34, 44):
    FMT[_t] = "f"
for _t in (15, 25, 35, 45):
    FMT[_t] = "d"


def kbin_write(root, six=True, encoding=0):
    """kbin() undone: the bytes of the binary XML"""
    nodes, data = bytearray(), bytearray()
    st = {"byte": 0, "word": 0}

    def pad(buf):
        buf += b"\0" * (-len(buf) % 4)

    def name_of(n):
        if six:
            bits = 0
            for ch in n:
                bits = bits << 6 | SIX.index(ch)
            nb = (len(n) * 6 + 7) // 8
            nodes.append(len(n))
            nodes.extend((bits << (nb * 8 - len(n) * 6)).to_bytes(nb, "big") if n else b"")
        else:
            raw = n.encode("cp932")
            nodes.append(len(raw) - 1 | 64)
            nodes.extend(raw)

    def sized(v):
        data.extend(struct.pack(">I", len(v)) + v)
        pad(data)

    def aligned(raw):
        if len(raw) == 1:
            if st["byte"] % 4 == 0:
                st["byte"] = len(data); data.extend(b"\0" * 4)
            data[st["byte"]] = raw[0]; st["byte"] += 1
        elif len(raw) == 2:
            if st["word"] % 4 == 0:
                st["word"] = len(data); data.extend(b"\0" * 4)
            data[st["word"]:st["word"] + 2] = raw; st["word"] += 2
        else:
            data.extend(raw)
            pad(data)

    def node(n):
        name, t, v, kids, attrs, arr = n
        nodes.append(t | (64 if arr else 0))
        name_of(name)
        if t != 1:
            sz, count = TYPES[t]
            if count < 0:
                sized(v)
            else:
                raw = struct.pack(">%d%s" % (len(v), FMT[t]), *v)
                sized(raw) if arr else aligned(raw)
        for an, av in attrs:
            nodes.append(46)
            name_of(an)
            sized(av)
        for k in kids:
            node(k)
        nodes.append(190 | 64)   # (written with the array bit, as Konami's are)
    node(root)
    nodes.append(191 | 64)
    pad(nodes)
    return bytes([0xA0, 0x42 if six else 0x45, encoding, encoding ^ 0xFF]) + struct.pack(">I", len(nodes)) + bytes(nodes) + \
        struct.pack(">I", len(data)) + bytes(data)


def lz77_undo(b):
    """avslz: the size undone, the size done, then flag bytes (bit set: a byte as is; clear: 12 bits
    back and 4 bits of length - 3; 0 ends)"""
    want = struct.unpack_from(">I", b, 0)[0]
    out, i = bytearray(), 8
    while len(out) < want and i < len(b):
        flag = b[i]; i += 1
        for k in range(8):
            if flag >> k & 1:
                out.append(b[i]); i += 1
            else:
                w = b[i] << 8 | b[i + 1]; i += 2
                if not w:
                    return bytes(out)
                back, n = w >> 4, (w & 15) + 3
                for _ in range(n):
                    out.append(out[-back] if back <= len(out) else 0)
            if len(out) >= want:
                break
    return bytes(out[:want])


def lz77_do(raw):
    """avslz done: the longest of the last few places the next 3 bytes were (up to 4095 back, 18
    long), else the byte as is"""
    out = bytearray(struct.pack(">II", len(raw), 0))
    where = {}
    i, n = 0, len(raw)
    while True:
        flag_at = len(out)
        out.append(0)
        flag = 0
        for k in range(8):
            if i >= n:
                out.extend(b"\0\0")   # a reference of 0: the end
                out[flag_at] = flag
                struct.pack_into(">I", out, 4, len(out) - 8)
                return bytes(out)
            best, at = 0, 0
            key = raw[i:i + 3]
            for c in where.get(key, ()) if len(key) == 3 else ():
                if i - c > 4095:
                    continue
                m = 3
                while m < 18 and i + m < n and raw[c + m] == raw[i + m]:
                    m += 1
                if m > best:
                    best, at = m, c
                    if m == 18:
                        break
            if best >= 3:
                w = (i - at) << 4 | (best - 3)
                out.extend((w >> 8, w & 255))
                step = best
            else:
                flag |= 1 << k
                out.append(raw[i])
                step = 1
            for j in range(i, min(i + step, n - 2)):
                l = where.setdefault(raw[j:j + 3], [])
                l.append(j)
                if len(l) > 4:
                    del l[0]
            i += step
        out[flag_at] = flag


def plain(name):
    if len(name) > 1 and name[0] == "_" and name[1].isdigit():
        name = name[1:]
    return name.replace("_E", ".").replace("__", "_")


def mangle(name):
    name = name.replace("_", "__").replace(".", "_E")
    return "_" + name if name[:1].isdigit() else name


class Pack:
    """a pack's members: path ('a/b.ifs') -> (where in the file, size)"""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.head = head = f.read(0x24)
            sig, ver, nver, _, tree, data_at = struct.unpack_from(">IHHIII", head)
            if sig != 0x6CAD8F89 or ver ^ nver != 0xFFFF:
                raise ValueError(path + ": not an IFS pack")
            start = 0x24 if ver > 1 else 0x14
            f.seek(start)
            self.listing = listing = f.read(data_at - start)
        self.version, self.data_at = ver, data_at
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

    def rebuild(self, replace):
        """the pack's bytes with the members in replace (path -> bytes) replaced: the same list,
        the files where they were in order, 4-byte aligned, the data to 16; _info_'s MD5 and size
        and the header's MD5 of the list made again"""
        import hashlib
        if self.version < 2:
            raise ValueError(self.path + ": an old pack (version 1)")
        root = kbin(self.listing)
        six, enc = kbin.six, kbin.encoding
        files = []

        def walk(node, prefix):
            for k in node[3]:
                if k[0] in ("_info_", "_super_"):
                    continue
                if k[3]:
                    walk(k, prefix + plain(k[0]) + "/")
                elif k[1] == 30 and k[2]:
                    files.append((k[2][0], prefix + plain(k[0]), k))
        walk(root, "")
        data = bytearray()
        for _, path, k in sorted(files, key=lambda f: f[0]):
            b = replace[path] if path in replace else self.read(path)
            k[2] = (len(data), len(b), k[2][2])
            data.extend(b)
            data.extend(b"\0" * (-len(data) % 4))
        data.extend(b"\0" * (-len(data) % 16))
        for k in root[3]:
            if k[0] == "_info_":
                for i in k[3]:
                    if i[0] == "md5":
                        i[2] = hashlib.md5(data).digest()
                    elif i[0] == "size":
                        i[2] = (len(data),)
        listing = kbin_write(root, six, enc)
        listing += b"\0" * (-(0x24 + len(listing)) % 16)   # the data from a 16-byte boundary
        head = bytearray(self.head)
        struct.pack_into(">I", head, 16, 0x24 + len(listing))
        head[0x14:0x24] = hashlib.md5(listing).digest()
        return bytes(head) + listing + bytes(data)


def write(members, when=0, tree=1212):
    """a new pack (version 3) of members [(path, bytes)], in that order: the folders as they first
    come; the header's tree size 2 per letter of the members' and folders' names and `tree` (what
    Konami's packs have: 1212 a character's, folders tex, afp, afp/bsi and geo; 1042 one picture's,
    tex only)"""
    import hashlib
    root = ["imgfs", 1, None, [], [], 0]
    info = ["_info_", 1, None, [["md5", 10, b"", [], [], 0], ["size", 7, (0,), [], [], 0]], [], 0]
    root[3].append(info)
    data, letters = bytearray(), 0
    folders = {"": root}
    for path, b in members:
        parts = path.split("/")
        for i in range(1, len(parts)):
            key = "/".join(parts[:i])
            if key not in folders:
                node = [mangle(parts[i - 1]), 6, (when,), [], [], 0]
                folders["/".join(parts[:i - 1])][3].append(node)
                folders[key] = node
                letters += len(parts[i - 1])
        folders["/".join(parts[:-1])][3].append([mangle(parts[-1]), 30, (len(data), len(b), when), [], [], 0])
        letters += len(parts[-1])
        data.extend(b)
        data.extend(b"\0" * (-len(data) % 4))
    data.extend(b"\0" * (-len(data) % 16))
    info[3][0][2] = hashlib.md5(data).digest()
    info[3][1][2] = (len(data),)
    listing = kbin_write(root, True, 0)
    listing += b"\0" * (-(0x24 + len(listing)) % 16)
    head = struct.pack(">IHHIII", 0x6CAD8F89, 3, 0xFFFC, when, 2 * letters + tree, 0x24 + len(listing))
    return head + hashlib.md5(listing).digest() + listing + bytes(data)
