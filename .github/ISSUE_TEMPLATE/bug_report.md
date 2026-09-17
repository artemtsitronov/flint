---
name: bug report
about: something is broken
title: '[bug] '
labels: bug
assignees: ''
---

## what happens

the wrong output, the crash, or the error message you got.

## reproducer

the smallest script that triggers it. trim the noise.

```flint
# paste here
```

## expected

what you expected instead.

## environment

- os and arch: `uname -mrs`
- compiler: `cc --version`
- target: `make release`, `make stress`, or `make debug`

## if it crashed

the output of `make stress`, or a gdb backtrace. text, not a screenshot.
