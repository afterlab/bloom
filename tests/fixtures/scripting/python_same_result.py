"""One script, one result, on every platform (issue #677).

Run by the carried interpreter inside Bloom (`bloom-cli run`). It computes with the standard
library and the bundled package set and prints one digest per section and one over all of them.
The digests are compared with the values committed in python_same_result.txt, which are the same
on Linux, macOS and Windows.

Deliberately absent: what each system's math library computes, other than the square root, which
every system rounds correctly. That is the transcendental functions (sin, exp, log, pow with a
fractional exponent) and hypot, which is also the absolute value of a complex number, with their
numpy counterparts. They may differ in the last bit between platforms;
docs/architecture/platform-support.md says so.
"""

import bz2
from compression import zstd
import decimal
import fractions
import hashlib
import json
import lzma
import math
import os
import sqlite3
import struct
import sys
import zlib

import numpy


def digest(*parts: bytes) -> str:
    state = hashlib.sha256()
    for part in parts:
        state.update(len(part).to_bytes(8, "big"))
        state.update(part)
    return state.hexdigest()


def sequence(count: int, seed: int = 0x5EED) -> list[int]:
    """A fixed 64-bit linear congruential sequence, computed with Python integers."""
    values, state = [], seed
    for _ in range(count):
        state = (state * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        values.append(state)
    return values


def integers() -> str:
    big = math.factorial(300) // (3**200) + pow(7, 4321, 2**521 - 1)
    ratio = fractions.Fraction(355, 113) ** 40
    array = numpy.array(sequence(4096), dtype=numpy.uint64)
    mixed = (array * numpy.uint64(2654435761) + (array >> numpy.uint64(7))) ^ (array << numpy.uint64(3))
    signed = array.astype(numpy.int64) // numpy.int64(977)
    return digest(
        str(big).encode(),
        str(ratio.numerator).encode(),
        str(ratio.denominator).encode(),
        mixed.tobytes(),
        signed.tobytes(),
        numpy.cumsum(array[:512] % numpy.uint64(1000003)).tobytes(),
    )


def floats() -> str:
    raw = numpy.array(sequence(8192, 0xF10A7), dtype=numpy.uint64)
    unit = (raw >> numpy.uint64(11)).astype(numpy.float64) * (1.0 / 9007199254740992.0)
    scaled = unit * 1024.0 - 512.0
    single = scaled.astype(numpy.float32)
    results = [
        scaled + unit,
        scaled - unit * 3.0,
        scaled * unit,
        scaled / (unit + 0.5),
        numpy.sqrt(unit),
        numpy.abs(scaled),
        numpy.floor(scaled),
        numpy.rint(scaled),
        scaled % 7.0,
        single * numpy.float32(1.5) + numpy.float32(0.25),
        numpy.sqrt(numpy.abs(single)),
    ]
    # Complex arrays: the product is where a processor-specific kernel would fuse a
    # multiplication and an addition. (The absolute value is the math library's hypot.)
    pairs = scaled[:4096] + 1j * scaled[4096:]
    narrow = pairs.astype(numpy.complex64)
    results += [pairs * pairs[::-1], narrow * narrow[::-1], pairs / pairs[::-1]]
    scalar = 0.1
    for value in sequence(256, 0xABC):
        scalar = (scalar * 1.0000001 + (value % 1000) / 7.0) / 1.5
    text = json.dumps([0.1 + 0.2, 1e22, 5e-324, math.sqrt(2.0), float(scaled[17])])
    return digest(
        *[result.tobytes() for result in results],
        struct.pack("<d", scalar),
        struct.pack("<d", math.sqrt(2.0) * math.fsum(float(x) for x in unit[:100])),
        struct.pack("<dd", ((1.5 + 2.5j) * (0.1 - 3.0j)).real, ((1.5 + 2.5j) / (0.1 - 3.0j)).imag),
        text.encode(),
    )


def sorting() -> str:
    raw = numpy.array(sequence(20000, 0x50A7), dtype=numpy.uint64)
    unit = (raw >> numpy.uint64(11)).astype(numpy.float64) * (1.0 / 9007199254740992.0)
    small = (raw % numpy.uint64(257)).astype(numpy.int32)
    generator = numpy.random.default_rng(20261112)
    drawn = generator.integers(0, 1 << 40, size=4096)
    uniform = generator.random(4096)
    words = sorted(f"{value:016x}"[::-1] for value in sequence(2000, 0x51))
    return digest(
        numpy.sort(raw).tobytes(),
        numpy.sort(unit).tobytes(),
        numpy.argsort(small, kind="stable").astype(numpy.int64).tobytes(),
        numpy.unique(small).tobytes(),
        numpy.partition(unit, 1000)[:1000].min().tobytes(),
        numpy.searchsorted(numpy.sort(unit), unit[:64]).astype(numpy.int64).tobytes(),
        drawn.astype(numpy.int64).tobytes(),
        uniform.tobytes(),
        "\n".join(words).encode(),
    )


def reductions() -> str:
    raw = numpy.array(sequence(65536, 0x2ED), dtype=numpy.uint64)
    unit = (raw >> numpy.uint64(11)).astype(numpy.float64) * (1.0 / 9007199254740992.0)
    grid = unit.reshape(256, 256)
    single = grid.astype(numpy.float32)
    return digest(
        numpy.float64(unit.sum()).tobytes(),
        numpy.float64(unit.mean()).tobytes(),
        numpy.float64(unit.var()).tobytes(),
        grid.sum(axis=0).tobytes(),
        grid.sum(axis=1).tobytes(),
        numpy.cumsum(unit[:4096]).tobytes(),
        grid.prod(axis=1).tobytes(),
        grid.min(axis=0).tobytes(),
        grid.max(axis=1).tobytes(),
        numpy.argmax(grid, axis=1).astype(numpy.int64).tobytes(),
        numpy.float32(single.sum()).tobytes(),
        single.mean(axis=0).tobytes(),
        numpy.median(grid, axis=1).tobytes(),
        numpy.add.reduce(raw % numpy.uint64(1000)).tobytes(),
    )


def linear_algebra() -> str:
    raw = numpy.array(sequence(64 * 64 * 2, 0x11A), dtype=numpy.uint64)
    unit = (raw >> numpy.uint64(11)).astype(numpy.float64) * (1.0 / 9007199254740992.0)
    a = unit[: 64 * 64].reshape(64, 64) + numpy.eye(64) * 8.0
    b = unit[64 * 64 :].reshape(64, 64)
    vector = b[:, 0].copy()
    product = a @ b
    return digest(
        product.tobytes(),
        (a.astype(numpy.float32) @ b.astype(numpy.float32)).tobytes(),
        numpy.dot(vector, vector).tobytes(),
        numpy.outer(vector[:8], vector[:8]).tobytes(),
        numpy.linalg.solve(a, vector).tobytes(),
        numpy.linalg.inv(a).tobytes(),
        numpy.float64(numpy.linalg.det(a[:12, :12])).tobytes(),
        numpy.linalg.cholesky(a @ a.T).tobytes(),
        numpy.linalg.qr(a[:16, :16])[1].tobytes(),
        numpy.einsum("ij,jk->ik", a[:8, :8], b[:8, :8]).tobytes(),
        numpy.tensordot(a[:6, :6], b[:6, :6], axes=1).tobytes(),
    )


def hashing() -> str:
    data = b"".join(value.to_bytes(8, "little") for value in sequence(4096, 0x4A54))
    names = ["md5", "sha1", "sha256", "sha512", "sha3_256", "sha3_512", "blake2b", "blake2s"]
    parts = [hashlib.new(name, data).digest() for name in names]
    parts.append(hashlib.shake_128(data).digest(40))
    parts.append(hashlib.pbkdf2_hmac("sha256", b"bloom", data[:16], 1000))
    parts.append(hashlib.scrypt(b"bloom", salt=data[:16], n=1024, r=8, p=1, dklen=32))
    parts.append(zlib.crc32(data).to_bytes(4, "big") + zlib.adler32(data).to_bytes(4, "big"))
    return digest(*parts)


def compressed() -> str:
    words = [f"{value % 5000:04d}-layer-{value % 17}" for value in sequence(6000, 0xC0DE)]
    text = " ".join(words).encode()
    noise = b"".join(value.to_bytes(8, "little") for value in sequence(2048, 0x9015E))
    parts = []
    for data in (text, noise, b"", bytes(100000)):
        packed = [
            zlib.compress(data, 6),
            zlib.compress(data, 9),
            bz2.compress(data, 9),
            lzma.compress(data, preset=6),
            lzma.compress(data, format=lzma.FORMAT_ALONE, preset=1),
            zstd.compress(data, level=3),
            zstd.compress(data, level=19),
        ]
        assert zlib.decompress(packed[0]) == data and bz2.decompress(packed[2]) == data
        assert lzma.decompress(packed[3]) == data and zstd.decompress(packed[5]) == data
        parts.extend(packed)
    return digest(*parts)


def database() -> str:
    connection = sqlite3.connect(":memory:")
    connection.execute("create table layer(id integer primary key, name text, opacity real, tag blob)")
    rows = [
        (index, f"layer-{value % 97:02d}", (value % 1000) / 8.0, value.to_bytes(8, "big"))
        for index, value in enumerate(sequence(2000, 0xDB))
    ]
    connection.executemany("insert into layer values(?, ?, ?, ?)", rows)
    connection.execute("create index layer_name on layer(name, opacity)")
    connection.execute("create virtual table note using fts5(body)")
    notes = [(f"shot {value % 13} take {value % 7} bloom",) for value in sequence(300, 0x407E)]
    connection.executemany("insert into note values(?)", notes)
    queries = [
        "select name, count(*), sum(opacity), avg(opacity), min(id), max(id) from layer group by name order by name",
        "select id, hex(tag) from layer where opacity between 10 and 20 order by opacity desc, id limit 50",
        "select name, group_concat(id, ',') from (select * from layer order by id) where id % 211 = 0 group by name order by name",
        "select rowid, body from note where note match 'take AND bloom' order by rowid limit 40",
        "select json_group_array(id) from layer where id < 20",
        "select sqrt(opacity), abs(opacity - 60), round(opacity / 3, 4) from layer order by id limit 30",
        "select typeof(opacity), cast(opacity as integer), printf('%.3f|%08d', opacity, id) from layer order by id desc limit 30",
    ]
    parts = [repr(connection.execute(query).fetchall()).encode() for query in queries]
    return digest(*parts)


def decimals() -> str:
    context = decimal.Context(prec=120, rounding=decimal.ROUND_HALF_EVEN)
    third = context.divide(decimal.Decimal(1), decimal.Decimal(3))
    root = context.sqrt(decimal.Decimal(2))
    power = context.power(decimal.Decimal("1.000000001"), decimal.Decimal(100000))
    total = decimal.Decimal(0)
    for value in sequence(500, 0xDEC):
        total = context.add(total, context.divide(decimal.Decimal(value % 100003), decimal.Decimal(977)))
    return digest(str(third).encode(), str(root).encode(), str(power).encode(), str(total).encode(),
                  str(context.quantize(total, decimal.Decimal("0.0001"))).encode())


SECTIONS = {
    "integers": integers,
    "floats": floats,
    "sorting": sorting,
    "reductions": reductions,
    "linear-algebra": linear_algebra,
    "hashing": hashing,
    "compression": compressed,
    "database": database,
    "decimal": decimals,
}


def main() -> None:
    results = {name: function() for name, function in SECTIONS.items()}
    results["all"] = digest(*[f"{name}={value}".encode() for name, value in results.items()])
    for name, value in results.items():
        print(f"{name:15s} {value}")
    print(
        f"python {sys.version.split()[0]} numpy {numpy.__version__} sqlite {sqlite3.sqlite_version} "
        f"zlib {zlib.ZLIB_RUNTIME_VERSION} {sys.platform}"
    )
    # The test names the committed result in the environment; without it the script only prints.
    expected_path = os.environ.get("BLOOM_PYTHON_SAME_RESULT")
    if not expected_path:
        return
    with open(expected_path, encoding="utf-8") as stream:
        expected = dict(line.split() for line in stream if line.strip() and not line.startswith("#"))
    wrong = sorted(name for name in set(expected) | set(results) if expected.get(name) != results.get(name))
    assert not wrong, "different from the committed result: " + ", ".join(wrong)
    print("same result as committed")


main()
