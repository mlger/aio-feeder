# test — host unit tests (PLAN.md §11.1)

Unity on the `native` platform. Run all: `pio test -d test`. CI runs this on
every push (`.github/workflows/ci.yml`). Covers `lib/proto` and later
`node_core` (safety logic, DST handling, limits).
