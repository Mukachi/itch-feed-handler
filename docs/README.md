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
| [001](design/001-scope.md) | Scope and plan | Agreed |
| [002](design/002-order-book.md) | Order book | Measured |
| [003](design/003-events-directory-status.md) | Event handler, instrument directory, trades and market status | Measured |
| [004](design/004-decoder.md) | ITCH decoder | Measured |
