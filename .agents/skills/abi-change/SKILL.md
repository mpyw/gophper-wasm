---
name: abi-change
description: Change the contract between gophper-wasm's binaries and the gophper host - add, remove or change a "gophper" host function, an exported global, or a shared constant. Use before editing ABI.md or a GOPHPER_IMPORT in compat/.
---

# Change the ABI

ABI.md is the whole contract. `phpwasm.ABIVersion` numbers it, and gophper refuses binaries with another one.

## Steps

1. Change ABI.md first: the function's signature, what it returns, and when the host returns `EINTR` or `EIO`.
2. Bump `ABIVersion` in `phpwasm.go`.
3. Import it in compat with `GOPHPER_IMPORT("name")`, as the other files do.
4. `phpwasm_test.go` stubs every "gophper" import. Extend it if the new function changes what the stub must return.
5. Implement the host side in gophper, and set its `engineABIVersion` to the same number (gophper's `gophper-wasm-upgrade` skill).
6. Release as a minor version (release skill).

## Rejected designs

| Idea | Why not |
| --- | --- |
| Calling a Go function again when its result does not fit | It may have side effects. `fn_call` keeps the result on the host, and `fn_take` copies it into a buffer of the right size. |
