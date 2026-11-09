# Clipper2 1.5.4 License And Feature Review

Reviewed: 2026-11-09

## License

Clipper2 is released under the Boost Software License 1.0 (Copyright Angus Johnson 2010-2025). The
license is compatible with Bloom's Apache-2.0 distribution; the complete, unmodified text is retained
beside the sources and in this record (`LICENSE`), and the repository notices list the component.

## Build Boundary

The sources are unmodified. The three library sources are compiled as C++ by one object-library
target (`bloom_runtime_clipper2`) with third-party warnings disabled and the library's 64-bit integer
coordinate build (the `Z` coordinate extension is not enabled). Bloom's public headers contain no
Clipper2 types: the path operations in `bloom_runtime` convert between Bloom's path value and the
library's polygons inside one private translation unit. No superbuild recipe, `FetchContent`, or lock
entry is used.

## Feature Use

Bloom uses polygon union, difference, intersection and exclusive-or, and polygon offsetting with
miter, round and bevel joins, over polygons flattened from a path value at a stated tolerance. The
rectangle clipping and Minkowski operations are not used.
