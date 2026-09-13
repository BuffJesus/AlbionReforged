# TU1 diagnostic adapters

The table hook is opt-in via `FABLE2_TU1_TABLE_CAPTURE`, disabled by default, and
always forwards the original game functions. It requires the audited TU1 addresses
and a matching ReXGlue SDK; do not use its addresses for GOTY without a mapping.

After source installation it observes a bounded indexed lookup while its manager
lock is held. Storage is allocated beforehand. Output is written after the outer
lookup returns, using exclusive creation so existing evidence is not overwritten.

TU1TAB02 captures the manager table and supported directory names. TU1MAN01 in
`<table-output>.manifest` additionally captures up to 32 manifest-backed rows,
including exact counted UTF-16 names, parent identity, status and truncation count.
The readers under `tools/` reject malformed/truncated files. Captured table rows
are not proof that a particular parent call or completed read executed.

The fake-ABI forwarding harness and bounded-memory fixtures test sequencing,
limits, failures and observed mutation. They do not prove live ABI/locking parity
or atomic snapshots. No diagnostic replaces a guest read or identifies a physical
file solely from a parent address.
