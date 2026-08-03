#ifndef GAMEPAD_RUNTIME_H
#define GAMEPAD_RUNTIME_H

#include "codegen_internal.h"

// Descriptor-driven controller runtime (any HID gamepad, any brand).
//
// Reports arrive via WM_INPUT (Raw Input, RIDEV_INPUTSINK). Instead of
// hardcoding byte offsets per brand, the device's own HID report descriptor is
// parsed via hid.dll (HidP_*), so axes/buttons are decoded by USAGE, not by a
// guessed layout. Preparsed data comes from GetRawInputDeviceInfoW with
// RIDI_PREPARSEDDATA -- no SetupAPI, no deployment dependency.
//
// Canonical usage mapping (Generic Desktop page 0x01):
//   X  -> LX   Y  -> LY   Rx -> RX   Ry -> RY   (signed -32768..32767)
//   Z  -> LT   Rz -> RT                          (0..32767)
//   Hat(0x39) -> dpad up/down/left/right bits
//   Button page 0x09 usages -> gpad_button ids (0-based)
//
// Slag-facing API (unchanged contract):
//   on gpad_button(int gpad_id, bool down)      -- boolean, edge-diffed
//   gpad.lx()/ly()/rx()/ry()   -> int signed -32768..32767   (polled)
//   gpad.lt()/rt()             -> int 0..32767               (polled)
void emit_gamepad_imports(Codegen *cg);
void emit_gamepad_data(Codegen *cg);
void emit_gamepad_runtime(Codegen *cg);

#endif
