#!/usr/bin/env python3
"""vppdis.py -- read Volition VPP v4 packfiles from the PS3 build of Saints Row 2.

    python vppdis.py list    <pack.vpp_ps3>
    python vppdis.py extract <pack.vpp_ps3> <out-dir> [name-substring]

WHY THIS EXISTS. Every PC-side Volition tool (Gibbed's, ThomasJepp's) reads the header
LITTLE-endian and refuses these files at the magic check. The PS3 build stores the whole
container big-endian, and its payload codec is not the zlib stream the PC packs use. Both
differences were measured on
    E:\\PS3 Games\\Saints Row 2 (USA) (En,Fr)\\PS3_GAME\\USRDIR\\packfiles\\ps3
on 2026-09-12 and are written down below so nobody has to re-derive them.

THE FORMAT, all fields big-endian:

    0x000  u32 magic              0x51890ACE   (PC tools look for 0xCE0A8951)
    0x004  u32 version            4
    0x14C  u32 flags              0x01 = compressed, 0x02 / 0x20 = seen but not decoded
    0x154  u32 file_count
    0x158  u32 package_size       equals the file's size on disk, checked
    0x15C  u32 directory_size     == file_count * 28
    0x160  u32 filename_size
    0x164  u32 extension_size
    0x168  u32 uncompressed_size
    0x16C  u32 compressed_size    0xFFFFFFFF when the pack stores everything raw

    directory  at 0x800, entries of 28 bytes:
        u32 name_offset          into the filename block
        u32 ext_offset           into the extension block
        u32 unknown              always 0 in all 30 retail packs
        u32 data_offset          into UNCOMPRESSED space, not the file
        u32 uncompressed_size
        u32 compressed_size      0xFFFFFFFF = this file is stored raw
        u32 flags                always 0 in all 30 retail packs

    filenames  at align(0x800 + directory_size, 0x1000), NUL-separated
    extensions immediately after the filename block, NUL-separated
    data       at align(filenames + filename_size + extension_size, 0x1000)

THE PAYLOAD. A compressed file is a sequence of chunks, each

    [u16 chunk_compressed_size][u16 pad][u32 chunk_uncompressed_size][raw DEFLATE payload]

and entry.compressed_size counts the 8-byte header(s) as well as the payload. The codec is
**raw deflate** -- zlib.decompressobj(-15). There is no zlib wrapper anywhere in these files,
which is why scanning for a 0x78 header finds nothing and why every off-the-shelf reader gives
up. chunk_compressed_size is a u16 because the uncompressed chunk is capped at 64 KiB; a file
larger than that is simply several chunks back to back.

Files are laid out in directory order at a running physical offset, each one **aligned up to
0x800 bytes**. That alignment was solved rather than guessed, and the obvious cheap method is
wrong: checking only entry 1 says 0x100, because entry 1 happens to land on 0x6800, which is
0x800-aligned anyway. Locating the first EIGHT entries by searching for a chunk header whose
declared uncompressed size matches the directory gives deltas 0x1800 x5 then 0x2000 x2, and
align(packed, 0x800) reproduces every one of them.

VERIFIED on shaders.vpp_ps3 (318 files): every file's chunks sum to exactly the directory's
declared uncompressed size.
"""

import os
import struct
import sys
import zlib

MAGIC = 0x51890ACE
ENTRY_SIZE = 28
RAW = 0xFFFFFFFF
FILE_ALIGN = 0x800


def _align(value, boundary):
    return (value + boundary - 1) & ~(boundary - 1)


def _cstr(block, offset):
    end = block.find(b"\0", offset)
    return block[offset:end if end >= 0 else len(block)].decode("latin-1")


class Package:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as handle:
            self.data = handle.read()

        magic, version = struct.unpack_from(">II", self.data, 0)

        if magic != MAGIC:
            raise ValueError(
                "%s: magic 0x%08X is not a big-endian VPP (PC packs are 0xCE0A8951)"
                % (os.path.basename(path), magic))

        if version != 4:
            raise ValueError("%s: version %d, only 4 is decoded" % (os.path.basename(path), version))

        u32 = lambda off: struct.unpack_from(">I", self.data, off)[0]

        self.flags = u32(0x14C)
        self.count = u32(0x154)
        self.package_size = u32(0x158)
        self.directory_size = u32(0x15C)
        self.filename_size = u32(0x160)
        self.extension_size = u32(0x164)
        self.uncompressed_size = u32(0x168)
        self.compressed_size = u32(0x16C)

        # Cheap integrity checks. Each one has caught a wrong offset at some point while this
        # was being written, and they cost nothing next to reading the file.
        if self.directory_size != self.count * ENTRY_SIZE:
            raise ValueError("directory_size %d != count %d * %d"
                             % (self.directory_size, self.count, ENTRY_SIZE))

        if self.package_size != len(self.data):
            raise ValueError("package_size %d != %d bytes on disk"
                             % (self.package_size, len(self.data)))

        names_at = _align(0x800 + self.directory_size, 0x1000)
        exts_at = names_at + self.filename_size
        self.data_at = _align(exts_at + self.extension_size, 0x1000)

        name_block = self.data[names_at:names_at + self.filename_size]
        ext_block = self.data[exts_at:exts_at + self.extension_size]

        self.entries = []
        physical = self.data_at

        for i in range(self.count):
            fields = struct.unpack_from(">7I", self.data, 0x800 + (i * ENTRY_SIZE))
            name = _cstr(name_block, fields[0])
            ext = _cstr(ext_block, fields[1])
            entry = {
                "name": "%s.%s" % (name, ext) if ext else name,
                "data_offset": fields[3],
                "size": fields[4],
                "packed": fields[5],
                "flags": fields[6],
                "physical": physical,
            }
            self.entries.append(entry)

            # A raw file occupies its uncompressed size; a compressed one the packed size that
            # already includes its chunk headers. Either way the next file starts 256-aligned.
            span = entry["size"] if entry["packed"] == RAW else entry["packed"]
            physical = _align(physical + span, FILE_ALIGN)

    def read(self, entry):
        if entry["packed"] == RAW:
            start = self.data_at + entry["data_offset"]
            return self.data[start:start + entry["size"]]

        out = bytearray()
        pos = entry["physical"]
        end = pos + entry["packed"]

        while pos < end and len(out) < entry["size"]:
            packed, _pad, plain = struct.unpack_from(">HHI", self.data, pos)
            pos += 8
            out += zlib.decompressobj(-15).decompress(self.data[pos:pos + packed])
            pos += packed

        if len(out) != entry["size"]:
            raise ValueError("%s: inflated %d bytes, directory says %d"
                             % (entry["name"], len(out), entry["size"]))

        return bytes(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    mode, path = argv[1], argv[2]
    pack = Package(path)

    if mode == "list":
        print("%s: %d files, flags 0x%04X, %d -> %d bytes, data at 0x%X"
              % (os.path.basename(path), pack.count, pack.flags,
                 pack.uncompressed_size, pack.compressed_size, pack.data_at))
        for entry in pack.entries:
            print("  %-52s %9d %9s" % (entry["name"], entry["size"],
                                       "raw" if entry["packed"] == RAW else entry["packed"]))
        return 0

    if mode == "extract":
        if len(argv) < 4:
            print(__doc__)
            return 2

        out_dir = argv[3]
        needle = argv[4].lower() if len(argv) > 4 else None
        os.makedirs(out_dir, exist_ok=True)
        written = 0

        for entry in pack.entries:
            if needle and needle not in entry["name"].lower():
                continue

            blob = pack.read(entry)
            # Names are flat inside a pack, but a stray separator would escape out_dir.
            safe = entry["name"].replace("\\", "_").replace("/", "_")
            with open(os.path.join(out_dir, safe), "wb") as handle:
                handle.write(blob)
            written += 1

        print("extracted %d file(s) to %s" % (written, out_dir))
        return 0

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
