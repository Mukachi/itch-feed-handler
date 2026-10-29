# Documentation

Every component gets a design note before any code is written. Decisions with real alternatives
get an ADR.

- **Design notes** (`design/`): one file per component, sections Proposal, Design, Spec, Tasks,
  Review. Status moves Draft → Agreed → Built → Measured. Two review points: Draft → Agreed
  (approved before code) and Built → Measured (results checked against the Spec).
- **ADRs** (`adr/`): Context, Decision, Consequences, Status. Only for choices a reviewer might
  question.

Only Spec and Review are mandatory. Any section can be one line.

## Design notes

| # | Component | Status |
|---|---|---|
| [001](design/001-scope.md) | Scope and plan | Measured |
| [002](design/002-order-book.md) | Order book | Measured |
| [003](design/003-events-directory-status.md) | Event handler, instrument directory, trades and market status | Measured |
| [004](design/004-decoder.md) | ITCH decoder | Measured |
| [005](design/005-sequencing-recovery.md) | Sequencing and recovery | Measured |
| [006](design/006-snapshots-replay.md) | Snapshots and replay | Measured |
| [007](design/007-receiver-framer.md) | Receiver and framer | Measured |
| [008](design/008-publication.md) | Publication | Measured |
| [009](design/009-recording-monitoring.md) | Recording and monitoring | Measured |
| [010](design/010-measurement.md) | Measurement | Measured |
| [011](design/011-snapshot-recovery.md) | Recovery from a snapshot in the program | Agreed |
