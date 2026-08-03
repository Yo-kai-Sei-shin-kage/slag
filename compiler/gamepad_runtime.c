// Descriptor-driven controller runtime. See gamepad_runtime.h for the model.
//
// HID struct offsets (mingw x64, verified):
//   HIDP_CAPS (64B): Usage@0 UsagePage@2 NumberInputButtonCaps@46
//                    NumberInputValueCaps@48
//   HIDP_VALUE_CAPS (72B): UsagePage@0 ReportID@2 IsRange@12
//                          LogicalMin@40 LogicalMax@44 NotRange.Usage@56
//   HIDP_BUTTON_CAPS (72B): UsagePage@0 IsRange@12 NotRange.Usage@56
//                           Range.UsageMin@56 Range.UsageMax@58
//
// HidP_* signatures (NTSTATUS >= 0 on success; HIDP_STATUS_SUCCESS == 0x110000):
//   HidP_GetCaps(Preparsed, *Caps)
//   HidP_GetValueCaps(Type, *ValueCaps, *Length(PUSHORT), Preparsed)
//   HidP_GetUsageValue(Type, Page, LinkColl, Usage, *Value(PULONG),
//                      Preparsed, Report, ReportLen)          -- 8 args
//   HidP_GetUsages(Type, Page, LinkColl, *UsageList, *UsageLen,
//                  Preparsed, Report, ReportLen)              -- 8 args
// HidP_Input = 0. Generic Desktop page = 1, Button page = 9.

#include "gamepad_runtime.h"

#define E(s) cg_emit(cg, "%s", s)

void emit_gamepad_imports(Codegen *cg) {
    E("; --- Raw Input + HID descriptor imports (user32 + hid) ---");
    E("extern RegisterRawInputDevices");
    E("extern GetRawInputData");
    E("extern GetRawInputDeviceInfoW");
    E("extern HidP_GetCaps");
    E("extern HidP_GetValueCaps");
    E("extern HidP_GetUsageValue");
    E("extern HidP_GetUsages");
}

void emit_gamepad_data(Codegen *cg) {
    E("; --- controller latched state (edge-diff source) ---");
    E("_gpad_cur_btn:   dq 0        ; decoded button bitmask (this report)");
    E("_gpad_cur_lx:    dq 0        ; sticks: signed -32768..32767");
    E("_gpad_cur_ly:    dq 0");
    E("_gpad_cur_rx:    dq 0");
    E("_gpad_cur_ry:    dq 0");
    E("_gpad_cur_z:     dq 0        ; shared trigger axis: signed -32767..32767");
    E("_gpad_cur_lt:    dq 0        ; triggers: 0..32767 (derived from Z)");
    E("_gpad_cur_rt:    dq 0");
    E("_gpad_prev_btn:  dq 0");
    E("_gpad_have:      dq 0        ; 1 once a report has ever been seen");
    E("; --- HID descriptor cache (parsed once per device) ---");
    E("_gpad_learned:   dq 0        ; 1 once caps parsed");
    E("_gpad_ppd:       dq 0        ; preparsed data ptr (into _gpad_ppd_buf)");
    E("_gpad_rptlen:    dq 0        ; caps.InputReportByteLength");
    E("_gpad_cur_hat:   dq 0        ; live hat value (0 centered, 1..8 dirs)");
    E("; per-axis cap slots: LX LY RX RY LT RT (index 0..5). Each slot:");
    E(";   +0 present(dq)  +8 lmin(dq, signed)  +16 lmax(dq, signed)");
    E(";   (usage is implicit by slot; scale computed at decode)");
    E("_gpad_axis:      times (6*3) dq 0");
    E("_gpad_val:       dd 0        ; HidP_GetUsageValue out (ULONG)");
    E("_gpad_vcaps_len: dw 0        ; HidP_GetValueCaps in/out length");
    E("_gpad_ulen:      dd 0        ; HidP_GetUsages usage-list length");
    E("_gpad_caps:      times 64 db 0     ; HIDP_CAPS");
    E("_gpad_vcaps:     times (32*72) db 0 ; up to 32 HIDP_VALUE_CAPS");
    E("_gpad_ulist:     times 128 dw 0    ; pressed-button usage list");
    E("    align 16");
    E("_gpad_ppd_buf:   times 4096 db 0   ; preparsed data buffer");
    E("_gpad_raw:       times 1024 db 0   ; RAWINPUT scratch (8B-aligned)");
    E("_gpad_ppd_size:  dd 4096");
    E("; slot->usage and slot->dest tables (indexed by decode-loop slot 0..4)");
    E("_gpad_usage:  dw 0x30, 0x31, 0x33, 0x34, 0x32");
    E("_gpad_dest:   dq _gpad_cur_lx, _gpad_cur_ly, _gpad_cur_rx, _gpad_cur_ry, _gpad_cur_z");
}

void emit_gamepad_runtime(Codegen *cg) {
    E("; ===================================================================");
    E("; Controller input pipeline (HID descriptor driven)");
    E("; ===================================================================");
    E("RID_INPUT        equ 0x10000003");
    E("RIDI_PREPARSEDDATA equ 0x20000005");
    E("HIDP_INPUT       equ 0");
    E("HIDP_STATUS_SUCCESS equ 0x00110000");
    E("HID_PAGE_DESKTOP equ 1");
    E("HID_PAGE_BUTTON  equ 9");
    E("");

    // -----------------------------------------------------------------
    // _slag_gamepad_dispatch(rcx = hRawInput) -- WM_INPUT entry.
    // Fetch the report + hDevice, ensure caps learned, then decode.
    // -----------------------------------------------------------------
    E("; --- _slag_gamepad_dispatch --- rcx = hRawInput (WM_INPUT lParam)");
    E("_slag_gamepad_dispatch:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    sub  rsp, 72                 ; shadow(32)+locals; rsp%16==0 at call");
    E("    mov  rsi, rcx                ; hRawInput");
    E("    ; GetRawInputData(hRawInput, RID_INPUT, _gpad_raw, &pcb=1024, 24)");
    E("    mov  dword [rsp+56], 1024");
    E("    mov  rcx, rsi");
    E("    mov  edx, RID_INPUT");
    E("    lea  r8,  [_gpad_raw]");
    E("    lea  r9,  [rsp+56]");
    E("    mov  dword [rsp+32], 24");
    E("    call GetRawInputData");
    E("    cmp  eax, -1");
    E("    je   .gpd_ret");
    E("    ; hDevice = RAWINPUTHEADER.hDevice @ _gpad_raw+8");
    E("    mov  rbx, [_gpad_raw + 8]    ; rbx = hDevice");
    E("    ; ensure caps learned for this device");
    E("    cmp  qword [_gpad_learned], 0");
    E("    jne  .gpd_decode");
    E("    mov  rcx, rbx");
    E("    call _slag_gamepad_learn");
    E("    cmp  qword [_gpad_learned], 0");
    E("    je   .gpd_ret                ; learn failed -> ignore report");
    E(".gpd_decode:");
    E("    call _slag_gamepad_decode");
    E(".gpd_ret:");
    E("    add  rsp, 72");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // -----------------------------------------------------------------
    // _slag_gamepad_learn(rcx = hDevice)
    //   preparsed = GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, buf, &sz)
    //   HidP_GetCaps ; HidP_GetValueCaps ; build _gpad_axis[] by usage.
    // -----------------------------------------------------------------
    E("; --- _slag_gamepad_learn --- rcx = hDevice");
    E("_slag_gamepad_learn:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    sub  rsp, 40");
    E("    ; GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, _gpad_ppd_buf, &size)");
    E("    mov  edx, RIDI_PREPARSEDDATA");
    E("    lea  r8,  [_gpad_ppd_buf]");
    E("    lea  r9,  [_gpad_ppd_size]");
    E("    mov  dword [_gpad_ppd_size], 4096");
    E("    call GetRawInputDeviceInfoW");
    E("    cmp  eax, -1");
    E("    je   .learn_fail");
    E("    lea  rax, [_gpad_ppd_buf]");
    E("    mov  [_gpad_ppd], rax");
    E("    ; HidP_GetCaps(ppd, &_gpad_caps)");
    E("    mov  rcx, [_gpad_ppd]");
    E("    lea  rdx, [_gpad_caps]");
    E("    call HidP_GetCaps");
    E("    cmp  eax, HIDP_STATUS_SUCCESS");
    E("    jne  .learn_fail");
    E("    movzx eax, word [_gpad_caps + 4]   ; InputReportByteLength");
    E("    mov  [_gpad_rptlen], rax");
    E("    ; length = NumberInputValueCaps (word @ _gpad_caps+48), cap to 32");
    E("    movzx eax, word [_gpad_caps + 48]");
    E("    cmp   ax, 32");
    E("    jbe   .learn_vlen_ok");
    E("    mov   ax, 32");
    E(".learn_vlen_ok:");
    E("    mov   [_gpad_vcaps_len], ax");
    E("    ; HidP_GetValueCaps(HIDP_INPUT, _gpad_vcaps, &len, ppd)");
    E("    xor  ecx, ecx                ; HIDP_INPUT");
    E("    lea  rdx, [_gpad_vcaps]");
    E("    lea  r8,  [_gpad_vcaps_len]");
    E("    mov  r9,  [_gpad_ppd]");
    E("    call HidP_GetValueCaps");
    E("    cmp  eax, HIDP_STATUS_SUCCESS");
    E("    jne  .learn_fail");
    E("    ; clear axis table");
    E("    lea  rdi, [_gpad_axis]");
    E("    mov  rcx, 6*3");
    E("    xor  rax, rax");
    E("    rep  stosq");
    E("    ; iterate value caps: for each entry map desktop-page usage -> slot.");
    E("    movzx r12d, word [_gpad_vcaps_len]  ; count");
    E("    lea  r13, [_gpad_vcaps]             ; entry ptr");
    E("    xor  r14, r14                       ; i");
    E(".learn_loop:");
    E("    cmp  r14, r12");
    E("    jae  .learn_done");
    E("    ; only Generic Desktop page (UsagePage @0)");
    E("    cmp  word [r13 + 0], HID_PAGE_DESKTOP");
    E("    jne  .learn_next");
    E("    ; usage @ NotRange.Usage +56 (IsRange assumed 0 for axes)");
    E("    movzx eax, word [r13 + 56]          ; usage");
    E("    ; map usage -> slot: 0 LX(X 0x30) 1 LY(Y 0x31) 2 RX(Rx 0x33)");
    E("    ; 3 RY(Ry 0x34) 4 Z(shared trigger 0x32). Others (hat) unhandled here.");
    E("    xor  ebx, ebx                       ; slot");
    E("    cmp  eax, 0x30");
    E("    je   .lm_lx");
    E("    cmp  eax, 0x31");
    E("    je   .lm_ly");
    E("    cmp  eax, 0x33");
    E("    je   .lm_rx");
    E("    cmp  eax, 0x34");
    E("    je   .lm_ry");
    E("    cmp  eax, 0x32");
    E("    je   .lm_z");
    E("    jmp  .learn_next                    ; unhandled usage (hat read raw)");
    E(".lm_lx: mov ebx, 0 \n    jmp .lm_store");
    E(".lm_ly: mov ebx, 1 \n    jmp .lm_store");
    E(".lm_rx: mov ebx, 2 \n    jmp .lm_store");
    E(".lm_ry: mov ebx, 3 \n    jmp .lm_store");
    E(".lm_z:  mov ebx, 4");
    E(".lm_store:");
    E("    ; slot base = _gpad_axis + slot*24");
    E("    lea  rdi, [_gpad_axis]");
    E("    lea  r15, [rbx + rbx*2]             ; slot*3");
    E("    lea  rdi, [rdi + r15*8]             ; +slot*24");
    E("    mov  qword [rdi + 0], 1             ; present");
    E("    ; LogicalMin @40, LogicalMax @44. These 16-bit-report axes store their");
    E("    ; range in the low word: an unsigned axis (min>=0) reports max as the");
    E("    ; all-ones word 0xFFFF (=-1 as a 32-bit LONG), whose true value is");
    E("    ; 65535 -> read the max as an UNSIGNED WORD (movzx), never the full");
    E("    ; 32-bit field (which would give 0xFFFFFFFF and blow up the span).");
    E("    ; A signed axis (min<0) keeps its signed word range.");
    E("    movsxd rax, dword [r13 + 40]        ; LogicalMin (signed 32)");
    E("    mov  [rdi + 8], rax");
    E("    test rax, rax");
    E("    js   .lm_signed");
    E("    movzx eax, word [r13 + 44]          ; LogicalMax as unsigned 16-bit");
    E("    mov  [rdi + 16], rax");
    E("    jmp  .lm_done");
    E(".lm_signed:");
    E("    movsx rax, word [r13 + 44]          ; LogicalMax as signed 16-bit");
    E("    mov  [rdi + 16], rax");
    E(".lm_done:");
    E(".learn_next:");
    E("    add  r13, 72");
    E("    inc  r14");
    E("    jmp  .learn_loop");
    E(".learn_done:");
    E("    mov  qword [_gpad_learned], 1");
    E(".learn_fail:");
    E("    add  rsp, 40");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // -----------------------------------------------------------------
    // _slag_gamepad_decode : read each present axis via HidP_GetUsageValue,
    // normalize per its logical range, latch, then edge-diff dispatch.
    // Report bytes @ _gpad_raw+32, dwSizeHid @ _gpad_raw+24.
    // -----------------------------------------------------------------
    E("; --- _slag_gamepad_decode ---");
    E("_slag_gamepad_decode:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    sub  rsp, 72                 ; shadow(32)+stack args(24)+pad");
    E("    ; report ptr = _gpad_raw+32 (report[0]=id). Report length MUST be the");
    E("    ; ACTUAL dwSizeHid of this report (@_gpad_raw+24), not the cached");
    E("    ; caps.InputReportByteLength: reports vary in size (e.g. the hat/dpad");
    E("    ; only appears in a larger report), and HidP_* extract nothing past a");
    E("    ; too-short length. Using the real per-report length is controller-");
    E("    ; agnostic and makes every declared usage decode on the report carrying it.");
    E("    mov  r15d, dword [_gpad_raw + 24]   ; r15 = dwSizeHid (this report)");
    E("    ; slots: 0 LX 1 LY 2 RX 3 RY 4 Z(shared trigger). All decode as signed");
    E("    ; -32768..32767; Z is split into lt/rt after the loop.");
    E("    xor  r14, r14                ; slot index");
    E(".dec_loop:");
    E("    cmp  r14, 5");
    E("    jae  .dec_trig_split");
    E("    ; slot base");
    E("    lea  rbx, [_gpad_axis]");
    E("    lea  rax, [r14 + r14*2]      ; slot*3");
    E("    lea  rbx, [rbx + rax*8]      ; +slot*24");
    E("    cmp  qword [rbx + 0], 0      ; present?");
    E("    je   .dec_next");
    E("    ; usage for this slot from table _gpad_usage[slot]");
    E("    lea  rax, [_gpad_usage]");
    E("    movzx r13d, word [rax + r14*2]   ; usage");
    E("    ; HidP_GetUsageValue(HIDP_INPUT, DESKTOP, 0, usage, &_gpad_val, ppd, report, len)");
    E("    xor  ecx, ecx                ; HIDP_INPUT");
    E("    mov  edx, HID_PAGE_DESKTOP");
    E("    xor  r8d, r8d                ; LinkCollection 0");
    E("    mov  r9d, r13d               ; usage");
    E("    lea  rax, [_gpad_val]");
    E("    mov  [rsp+32], rax           ; arg5 *value");
    E("    mov  rax, [_gpad_ppd]");
    E("    mov  [rsp+40], rax           ; arg6 preparsed");
    E("    lea  rax, [_gpad_raw + 32]");
    E("    mov  [rsp+48], rax           ; arg7 report");
    E("    mov  [rsp+56], r15           ; arg8 report length");
    E("    call HidP_GetUsageValue");
    E("    cmp  eax, HIDP_STATUS_SUCCESS");
    E("    jne  .dec_next");
    E("    ; raw is a ULONG report field: zero-extend it. Signedness of the axis");
    E("    ; lives entirely in lmin/lmax; offset = raw - lmin is in [0, span].");
    E("    mov  r11d, dword [_gpad_val] ; raw, zero-extended into r11");
    E("    mov  r8,  [rbx + 8]          ; lmin (signed 64)");
    E("    mov  r9,  [rbx + 16]         ; lmax (extended per axis signedness)");
    E("    ; span = lmax - lmin (guard <= 0)");
    E("    mov  r10, r9");
    E("    sub  r10, r8                 ; span");
    E("    test r10, r10");
    E("    jle  .dec_next");
    E("    sub  r11, r8                 ; raw - lmin  (0..span)");
    E("    ; all slots -> signed -32768..32767: v = (r11*65535)/span - 32768");
    E("    mov  rax, r11");
    E("    mov  rcx, 65535");
    E("    imul rax, rcx");
    E("    cqo");
    E("    idiv r10                     ; 0..65535");
    E("    sub  rax, 32768");
    E("    ; store into cur slot by index (0..4 -> lx ly rx ry z)");
    E("    lea  rcx, [_gpad_dest]");
    E("    mov  rcx, [rcx + r14*8]      ; dest address");
    E("    mov  [rcx], rax");
    E(".dec_next:");
    E("    inc  r14");
    E("    jmp  .dec_loop");
    E("");
    E(".dec_trig_split:");
    E("    ; shared trigger Z (cur_z, signed -32767..32767): split into lt/rt.");
    E("    ; Empirically on this pad Z>0 is LT and Z<0 is RT (polarity confirmed");
    E("    ; via pad_diag). LT = Z when Z>0 else 0; RT = -Z when Z<0 else 0.");
    E("    mov  rax, [_gpad_cur_z]");
    E("    xor  rcx, rcx                ; lt");
    E("    xor  rdx, rdx                ; rt");
    E("    test rax, rax");
    E("    jz   .trig_done");
    E("    js   .trig_neg");
    E("    mov  rcx, rax                ; positive -> LT");
    E("    jmp  .trig_done");
    E(".trig_neg:");
    E("    neg  rax");
    E("    mov  rdx, rax                ; |negative| -> RT");
    E(".trig_done:");
    E("    mov  [_gpad_cur_lt], rcx");
    E("    mov  [_gpad_cur_rt], rdx");
    E("");
    E(".dec_hat:");
    E("    ; hat switch (dpad): read report byte [13] directly. The HidP usage");
    E("    ; extraction is unreliable across this pad's variable report sizes, but");
    E("    ; the raw hat byte is deterministic: 0 = centered, 1..8 = 8 directions");
    E("    ; (1=Up, then clockwise). Empirically confirmed via pad_diag.");
    E("    movzx eax, byte [_gpad_raw + 32 + 13]");
    E("    mov  [_gpad_cur_hat], rax");
    E("");
    E(".dec_buttons:");
    E("    ; buttons: HidP_GetUsages(HIDP_INPUT, BUTTON, 0, _gpad_ulist, &ulen, ppd, report, len)");
    E("    mov  dword [_gpad_ulen], 128");
    E("    xor  ecx, ecx");
    E("    mov  edx, HID_PAGE_BUTTON");
    E("    xor  r8d, r8d");
    E("    lea  r9,  [_gpad_ulist]");
    E("    lea  rax, [_gpad_ulen]");
    E("    mov  [rsp+32], rax");
    E("    mov  rax, [_gpad_ppd]");
    E("    mov  [rsp+40], rax");
    E("    lea  rax, [_gpad_raw + 32]");
    E("    mov  [rsp+48], rax");
    E("    mov  [rsp+56], r15");
    E("    call HidP_GetUsages");
    E("    xor  r10, r10                ; button bitmask");
    E("    cmp  eax, HIDP_STATUS_SUCCESS");
    E("    jne  .dec_btn_done");
    E("    ; for each pressed usage u (1-based), set bit (u-1) if u<=32");
    E("    mov  ecx, dword [_gpad_ulen]");
    E("    lea  rsi, [_gpad_ulist]");
    E("    xor  rdi, rdi                ; i");
    E(".dec_btn_loop:");
    E("    cmp  rdi, rcx");
    E("    jae  .dec_btn_done");
    E("    movzx eax, word [rsi + rdi*2]");
    E("    test eax, eax");
    E("    jz   .dec_btn_skip");
    E("    dec  eax                     ; 0-based");
    E("    cmp  eax, 32");
    E("    jae  .dec_btn_skip");
    E("    mov  r11, 1");
    E("    mov  ecx, eax");
    E("    shl  r11, cl");
    E("    or   r10, r11");
    E("    mov  ecx, dword [_gpad_ulen]");
    E(".dec_btn_skip:");
    E("    inc  rdi");
    E("    jmp  .dec_btn_loop");
    E(".dec_btn_done:");
    E("    ; fold hat (dpad) into the button mask: bit13 Up, 14 Down, 15 Left,");
    E("    ; 16 Right. hat 1..8 clockwise from Up; 0/other = centered (no bits).");
    E("    mov  rax, [_gpad_cur_hat]");
    E("    test rax, rax");
    E("    jz   .dec_hat_done");
    E("    cmp  rax, 8");
    E("    ja   .dec_hat_done");
    E("    ; Up = {8,1,2}");
    E("    cmp  rax, 8 \n    je .dh_up");
    E("    cmp  rax, 2 \n    ja .dh_nup");
    E(".dh_up:  or r10, (1 << 13)");
    E(".dh_nup:");
    E("    ; Right = {2,3,4}");
    E("    cmp  rax, 2 \n    jb .dh_nrt");
    E("    cmp  rax, 4 \n    ja .dh_nrt");
    E("    or   r10, (1 << 16)");
    E(".dh_nrt:");
    E("    ; Down = {4,5,6}");
    E("    cmp  rax, 4 \n    jb .dh_ndn");
    E("    cmp  rax, 6 \n    ja .dh_ndn");
    E("    or   r10, (1 << 14)");
    E(".dh_ndn:");
    E("    ; Left = {6,7,8}");
    E("    cmp  rax, 6 \n    jb .dh_nlf");
    E("    cmp  rax, 8 \n    ja .dh_nlf");
    E("    or   r10, (1 << 15)");
    E(".dh_nlf:");
    E(".dec_hat_done:");
    E("    mov  [_gpad_cur_btn], r10");
    E("");
    // ---- Edge diff + dispatch to handlers ----
    E("    ; buttons: changed = cur XOR prev; per changed bit call handler");
    E("    mov   rax, [_gpad_cur_btn]");
    E("    xor   rax, [_gpad_prev_btn]");
    E("    mov   rbx, rax");
    E("    xor   r12, r12");
    E(".diff_loop:");
    E("    cmp   r12, 32");
    E("    jae   .diff_done");
    E("    bt    rbx, r12");
    E("    jnc   .diff_next");
    E("    mov   rax, [_gpad_cur_btn]");
    E("    mov   rcx, r12");
    E("    shr   rax, cl");
    E("    and   eax, 1");
    E("    mov   edx, eax");
    E("    mov   rcx, r12");
    E("    call  _slag_on_gpad_button");
    E(".diff_next:");
    E("    inc   r12");
    E("    jmp   .diff_loop");
    E(".diff_done:");
    E("    mov   rax, [_gpad_cur_btn]");
    E("    mov   [_gpad_prev_btn], rax");
    E("    mov   qword [_gpad_have], 1");
    E("");
    E("    add  rsp, 72");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");
}
