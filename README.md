# datakit: the most efficient data structures possible

Also see:

- https://matt.sh/datakit
- https://matt.sh/best-database-ever
- https://matt.sh/trillion-dollar-data-structure

## Usage

```haskell
mkdir build
cd build
cmake ..
make -j8

./src/datakit-test test multimap
./src/datakit-test test multimapFull
./src/datakit-test test multilist
./src/datakit-test test multilistFull
./src/datakit-test test multiarray
./src/datakit-test test multiarraySmall
./src/datakit-test test multiarrayMedium
./src/datakit-test test multiarrayLarge
./src/datakit-test test xof
./src/datakit-test test dod
./src/datakit-test test intset
./src/datakit-test test intsetU32
./src/datakit-test test hll
./src/datakit-test test strDoubleFormat
./src/datakit-test test multiroar
./src/datakit-test test fibbuf
./src/datakit-test test jebuf
./src/datakit-test test mflex
./src/datakit-test test flex
./src/datakit-test test membound
./src/datakit-test test multimapatom
./src/datakit-test test multilru
./src/datakit-test test ptrprevnext
```

## Status

This is a recently updated development snapshot.

Many components are complete.

Documentation is in code comments and in tests at the end of files. Full documentation would take a couple hundred more hours of work mapping out and explaining everything.

Some components are a first or second draft and need another feature refactoring cycle or two.

Some components may be abandoned experiments and I just forgot to remove them from this snapshot.


Good luck.
