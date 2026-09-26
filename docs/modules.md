# modules

a module is a file you import. there is no package manager, no namespace, no
`module` keyword and no `require`.

```flint
# math.fl
export fn square(x) {
    return x * x
}
```

```flint
# main.fl
import "math.fl"
print(square(6))    # 36
```

`export` is the only thing that matters, and it is optional in practice. a
function without `export` is still global, because a module runs in the same
global table as everything else:

```flint
# util.fl
fn helper() { return 1 }        # no export
export fn public() { return 2 }
```

```flint
# main.fl
import "util.fl"
print(public())    # 2
print(helper())    # 1. also works. export is documentation here, not access control.
```

so `export` is a convention for marking what you meant to publish, not a
mechanism. nothing is private.

## import

`import` takes a string path and executes the file. it is a statement, and it
can appear anywhere a statement can:

```flint
let x = 1
import "setup.fl"
print(x)        # still 1. nothing is returned or scoped.
```

imports run once, in order, as the statement is reached. an import inside a
function body runs when that function is called, not when the file loads.

```flint
fn load() {
    import "lazy.fl"     # runs on the first call
    print("loaded")
}
```

that works, and it is a real thing to do. it is also a re-import on every
call: there is no module cache, so the file is re-executed each time and its
top-level code runs again.

importing the same file twice re-runs it. this is not an error.

## paths

paths resolve against the process working directory, not against the importing
file. this is the sharpest edge in the module system and it is not fixed.

```sh
$ flint main.fl          # run from the project root
```

```flint
import "lib/helpers.fl"     # resolved as ./lib/helpers.fl
```

so a script only finds its imports if it is run from the directory the paths
were written for. running the same script from two directories gives two
different results, or an error. the alternative is tracking a directory per
call frame, and nobody has needed that yet.

there is no search path, no `..` restriction, and no way to ask what a path
resolved to. the path is also not a path within a module system: two different
files can import the same file under different spellings and both will run.

## what a module can see

everything. all files share one global table, so:

- a name defined in one file is visible in every file
- a name defined twice is a collision, and the second one wins
- there is no `private`, and no way to get two files to have a same-named
  global mean different things

```flint
# a.fl
let shared = 1
```

```flint
# b.fl
let shared = 2
```

import both and `shared` is 2, whichever order you imported in. this is a real
hazard in a project with more than a handful of files, and the only mitigation
is a naming convention for module-level globals.

## failure

a module that fails to import reports the error and returns `nil`. the
importing script continues. there is no try, no catch, and no error value.

```flint
import "broken.fl"    # prints the error, continues
print("still here")
```

a module that does not exist is a runtime error, not a compile error, because
the import is not resolved until it runs.

```flint
import "nope.fl"    # error: could not open module file 'nope.fl'
```

a module whose top-level code throws gives you a stack trace whose innermost
frame is the module, and the importing script's frames are not listed, because
they are not part of the failure.

## cycles

a file that imports itself recurses until the frame limit and reports "Stack
overflow". there is no cycle detection. the trace is a hundred-odd identical
frames, which at least tells you what happened.

this is the thing to fix first if you extend the module system. the fix is a
set of in-progress paths: refuse to enter a file already on the stack.

## const across modules

a `const` is a property of the binding in the shared global table, so an
exported const is read-only for every file that imports it.

```flint
# limits.fl
export const LIMIT = 10
```

```flint
# main.fl
import "limits.fl"
print(LIMIT)      # 10
LIMIT = 99        # error: cannot assign to constant 'LIMIT'
```

the check happens when the assignment runs, not when the file is compiled,
because the import has not happened yet at compile time. see
[values.md](values.md).
