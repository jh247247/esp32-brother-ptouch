# Protocol and model provenance

This Apache-2.0 component independently expresses interoperability facts. It
does not copy or link GPL or proprietary driver source.

## Primary protocol sources

- Brother, *Raster Command Reference* for PT-E550W, PT-P750W, and PT-P710BT:
  command framing, status packets, raster modes, compression, feed, and cut.
- Brother PT-1950/1960 Windows driver 6.02.00.00, published 2010-03-02:
  independently observed default command order for PT-1950. Exact package and
  DLL SHA-256 values are retained in [NOTICE](../NOTICE).
- Brother product manuals/specifications and support articles: standard tape
  widths, P-Lite/Editor Lite identities, PT-1950 cut-guide behavior, and
  documented hardware limits.

## Cross-checks

- `treideme/brother_pt` (Apache-2.0) is the hardware-verified PT-P710BT oracle.
  `host/gen_oracle.py` records byte-exact fixtures generated from that reference.
- `ptouch-print` commit `f7cce686099df3034fc41e427ab14358277234fe`
  (GPL-3.0-or-later) was used only to cross-check USB identities and semantic
  model capabilities. No source, flag layout, comments, or data structures were
  copied.
- `packbits` 0.6 by Mikhail Korobov (MIT) informed the encoder state machine.
  The full required license text is in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

The public evidence tier for each USB identity is recorded in
[MODEL_SUPPORT.md](MODEL_SUPPORT.md) and enforced by host tests.
