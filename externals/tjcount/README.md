# tjcount — up-only ranged counter with a wrap bang

`tjcount` is a control-rate Pd object: a single up-counting, ranged
counter driven by bangs (e.g. a `[metro]`), part of the **tj
externals** series (ganged, reloadable, ranged counters).

## Usage

```
[tjcount min max]
```

`min`/`max` are optional creation args (floats, default `0 1`).

- **inlet 0** — `bang` counts up by 1; `float` clamps the count into
  `[min, max]` and outputs it; `set f` clamps without output;
  `min f` / `max f` change the range (guarded so `min < max` stays
  true; the current count is clamped into the new range, no output).
- **inlet 1** — float sets `min`.
- **inlet 2** — float sets `max`.
- **outlet left** — float count.
- **outlet right** — bang **on wrap only**: when a bang would push the
  count past `max`, the count returns to `min` and the right outlet
  bangs *before* the new count is output (standard pd right-outlet-bang
  convention).

The wrap bang is what you use to advance the next stage of a ganged
counter chain: it fires exactly on `max -> min`, unlike cyclone/else
counters whose carry fires when a boundary is reached (one count early
for that use).

## Build

Classic external makefile (vanilla Pd and Plugdata):

```sh
make PD=/path/to/pd-source   # e.g. ~/pd/pd or a Plugdata checkout
# result: tjcount~arch (tjcount_macosx / tjcount_linux / ...)
```

In the godot-libpd project the same source is compiled into the
embedded libpd build (`extension/cmake/pdexternals.cmake`) and
registered at process init — `[tjcount]` is available in every
libpd-loaded patch there with no extra setup.

## License

MIT (same terms as the godot-libpd repository; see LICENSE.txt).
