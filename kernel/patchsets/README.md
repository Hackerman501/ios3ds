# Per-version kernel patch sets

`patches.json` and `patches-*.json` in the parent directory are the 8C148
(4.2.1) recipes with fixed virtual addresses. They are the default and are
never edited by another version's build.

`4.1.json` is the 8B117 (iOS 4.1) set. It was derived from the 8C148 recipes by
checking every original byte sequence in the 8B117 kernelcache:

- `same-va`: the instruction is at the same address;
- `context-N`: 8–256 bytes of surrounding code from 8C148 occur exactly once
  in 8B117, which gives the new address;
- `anchor`: for short/generic instruction sequences, the address was derived
  from a nearby uniquely located function (UART TX data at 0x801dc4bc, CLCD
  backlight log at 0x80758408, memdev strategy's following code at 0x80072bf8)
  and the original bytes were checked at the result;
- `unique-orig`: the original bytes occur once in 8B117.

Every entry keeps the strict original-byte check, so a wrong kernelcache still
stops the build. Six cosmetic/optional patches whose code changed in 8B117
(gyro usage/dispatch, CLCD backlight log-enable and retry-off, the clock
controller's voltage lookup and the RTC wait literal) are deliberately absent;
see the stage-3 blocker log in `docs/multi-version.md`. No Apple code is stored
here beyond the same short verification patterns already used by 8C148's
recipes.
