# Conformance Checklist

Use this checklist before accepting a partner HAL integration.

## A. Build and Link

- [ ] SDK public headers compile cleanly on target toolchain.
- [ ] Partner HAL object links with provided SDK library.
- [ ] No unresolved external symbols for HAL/platform seams.

## B. HAL Safety

- [ ] Active slot write attempt returns FOTA_HAL_ERR_PROTECTED.
- [ ] Active slot erase attempt returns FOTA_HAL_ERR_PROTECTED.
- [ ] Out-of-range read/write/erase returns FOTA_HAL_ERR_PARAM.
- [ ] Unaligned erase returns FOTA_HAL_ERR_ALIGN.

## C. Registration and Check

- [ ] Device serial read is stable across reboot.
- [ ] Device registration succeeds and token persists.
- [ ] Update check handles no-update response cleanly.

## D. FULL Update Path

- [ ] Download succeeds with expected payload size.
- [ ] SHA-256 check succeeds on valid payload.
- [ ] ECDSA verification succeeds with valid signature.
- [ ] Boot slot switch occurs only after verification success.
- [ ] Device reboots to new firmware slot successfully.

## E. Failure and Recovery

- [ ] Network drop during download recovers via retry/range resume.
- [ ] Corrupted payload causes verification failure and no slot switch.
- [ ] Power-cycle during download does not brick device.
- [ ] Failure returns FSM to recoverable state.

## F. DELTA Path (if enabled)

- [ ] Delta patch applies successfully against matching base version.
- [ ] Mismatched base version is rejected safely.
- [ ] Patched image passes hash/signature verification.

## G. Observability

- [ ] State transitions are logged with timestamps.
- [ ] Last error code is captured and reportable.
- [ ] Build/package metadata (sdk version, platform) is traceable.
