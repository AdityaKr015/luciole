"""Decode a 7z header from held fd 3; never extract or write archive members."""

import io
import os
import sys
import zlib


def main():
    try:
        import py7zr
        from py7zr.archiveinfo import Header, SignatureHeader
        from py7zr.exceptions import PasswordRequired, UnsupportedCompressionMethodError
    except ImportError:
        return 69

    version = tuple(int(part) for part in py7zr.__version__.split(".")[:2])
    if not (1, 1) <= version < (2, 0):
        return 65
    maximum = int(sys.argv[1])
    password = sys.stdin.buffer.readline(1026)
    if not password.endswith(b"\n") or len(password) > 1025:
        return 65
    password = password[:-1].decode("utf-8", errors="strict") or None

    class RawHeader(Header):
        # Header._read verifies encoded-stream CRCs before this parsing hook.
        # Retain the original decoded bytes, not a rewritten/sanitized catalog.
        def _extract_header_info(self, buffer):
            self.plain = buffer.getvalue()
            if len(self.plain) > maximum:
                raise MemoryError

    class BoundedSource:
        def __init__(self, source):
            self.source = source
            self.size = os.fstat(3).st_size
            self.budget = self.size * 4

        def read(self, amount=-1):
            if amount < 0 or amount > maximum or amount > self.budget:
                raise MemoryError
            data = self.source.read(amount)
            self.budget -= len(data)
            return data

        def seek(self, offset, origin=0):
            position = offset if origin == 0 else (self.tell() if origin == 1 else self.size) + offset
            if not 0 <= position <= self.size:
                raise ValueError
            return self.source.seek(position)

        def tell(self):
            return self.source.tell()

    try:
        with open(3, "rb", closefd=False) as descriptor:
            source = BoundedSource(descriptor)
            if source.read(6) != b"7z\xbc\xaf\x27\x1c":
                return 65
            signature = SignatureHeader.retrieve(source)
            source.seek(0, 2)
            size = source.tell()
            offset = signature.nextheaderofs + 32
            if (signature.nextheadersize > maximum or offset > size
                    or signature.nextheadersize > size - offset):
                return 65
            source.seek(offset)
            encoded = source.read(signature.nextheadersize)
            if zlib.crc32(encoded) != signature.nextheadercrc:
                return 2
            header = RawHeader.retrieve(source, io.BytesIO(encoded), 32, password)
            if not hasattr(header, "plain") or not header.plain.startswith(b"\x01"):
                return 65
            sys.stdout.buffer.write(header.plain)
            return 0
    except PasswordRequired:
        return 3
    except (MemoryError, UnsupportedCompressionMethodError, AttributeError):
        return 65
    except OSError:
        return 74
    except Exception:
        # Wrong passwords and damaged encrypted headers are not distinguishable.
        return 2


if __name__ == "__main__":
    sys.exit(main())
