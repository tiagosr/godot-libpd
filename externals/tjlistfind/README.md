# tjlistfind

Second object of the **tj externals** series.

`tjlistfind <target> <tolerance>` searches a list of floats for the
first occurrence of a value and outputs its **1-indexed** position —
`0` when the value is absent.

## Usage

```
[tjlistfind 2.0]            exact match on 2.0
[tjlistfind 0.1 0.001]      |value - target| <= 0.001
```

| input (left inlet) | behavior |
|---|---|
| list of atoms `[1.5 2 3]` | first float atom within tolerance → its 1-indexed position; non-float atoms are skipped; no match / empty list → `0` |
| single float | one-element list: `1` if within tolerance, else `0` |
| `find f` | change the target (no output) |
| `tolerance f` | change the absolute tolerance, `f >= 0` (no output) |

Outlet (single): float — the 1-indexed position or `0`.

## Build

Standalone classic external (vanilla Pd 0.4x–0.6x / Plugdata):

    make PD=/path/to/pd-source

Copy the produced `tjlistfind<arch>` file into your externals folder.

Embedded (this repo): built by `extension/cmake/pdexternals.cmake` into
the libpd extension; registered automatically at process init.

## License

MIT (same as the enclosing repo).
