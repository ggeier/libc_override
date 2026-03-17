# scanf `%m` Hybrid Override

The scanf `%m` override in this repository is hybrid.

- The preload library still calls the real libc scanner for tokenization and conversion.
- After libc reports success, the override rehomes successful `%m` results into the preloaded allocator when it can do so safely.
- The mock allocator exposes bridge helpers in `include/allocator_bridge.h` so the override can tell whether a returned pointer is already owned by the preloaded allocator and can bypass directly to libc `free` when needed.

This keeps the scanner behavior close to libc while still fixing the common allocator-mismatch case for caller-owned `%m` buffers.

## Supported Rehome Paths

- narrow input `%ms` and `%m[...]` -> rehome by `strlen() + 1`
- narrow input `%mc` -> rehome by parsed field width
- narrow input `%mls` / `%ml[...]` -> rehome by `wcslen() + 1`
- wide input `%ms` and `%m[...]` -> rehome by `strlen() + 1`
- wide input `%mls` / `%ml[...]` -> rehome by `wcslen() + 1`
- wide input `%mlc` -> rehome by parsed field width

## Documented Limitations

### Embedded NUL truncation

String-like `%m` rehome paths only see the final pointer that libc produced. They do not know the original logical item length. As a result, embedded NUL data truncates during rehome for `%ms`, `%m[...]`, `%mls`, and `%ml[...]`.

Example:

```text
Input bytes: 61 62 00 63 64 20
Format: %ms
libc-owned result before rehome: "ab\0cd..."
rehomed result after strlen():   "ab"
```

Wide-string cases have the same limitation with embedded `L'\0'`.

### Wide-input `%mc`

For wide-input `%mc` that returns `char *`, the post-scan byte length is not recoverable safely from the libc-owned buffer. The current implementation therefore leaves that libc pointer in place.

That path is marked in code with a `MEMORY LEAK` comment because a caller that later reaches only the custom allocator's `free` may not be able to release it.

### Late rehome allocation failure

If libc scanning succeeds but the rehome allocation fails afterward, the override preserves the already-committed scanf result and leaves the libc-owned pointer in place.

Example:

```text
Format: %ms %d
Input:  abc 42
Observed result: return value stays 2, the integer assignment stays visible,
and the %m pointer remains libc-owned.
```

Those branches are also marked with `MEMORY LEAK` comments in the implementation so they are easy to revisit if the project later prefers a different tradeoff.
