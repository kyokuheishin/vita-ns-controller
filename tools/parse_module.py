#!/usr/bin/env python3
import argparse
import re
import struct
from pathlib import Path


def names_from_db(path):
    names = {}
    for yml in Path(path).glob("*.yml"):
        for line in yml.read_text(errors="replace").splitlines():
            match = re.match(r"\s+([A-Za-z_][A-Za-z0-9_]*): 0x([0-9A-Fa-f]{8})$", line)
            if match:
                names.setdefault(int(match.group(2), 16), match.group(1))
    return names


def parse(path, base, export_start, export_end, import_start, import_end, db):
    data = Path(path).read_bytes()

    def offset(address, size=1):
        result = address - base
        if result < 0 or result + size > len(data):
            raise ValueError(f"address outside dump: 0x{address:08X}")
        return result

    def u32(address):
        return struct.unpack_from("<I", data, offset(address, 4))[0]

    def string(address):
        start = offset(address)
        end = data.find(b"\0", start)
        if end < 0:
            raise ValueError(f"unterminated string: 0x{address:08X}")
        return data[start:end].decode("ascii", errors="replace")

    print("EXPORTS")
    cursor = export_start
    while cursor < export_end:
        fields = struct.unpack_from("<6H5I", data, offset(cursor, 0x20))
        size, _, _, functions, variables, tls, _, libnid, name, nids, entries = fields
        if size != 0x20:
            raise ValueError(f"bad export size 0x{size:X} at 0x{cursor:08X}")
        libname = string(name) if name else "<module>"
        print(f"library 0x{libnid:08X} {libname} functions={functions} variables={variables} tls={tls}")
        for index in range(functions):
            nid = u32(nids + index * 4)
            address = u32(entries + index * 4)
            print(f"  F 0x{address:08X} 0x{nid:08X} {db.get(nid, '?')}")
        cursor += size
    if cursor != export_end:
        raise ValueError("export table does not end on an entry boundary")

    print("IMPORTS")
    cursor = import_start
    while cursor < import_end:
        fields = struct.unpack_from("<6H6I", data, offset(cursor, 0x24))
        size, _, _, functions, variables, tls, libnid, name, fnids, stubs, vnids, vstubs = fields
        if size != 0x24:
            raise ValueError(f"bad import size 0x{size:X} at 0x{cursor:08X}")
        libname = string(name)
        print(f"library 0x{libnid:08X} {libname} functions={functions} variables={variables} tls={tls}")
        for index in range(functions):
            nid = u32(fnids + index * 4)
            address = u32(stubs + index * 4)
            print(f"  F 0x{address:08X} 0x{nid:08X} {db.get(nid, '?')}")
        for index in range(variables):
            nid = u32(vnids + index * 4)
            address = u32(vstubs + index * 4)
            print(f"  V 0x{address:08X} 0x{nid:08X} {db.get(nid, '?')}")
        cursor += size
    if cursor != import_end:
        raise ValueError("import table does not end on an entry boundary")


parser = argparse.ArgumentParser()
parser.add_argument("dump")
parser.add_argument("base", type=lambda value: int(value, 0))
parser.add_argument("export_start", type=lambda value: int(value, 0))
parser.add_argument("export_end", type=lambda value: int(value, 0))
parser.add_argument("import_start", type=lambda value: int(value, 0))
parser.add_argument("import_end", type=lambda value: int(value, 0))
parser.add_argument("--db", default="/usr/local/vitasdk/share/vita-headers/db/360")
args = parser.parse_args()
parse(args.dump, args.base, args.export_start, args.export_end,
      args.import_start, args.import_end, names_from_db(args.db))
