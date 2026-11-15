# TinyOTA Edge SDK Partner Distribution Model

This document defines the HAL-open / core-closed delivery model for partner integration.

## Goals

- Keep porting flexibility for partner teams by opening HAL and platform contracts.
- Protect core update engine implementation details.
- Keep integration stable with a versioned C API and ABI-compatible release policy.

## Open Boundary (partner-visible)

- Public headers:
  - include/fota_hal.h
  - include/fota_platform.h
  - include/fota_fsm.h
- Porting guide and conformance docs.
- HAL implementation templates/examples.

## Closed Boundary

- FSM internal implementation details.
- Patch engine implementation details and optimizations.
- Internal diagnostics and tuning internals.

In partner distribution mode, install output should include only:

- include/fota/*.h
- lib/libfota_sdk.a or fota_sdk.lib
- optional bin/fota_sdk_sample for reference testing

Reference source installation is disabled by default in CMake.

## Partner Package Rules

- Do not install SDK source files in the partner package.
- Ship headers + prebuilt library artifacts.
- Keep install output limited to items required for integration and validation.
- `fota_fsm.c`, `fota_common.c` and `bspatch.c` stay inside the prebuilt `fota_sdk` library.

## Integrator Responsibilities

- Implement all HAL functions from fota_hal.h.
- Implement platform seams (HTTP GET/POST, SHA-256, ECDSA verify) from fota_platform.h.
  JSON helpers, `fota_register()` and `fota_report_status()` come from `fota_common.c`.
- Run conformance checklist before production rollout.

## Delivery Checklist

- Public headers reviewed for compatibility.
- Conformance checklist attached to partner handoff.
- Install package contains no SDK source files.
