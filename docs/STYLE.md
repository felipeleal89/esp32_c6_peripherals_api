# Style Guide

## C Code

- Use clear and direct C code, prioritizing readability.
- Avoid dynamic allocation in critical paths.
- Return `esp_err_t` and validate arguments at function entry.
- Prefer explicit names for variables and macros.

## Structure

- Public API in `components/dht20_api/include/dht20_api.h`.
- Implementation in `components/dht20_api/src/dht20_api.c`.
- Public API in `components/display_api/include/display_api.h`.
- Implementation in `components/display_api/src/display_api.c`.
- Public API in `components/knob_api/include/knob_api.h`.
- Implementation in `components/knob_api/src/knob_api.c`.
- Usage/integration example in `main/main.c`.

## Headers

All source files should use:

```c
/*
 * SPDX-License-Identifier: 0BSD
 */
```

## Logs

- Use local example macros (`UART_PRINT_INFO/WARN/ERR`) to keep consistency.
- Avoid UART spam; prefer time-window aggregation when possible.
