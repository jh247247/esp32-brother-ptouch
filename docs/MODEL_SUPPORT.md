# Brother P-touch model profiles

Version `0.2` replaces the PT-P710BT-only path with an explicit, fail-closed
profile catalog. A recognized printer is claimed only when its USB identity is
enabled and one alternate-setting-zero printer interface contains a valid
same-interface bulk-IN/bulk-OUT pair. Unknown models, Editor Lite/P-Lite
identities, malformed descriptors, and known incompatible raster protocols are
detected but never receive output.

## Printable catalog

`3.5` is represented by Brother's protocol width value `4`. Generic 21 mm is
intentionally absent: it is an FLe flag-label or HSe heat-shrink size on the
models that advertise it, not a standard TZe/TZ tape width. Only laminated
(`0x01`) and non-laminated (`0x03`) standard tape are admitted; fabric,
flexible-ID, satin, flag-label, heat-shrink, and incompatible media fail
preflight until they have their own validated recipes.

| USB PID | Model profile | Head | Standard tape admitted (mm) | Raster recipe | Current hardware authority |
|---|---|---:|---|---|---|
| `2001` | PT-9200DX | 384 at 360 dpi | 6, 9, 12, 18, 24, 36 | legacy + PackBits | experimental recipe; explicit opt-in |
| `2002` | PT-9200DX (alternate identity) | 384 at 360 dpi | 6, 9, 12, 18, 24, 36 | legacy + PackBits | experimental recipe; explicit opt-in |
| `2004` | PT-2300 | 112 at 180 dpi | 6, 9, 12, 18, 24 | legacy + PackBits | experimental recipe; explicit opt-in |
| `2007` | PT-2420PC | 128 at 180 dpi | 6, 9, 12, 18, 24 | legacy + PackBits | experimental recipe; explicit opt-in |
| `2011` | PT-2450PC / PT-2450DX | 128 at 180 dpi | 6, 9, 12, 18, 24 | legacy + PackBits | experimental recipe; explicit opt-in |
| `2019` | PT-1950 | 112 at 180 dpi | 6, 9, 12, 18 | Brother-default implicit + PackBits, framed | inspected hardware output and cut; completion inspection-gated; chaining unavailable |
| `201F` | PT-2700 | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | legacy raw | experimental recipe; explicit opt-in |
| `202C` | PT-1230PC | 128 at 180 dpi | 3.5, 6, 9, 12 | legacy raw | experimental recipe; explicit opt-in |
| `202D` | PT-2430PC | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | legacy raw | experimental recipe; explicit opt-in |
| `2041` | PT-2730 | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | legacy raw | experimental recipe; explicit opt-in |
| `205E` | PT-H500 | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | legacy + PackBits | experimental recipe; explicit opt-in |
| `205F` | PT-E500 | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | legacy + PackBits | experimental recipe; explicit opt-in |
| `2061` | PT-P700 | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | dynamic + PackBits | experimental recipe; explicit opt-in |
| `2062` | PT-P750W | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | dynamic + PackBits | experimental recipe; explicit opt-in |
| `2073` | PT-D450 | 128 at 180 dpi | 3.5, 6, 9, 12, 18 | legacy raw + print info | experimental recipe; explicit opt-in |
| `2074` | PT-D600 | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | legacy + PackBits | experimental recipe; explicit opt-in |
| `2085` | PT-P900Wc | 560 at 360 dpi | 3.5, 6, 9, 12, 18, 24, 36 | dynamic + PackBits + print info | experimental recipe; explicit opt-in |
| `20AF` | PT-P710BT | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | dynamic + PackBits | hardware-validated output, cut, completion, and five-label batching |
| `20DF` | PT-D410 | 128 at 180 dpi | 3.5, 6, 9, 12, 18 | legacy raw + D4 finalize | experimental recipe; explicit opt-in |
| `20E0` | PT-D460BT | 128 at 180 dpi | 3.5, 6, 9, 12, 18 | dynamic raw + D4 finalize | experimental recipe; explicit opt-in |
| `20E1` | PT-D610BT | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | dynamic raw + D4 finalize | experimental recipe; explicit opt-in |
| `2201` | PT-E310BT | 128 at 180 dpi | 3.5, 6, 9, 12, 18 | dynamic raw + D4 finalize | experimental recipe; explicit opt-in |
| `2203` | PT-E560BT | 128 at 180 dpi | 3.5, 6, 9, 12, 18, 24 | dynamic raw + D4 finalize | experimental recipe; explicit opt-in |

All 23 profiles can build a host-tested stream; that does not claim 23 models
have been physically proven. PT-P710BT and PT-1950 are enabled by default at
their stated evidence tiers. The other 21 are refused unless
`allow_experimental_profiles` is explicitly enabled. Any profile without
validated completion returns `PTOUCH_USB_COMPLETION_UNVALIDATED` after nominal
completion, latches the output quarantine, and requires physical inspection.
Applications must not retry a result with `stream_submitted=true`. Chaining
currently remains PT-P710BT-only.

## Recognized but refused

| USB PID | Identity | Refusal |
|---|---|---|
| `200D` | PT-3600 | known unsupported profile |
| `2030` | PT-1230PC P-Lite | switch off P-Lite mode |
| `2031` | PT-2430PC P-Lite | switch off P-Lite mode |
| `2060` | PT-E550W | incompatible raster transfer |
| `2064` | PT-P700 Editor Lite | switch off Editor Lite mode |
| `2065` | PT-P750W Editor Lite | switch off Editor Lite mode |

## PT-1950 acceptance

PT-1950 is the first non-P710 physical acceptance target. It is validated for
inspected, single-label output and cutting. Completion and batch chaining
deliberately remain unvalidated.

Evidence captured against the final command recipe:

1. USB reports VID/PID `04F9:2019`, profile `PT-1950`, endpoints `02/81`, and
   Brother status model `0x35` with 12 mm laminated tape admitted.
2. The built stream has exactly `ESC i M 42`, `ESC i K 08`, `M 02`, raster
   rows, and terminal `1A`; it contains no `ESC i R 01`, `d`, `A`, or print
   information.
3. A 293-dot (41.35 mm) control printed correctly and auto-cut. A deliberately
   asymmetric two-line 100-dot (14.1 mm) label was upright and unclipped and
   also auto-cut.
4. The short label produced an approximately 37 mm strip. The printer placed
   its manual cut guide about 24 mm from the leading edge, matching the fixed
   head-to-cutter leader. No synthetic raster-length floor is added; the prior
   180-dot padding produced an approximately 46 mm strip and wasted about 9 mm.
5. Passive status was idle `0x00`, printing phase `0x06`, then completion
   `0x01`. That nominal sequence is not sufficient to distinguish current-job
   physical success from stale or merely submitted output, so
   `completion_validated` remains false.
6. Each provisional completion was physically inspected and confirmed through
   the purpose-bound web action. A fresh idle status and subsequent preflight
   succeeded after clearing the durable hold.

The PT-1950 has only a full cutter. Keeping individual auto-cut enabled also
keeps the narrow-margin `:` guide; suppressing the guide would disable the
requested cut behavior. Consecutive-label optimization remains unavailable
until this model has a separately proven chaining recipe.

## Sources and test contract

- Brother's official [P-touch raster command reference](https://download.brother.com/welcome/docp100064/cv_pte550wp750wp710bt_eng_raster_102.pdf)
  defines command/status semantics for the documented modern family.
- Brother's official [PT-1950/1960 Windows driver 6.02.00.00](https://download.brother.com/welcome/dlfp001567/pd1950w550axpus.exe)
  provides the legacy model's default command order. The package and raster
  DLL hashes are recorded in this component's `NOTICE`.
- Brother's [3.5 mm detection FAQ](https://support.brother.com/g/b/faqend.aspx?c=gb&faqid=faqp00001029_000&lang=en&prod=2420euk)
  documents the 6 mm reported value on PT-1950, PT-2420PC, PT-2450DX, and
  PT-9200DX.
- Brother's [25 mm P-touch Editor FAQ](https://support.brother.com/g/b/faqend.aspx?c=au&faqid=faqp00000210_001&lang=en&prod=2730eas)
  lists PT-1950 and specifies a one-inch (nominally 25 mm) output floor when
  Auto Cut is selected.
- Brother's [PT-1950 cut-guide FAQ](https://support.brother.com/g/b/sp/faqend.aspx?c=gb&faqid=faqp00000929_003&ftype3=615&lang=en&prod=1950euk)
  confirms that the two printed dots mark the manual cut position used with
  narrow margins.
- Standard-tape maxima were checked against Brother product specifications and
  manuals; the auditable source record is in [PROVENANCE.md](PROVENANCE.md).
- The compatibility catalog was independently expressed from documented USB
  identities and semantic interoperability facts. No GPL source or data
  structure is copied into the Apache-2.0 component.

Host gates:

```bash
bash host/build.sh
```

The model suite builds every available recipe and pins all refused identities,
profile invariants,
standard media admission, malformed profiles, exact P710 regression streams,
asymmetric PT-1950/P900 raster placement, frame-table validation, and USB
descriptor rejection.
