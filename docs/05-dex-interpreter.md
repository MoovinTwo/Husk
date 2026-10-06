# The Dex interpreter (older translation layer)

`src/translation-layer/husk-tl-dex.c` is a small Dalvik bytecode interpreter
that the older translation layer (the "attempt" in `husk-tl-load.c`) falls back
to when an APK has no arm64 libraries, or none with `ANativeActivity_onCreate`.
It runs an app's `classes*.dex` directly, against the hand-written framework
shims in `husk-tl-framework.c` (Canvas, Paint, Bitmap, Rect, View and friends),
`husk-tl-res.c` (the resource table) and `husk-tl-prefs.c`
(SharedPreferences). Canvas calls are rasterised in software
(`husk-tl-blit.c`) into a framebuffer the app shows. It was written to run one simple pure-Java game
(a Flappy Bird clone) end to end, and that is the extent of what it targets.

It is not ART, contains no AOSP code, and is GPL-2.0-or-later like the rest
of Husk. The native runtime in `src/translation-layer-next` does not use it:
that runtime runs games whose logic is native code and answers their JNI calls
from C (see [04-translation-layer.md](04-translation-layer.md)).

## What it does

- Parses every `classes*.dex` in the APK (`tl_dex_load_apk`), builds a class
  table, and lays out instance fields including inherited ones
  (`instance_base` / `instance_size`), with enums' name and ordinal slots.
- Interprets the standard Dalvik opcode set: moves, constants (including
  strings and classes), all integer, long, float and double arithmetic and
  conversions with Java's semantics (wrapping overflow, masked shift counts,
  `INT_MIN / -1`), compares and branches, `packed-switch`/`sparse-switch`,
  arrays (`new-array`, `filled-new-array`, `fill-array-data`, typed `aget`/`aput`
  with index and width checks), instance and static fields, and the five
  `invoke-*` kinds in both normal and `/range` forms.
- Runs a class's `<clinit>` lazily, on first `new-instance`, static field access
  or static call, after its superclass's. The class is marked initialised
  before its initialiser runs, so a `<clinit>` that touches its own class does
  not recurse; eager initialisation of every class was tried and rejected
  because it runs Play Services/AndroidX initialisers the app never asked for.
- Calls a framework shim when a method resolves to one; a framework method
  with neither bytecode nor shim logs its name once and returns zero.
- An unimplemented opcode ends the current call and is logged once per opcode,
  rather than being stepped over.
- A deadline-paced frame pump (`tl_pacer_*`) drives the app's `doFrame`/`onDraw`
  at 60 fps, with touch input queued from the UI thread.

`tools/dex-test.c` runs it on a Mac against an APK and saves frames; it is not
part of `tests/translation-layer/run.sh`.

## Known limits

These are properties of the code as it stands, not plans.

- **No exceptions.** `throw` is not implemented (it ends the call as an
  unimplemented opcode), try/catch tables are ignored, and conditions that
  would throw in Java instead carry on: division by zero gives 0, a negative
  array size gives an empty array, an out-of-range array access or a field
  access on null reads zero and writes nowhere.
- **Type checks are no-ops.** `instance-of` returns true for any non-null
  reference, whatever the type; `check-cast` does nothing.
- **No locking.** `monitor-enter`/`monitor-exit` do nothing; the interpreter
  assumes one thread runs app code.
- **Static dispatch.** A call is resolved once, from the class named in the
  method reference (walking up to its superclasses), not from the receiver's
  runtime class. An overriding method in a subclass is not chosen when the
  call names the parent, and an interface call reaches a shim or nothing.
- **Lazy `<clinit>` only roughly follows the JVM.** There is no
  "initialisation in progress on another thread" state and no record of a
  failed initialiser; a class counts as initialised as soon as its
  initialiser starts.
- **No bounds checks on registers or code.** Register numbers taken from the
  bytecode (up to 255, or 65535 for `/range` calls) index the frame without
  being checked against `registers_size`, and operands are read past `pc`
  without checking the method's length. Branch targets are not validated.
  The Dex file is untrusted input from the APK, so a crafted APK can make the
  interpreter read and write outside its frame. Nothing here is a sandbox.
- **No recursion limit.** Each Java call is a C call; deep Java recursion
  overflows the native stack.
- **Objects are never freed.** Objects, arrays and strings are `calloc`ed and
  never released (there is no garbage collector); `tl_dex_context_destroy`
  frees classes and DEX buffers, not objects.
- **No `invoke-polymorphic`, `invoke-custom`, `const-method-handle` or
  `const-method-type`** (lambdas compiled to those, and method handles).
- **`const-class`** yields the interpreter's own class structure as the
  reference, which only shims that expect it can use.
- **Framework coverage is whatever `husk-tl-framework.c` implements**, which
  is the set one game needed. Missing methods are named once in the log.
