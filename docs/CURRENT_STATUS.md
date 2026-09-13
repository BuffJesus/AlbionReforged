# Current project status

Updated September 13, 2026. The active direction is the hybrid recomp-to-native
port. The prior standalone reconstruction is discontinued as a product, but its
format, rendering, audio, animation and scripting code remains reusable.

Independent native services now cover BNK v3 archives and decoded ranges, retained
byte sources and handles, indexed resource selection, PCM/WAVE parsing, manifest
framing and counted UTF-16 normalization. Fifteen Windows contract suites pass.
The diagnostic adapter preserves original guest execution; no guest path is retired.

Local TU1 visual testing has verified responsive startup progress, creation of
305 cached pipelines, and title/menu interaction. A previous initial black-screen
pause did not recur in the latest pass, but is not proven permanently fixed.
Earlier passes reached childhood/Fairfax. Temporal Lionhead/breadcrumb flashing,
new-route stutter, audio fidelity and comprehensive save coverage remain open.

Resource integration is the next gate: manifest tokens identify stored names
which delegate to parent providers, not archive indexes. A bounded manifest-name
sidecar is built and headless-tested; live source identification and completed
read byte/status/lifetime comparison remain outstanding.

Work sequence: recover original contracts, implement host-owned services, compare
against actual guest behavior, enable bounded replacements, then retire their
compatibility dependencies. Move audio, scripting, rendering and clocks through
those same gates. Do not equate recovered functions or green fixtures with a
completed engine migration.

Detailed local reports and candidate hashes remain in the integration workspace.
This public summary does not distribute original game code/data or runtime binaries.
