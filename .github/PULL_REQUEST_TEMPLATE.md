## What

<!-- one or two sentences -->

## Why / linked plan

<!-- PLAN.md section or milestone, e.g. "M3 hub core, §9.2" -->

## Checklist

- [ ] Title follows Conventional Commits (`type(scope): subject`)
- [ ] `pio test -d test` passes (host tests)
- [ ] Touches actuation / limits / watchdogs / relay-off timer → PLAN.md §3.3 safety rules applied
- [ ] Shared wire format changed only in `lib/proto/proto.h` (tests updated)
