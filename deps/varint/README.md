varint: variable length integer storage
=======================================

Background
----------
Numbers in computers have limited native sizes: one byte, two bytes, four bytes, and eight bytes. The lack of three, five, six, and seven byte quantities can waste a lot of storage space if most of your data is small but you still need the ability to grow to large quantities on-demand. That's where varints come in.

Varints let you store and retrieve variable length integers in units of single byte widths instead of the standard fixed widths of 8, 16, 32, and 64 bits.

Each implementation of a varint has different ways of organizing the "variable" aspect determining the total length of any byte-reduced data representations.

The goal of varints is instead of limiting you to only 8, 16, 32, and 64-bit quantities, you can save/restore any of 8, 16, 24, 32, 40, 48, 56, 64 bit quantities.

Varints don't add any magic to your programming language or environment. Everything is accessed through a varint encode/decode API.

This package also includes packed integer encodings allowing you to store any bit-width integer across bytes if you need to optimize storage for, example, 8,000 integers but only with 14-bits each in a byte array.

The package also allows you to maintain sorted packed integer encodings if you want to maintain a large binary-search capable packed bit array to represent integer sets for easy [membership testing](https://github.com/mattsta/varint/blob/main/src/varintPacked.h#L412-L459) and [fast deletions](https://github.com/mattsta/varint/blob/main/src/varintPacked.h#L516-L531).

This package also includes packed matrix encodings so you can easily operate bit-packed matrix entries of arbitrary size.


Example Usage
-------------

For more complete usage, see the code tests for varints in [`src/varintCompare.c`](https://github.com/mattsta/varint/blob/main/src/varintCompare.c) and code tests for things like packed bit matrices in [`src/varintDimension.c:varintDimensionTest()`](https://github.com/mattsta/varint/blob/main/src/varintDimension.c#L355-L544) and code tests for multi-word packed integer bit level encodings also in  [`varintDimension.c`](https://github.com/mattsta/varint/blob/main/src/varintDimension.c#L252-L317).


### Tagged

```c
uint64_t x = 555557;
uint8_t z[9];

varintWidth length = varintTaggedPut64(z, x);
/* 'length' is 4 because the tagged varint used 4 bytes. */
uint64_t y;
varintTaggedGet64(z, &y);
assert(x == y);
```

### External

```c
uint64_t x = 555557;
uint8_t z[8];

varintWidth encoding = varintExternalPut(z, x);
/* 'encoding' is 3 because 'x' fits in 3 bytes */
uint64_t y = varintExternalGet(z, encoding);
assert(x == y);
```

For storage, all varints are evaluated as unsigned byte quantities. Any conversion to/from signed values are the responsibility of the caller.

Storage Overview
----------------

| varint             | length stored in  | 1 byte max |  2 byte max |   3 byte max |    4 byte max |
|--------------------|-------------------|-----------:|------------:|-------------:|--------------:|
| Tagged             | first byte        |        240 |       2,287 |       67,823 |    16,777,215 |
| Split              | first byte        |         63 |      16,701 |       81,982 |    16,793,661 |
| Split Full         | first byte        |         63 |      16,446 |    4,276,284 |    20,987,964 |
| Split Full No Zero | first byte        |         64 |      16,447 |    4,276,285 |    20,987,965 |
| Split Full 16      | first byte        |          X |      16,383 |    4,210,686 | 1,077,952,509 |
| Chained            | final flag bit    |        127 |      16,383 |    2,097,151 |   268,435,455 |
| External           | first byte        |          X |         255 |       65,535 |    16,777,215 |
| External           | external metadata |        255 |      65,535 |   16,777,215 | 4,294,967,295 |

Note: Split and Split Full use two level split encodings where certain byte maximums
have two different encodings, so we count the maximum encoding at these boundary byte
positions for the highest numbers capable of being stored:

| varint             | level     | 1 byte max |  2 byte max |   3 byte max |    4 byte max |
|--------------------|-----------|-----------:|------------:|-------------:|--------------:|
| Split              | first     |         63 |      16,446 |            X |             X |
| Split              | second    |          X |      16,701 |       81,982 |    16,793,661 |
| Split Full         | first     |         63 |      16,446 |    4,210,749 |             X |
| Split Full         | second    |          X |           X |    4,276,284 |    20,987,964 |
| Split Full No Zero | first     |         64 |      16,447 |    4,210,750 |             X |
| Split Full No Zero | second    |          X |           X |    4,276,285 |    20,987,965 |

Code Guide
----------
Varints are defined by how they track their size. Since varints have variable lengths, a varint must know how many bytes it contains.

We have four types of varints: tagged, external, split, and chained. The chained type is the slowest and is not recommended for use in new systems.

The goal of a varint isn't to store the *most* data in the least space (which is impossible since the value here *includes* metadata information which takes away from user storage space), but to allow you to let users store data without needing to pre-allocate everything as a 64-bit quantity up front.

### Tagged
Tagged varints hold their full width metadata in the first byte. The first byte also contributes to the stored value and can, by itself, also hold a user value up to 240. The maximum length of a 64-bit tagged varint is 9 bytes.

This is a varint format adapted from the abandoned sqlite4 project. Full encoding details are in [source comments](https://github.com/mattsta/varint/blob/main/src/varintTagged.c).

### Split
Split varints hold their full width metadata in the first byte. The first byte can hold a user value up to 63. The maximum length of a 64-bit split varint is 9 bytes.

This varint uses split "levels" for storing integers. The first level is one byte prefixed with bits `00` then stores 6 bits of user data (up to number 63). The second level is two bytes, with the first byte prefixed with bits `01` plus the following full byte, and stores 14 bits of user data (16383 + the previous level of 63 = 16446). After the second level, we reserve the first byte only for type data and use an external varint for the following bytes. The third level is between 2 and 9 bytes wide. The third level can store up to value 16701 in two bytes, and up to 81981 in three bytes, all the way up to a full 9 bytes to store a 64 bit value (8 bytes value + 1 byte type metadata).

### External
External varints keep their full width metadata external to the varint encoding itself. You have to track the length of your varint either by manually prefixing your varint with a byte describing the length of the following varint or by implicitly knowing the type by other means. The maximum length of a 64-bit external varint is 8 bytes since your are maintaining the type data external to the varint encoded data.

External varints are equivalent to just splitting a regular 64-bit quantity into single byte slices.

External varints just save the bytes containing data with no internal overhead. So, if you store `3` inside of a uint64_t, it won't use 8 bytes, it will use one byte. Since the external encoding doesn't pollute the bytes with encoding metadata, you can even cast system-width external varints (8, 16, 32, 64) to native types without further decoding (little-endian only).

External varints do store the most data in the least space possible, but you must maintain the type/length of the stored varint external to the varint itself for later retrieval.

### Chained
Chained varints don't know the full type/length of their data until they traverse the entire varint and reach an "end of varint" bit, so they are the slowest variety of varint. Each byte of a chained varint has a "continuation" bit signaling if the end of the varint has been reached yet. The maximum length of a 64-bit chained varint is 9 bytes.

This is the most common legacy varint format and is used in sqlite3, leveldb, and many other places. Full encoding details are in source comments for the [sqlite3 derived version](https://github.com/mattsta/varint/blob/main/src/varintChained.c) and for the [leveldb derived (and herein optimized further) version](https://github.com/mattsta/varint/blob/main/src/varintChainedSimple.c).

### Packed Bit Arrays

Also includes support for arrays of fixed-bit-length packed integers in `varintPacked.c` as well as reading and writing
packed bit arrays into matrices in `varintDimension.c`.

Building
--------

    mkdir build
    cmake ..
    make -j12

Testing
-------
For testing and performance comparisons, run:

- `./build/src/varint-compare`
- `./build/src/varintDimensionTest`
- `./build/src/varintPackedTest 3000`


License
-------
Many of the functions and routines here are just bit manipulations which aren't uniquely license-able. Otherwise, `varintTagged` is adapted from sqlite4 and `varintChained` is adapted from sqlite3 and `varintChainedSimple` is adapted from leveldb and `varintSplit` metadata layouts were inspired by legacy redis ziplist bit-type management circa 2015 (but code here is a new non-derived implementation and is uniquely expanded over the original ideas). All other implementation details are made available under Apache-2.0 for features such as common bit manipulations and novel implementations of routine data manipulation patterns.
