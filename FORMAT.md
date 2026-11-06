# RPK binary archive format — version 4

All integers are little-endian. Strings are UTF-8 without a terminating NUL. File and directory paths are relative, use `/` separators, and are validated against traversal and non-portable Windows components.

## Fixed header (56 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Magic bytes: `RPKG CR LF SUB LF` |
| 8 | 4 | Format version: `1`, `2`, `3`, or `4` |
| 12 | 4 | Header flags; must be zero |
| 16 | 8 | Manifest byte length |
| 24 | 32 | SHA-256 of the complete manifest |

The manifest begins at offset 56. The compressed payload begins immediately after the manifest.

## Manifest common prefix

| Field | Size |
|---|---:|
| File count | u32 |
| Directory count | u32 |
| Block count | u32 |
| LZMA preset | u32 |
| LZMA dictionary size in MiB | u32 |
| Maximum uncompressed solid-block size | u64 |

Version 3 and 4 then store a length-prefixed zlib runtime version string. Versions 1 and 2 do not contain this field.

## File records

Every file starts with a length-prefixed relative UTF-8 path and an original file size (u64).

Version 3 and 4 add a stored/intermediate file size (u64). In versions 1 and 2, stored size equals original size.

All versions then store:

| Field | Size |
|---|---:|
| File flags (bit 0 = read-only) | u8 |
| Reserved zero bytes | 7 |
| Original file SHA-256 | 32 |
| Extent count | u32 |
| Extents | `extent_count × 28` bytes |

Each extent contains a solid block index (u32), offset inside the decompressed block (u64), offset inside the stored/intermediate file (u64), and length (u64). Extents are ordered and cover the entire stored/intermediate file contiguously.

Version 3 and 4 append a precompression-record count (u32) to each file record, followed by these records:

| Field | Size |
|---|---:|
| Transformed-file offset of raw replacement bytes | u64 |
| Raw replacement length | u64 |
| Original encoded stream length | u64 |
| zlib compression level | u32 |
| SHA-256 of original encoded stream | 32 |

A record means that a valid zlib-wrapped stream in the original file was replaced by its decoded raw bytes at the transformed-file offset. To restore the original file, the extractor copies unchanged ranges and re-encodes each raw replacement with the recorded zlib level. It checks both the encoded stream size and SHA-256. The archive's zlib runtime version must match the extractor runtime; otherwise extraction is refused before installing restored files.

## Directories and blocks

Directory records follow the file records. Each directory record is a u32 path length followed by UTF-8 path bytes.

Every block starts with this common record:

| Field | Size |
|---|---:|
| Compressed payload offset (relative to payload start) | u64 |
| Compressed stream length | u64 |
| Uncompressed block length | u64 |

Version 1, 2, and 3 then store a 32-byte SHA-256 checksum. Those legacy blocks are always standalone XZ streams with LZMA2 data and CRC64.

Version 4 inserts the following fields between uncompressed length and checksum:

| Field | Size |
|---|---:|
| Codec identifier | u32 |
| Reserved zero bytes | u32 |

Codec identifiers:

| ID | Codec | Payload |
|---:|---|---|
| 1 | LZMA2/XZ | Standalone XZ stream with CRC64 check |
| 2 | Zstandard long-range | Standalone Zstandard frame with frame checksum; long-distance matching enabled |
| 3 | Store | Raw block bytes; compressed length must equal uncompressed length |

The 32-byte SHA-256 in every version-4 block record is calculated over the uncompressed block bytes, regardless of codec. The reader rejects unknown codec IDs and non-zero reserved fields. Payload blocks are contiguous in block-table order; offsets may not overlap or leave gaps.

## Version semantics

- **Version 1:** original manifest layout, without shared extents.
- **Version 2:** same manifest layout as version 1, but exact shared extents are permitted for content-defined deduplication.
- **Version 3:** adds the zlib runtime identifier, separate original/stored file sizes, and reversible zlib transform records. Blocks remain LZMA2/XZ.
- **Version 4:** retains version-3 file records and adds a codec ID to each block. Blocks may independently use LZMA2/XZ, Zstandard long-range, or Store.

The reader accepts versions 1 through 4. New archives are written as version 4, including archives that contain no precompression records.

## Integrity and safety

- The manifest is protected by SHA-256 in the fixed header.
- Every decompressed or stored block is protected by SHA-256 in the manifest. XZ adds CRC64; Zstandard adds its frame checksum.
- Every restored original file is protected by SHA-256 in its file record.
- Every reconstructed zlib stream is protected by the recorded encoded-stream SHA-256.
- The reader validates record counts, codec IDs, reserved fields, path safety, extent ranges, block coverage, payload bounds, stored-size coverage, transform offsets, and archive length before extraction.
