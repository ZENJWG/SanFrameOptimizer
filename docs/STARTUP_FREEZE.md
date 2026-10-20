# Startup freeze with ImmediateBatching / StaticPropInstancing

## Confirmed failure (2026-10-18)

The running game continued processing input and game logic while the display
stopped updating. Debugger captures in `build/diagnostics` showed:

- GTA's camera update returned zero because device `BeginScene` returned
  `0x8876086C` (`D3DERR_INVALIDCALL`).
- The same device object had a different vtable address from the one recorded
  when submission hooks were installed. The new table contained copies of
  SanFrame's hook pointers.
- The BeginScene adapter searched only by vtable address. It could not find
  the original method for the copied table and manufactured INVALIDCALL
  without calling the original D3D9 method.

This explains why the thread was not blocked and why there were no subsequent
Present reports. The captures do not identify which component copied the table.

## Change

Device hook records now retain their COM object identity. If table lookup
fails after a table copy, the same device can still resolve its own original
method chain. Resource hooks retain table lookup because resource addresses
can be reused after destruction. Uninstall restores both the original table
and the device's current copied table when our hook still occupies the slot.

Both INI settings remain enabled. No game installation files were changed.

## Validation

The copied-table regression in `tests/SubmissionSafety.cpp` fails with
"copied device vtable made BeginScene fail" when the identity fallback is
removed. It passes with the fix and verifies draw interception and restoration
of both tables. The Release plugin builds successfully.

CTest: 7/8 passed. The separate `skygfx_constants` test still fails its recovery
check after unloading the Proper Shaders fixture. It does not link the
submission implementation changed here.

The rebuilt plugin requires an in-game restart test to confirm recovery of
the display in the user's full mod setup.
