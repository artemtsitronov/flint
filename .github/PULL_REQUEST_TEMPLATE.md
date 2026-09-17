## what and why

the problem this patch solves, and why this fix.

## checklist

- [ ] `make release` builds with zero warnings
- [ ] `make stress` is clean under asan and ubsan
- [ ] bug fix has a test under `tests/language/` with its `.expected` output
- [ ] tabs, 80 columns, no trailing whitespace, no reformatting of untouched lines
- [ ] no new dependencies, no compiler extensions
