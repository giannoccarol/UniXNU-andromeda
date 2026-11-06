# Cepheus SCM firmware investigation, 2026-11-06 (static only)

Target: native AArch64 PE `ScmDxeLA.efi` from the existing Cepheus UEFI tree,
49,152 bytes, SHA256
`1a00f9793f8178ca9579b06f46aeec395415652d8925db5020516f1c94f2af50`.
REA/Ghidra12.1.2 inspected the original immutable binary; no firmware or phone
was executed. Full evidence: `../logs/scm-static-20261106/evidence.json`.

## Observations

- Protocol registration at `0x1524` selects the ARMv8 table `0xa120` after
  `0x2988` locates a shared allocator. It installs the interface through the
  saved UEFI Boot Services pointer (`+0x148`, InstallMultipleProtocolInterfaces).
  Evidence `ev_8011686680b0501a33b042eb1ddb7cc0b948846b593f50cf4371054cbf8c448c`.
- The 72-byte table starts with revision `0x10002`, followed by pointers
  `0x2dd4,0x2ddc,0x2de4,0x32bc,0x2dec,0x3474,0x2c18,0x3394`.
  Evidence `ev_a8fb7d44150bd348d32709d56c004ed0db76fee4ad0ecf8548584bd382a97e45`.
  **Correction:** the shipped table has one more entry than the available
  `EFIScm.h` struct. Offsets may be matched to that header, not blindly treated
  as a fully described ABI. `0x2c18` is a small entry stub Ghidra did not define
  as a function; direct analyze_function refused it, so its 24 raw bytes were
  read instead (no fabricated pseudocode).
- The stub drops the protocol instance and branches to `0x29fc`, the surrounding
  SIP implementation: raw instruction sequence forwards w1,w2,x3,x4, zeroes
  x4, then branches. Evidence
  `ev_ea252a25aa7061d71947a0aea56d986c4e31e45d2032cb058ddf573803b62156`.
  Target boundary remains partly unresolved; no claim that all IPC paths are
  completely recovered.
- `0x29fc` makes a shared allocation for more than four args, then calls UEFI
  RaiseTPL/RestoreTPL through saved Boot Services (`+0x18/+0x20`). It ORs
  SMC64 bit30 into the requested function ID and invokes `0x373c`.
  Evidence `ev_70cecf46533a8f431bd7cc9ca1e165f2f80485920b657f8914c951d3cb327d31`.
- Assembly at `0x373c`: load argument registers, `smc #0`, repeat while signed
  w0 is positive, and return x1/x2/x3/x6 separately. The decompiler's apparent
  constant input condition is NOT faithful to the inline SMC register effects;
  assembly is the evidence for this loop. No timeout is visible in this loop.
  Evidence `ev_093d8801fcd89f58c7e2f323348f51b0d47ca1889656cb4713abc69c1e192d71`.
- `0x2c30` handles returned listener requests (`0xee02`) by invoking a stored
  callback and continuing with `0x32000203`. Exit handler candidate `0x3474`
  unregisters listeners and calls a located protocol depending on SecurityFlag.
  Evidence `ev_94ad267319a34ba5d0b5038e96bfb0ec76c4ad962af3d68c4539d1caedf042cc`
  and `ev_e0837c7ac4e6ea6b479d78cc15ab5c5b8d57c977f9057bf651af427a9c44b746`.

## Consequences and unknowns

This is concrete evidence of an ARMv8 SCM route in the device artifact, **not**
proof that XNU may invoke a relocated DXE function after ExitBootServices. Its
Boot Services, allocator and listener dependencies need lifecycle replacement.
The raw `smc` instruction does not prove PAS13 availability, valid crypto
bandwidth ownership, XPU-safe memory, authentication, or a bounded firmware
return. An OS timer cannot preempt a secure call that never returns.

Available Linux source (`drivers/soc/qcom/scm.c`) separately documents convention
discovery, SMC32/64, transport vs secure returns, and buffer cache maintenance.
Source knowledge is useful for a new narrow native transport, but neither the
source nor this static investigation is a runtime validation of that transport.
`uefipil.cfg` lists ABL/ImageFv/SPSS in the retail allowlist, not A640_ZAP;
do not add an allowlist entry to manufacture a trusted load or use `no_auth`.

The single user-authorized fastboot opportunity remains **unused**. Need real
SCM/PAS admission, resources and safe stop before considering a GPU-execution
candidate. GPU command retirement is a separate check from Metal rendering.
