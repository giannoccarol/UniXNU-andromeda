#!/usr/bin/env python3
"""On-screen diagnostics for DarwinBoot failures (andromeda bring-up).

When DarwinBoot refuses to boot, UniXNU redraws its menu at once and only says
"'<entry>' returned: <status>": what failed, and why, is lost. This patch:

  * records the boot step in progress (kernel read/parse, ADT, trust cache,
    video, window, layout) and, on failure, draws "FAIL <status> AT <step>";
  * when no kernel window fits, draws the UEFI memory map between 0xB0000000
    and 0x210000000, one line per descriptor of at least 1 MiB (allocating at
    a fixed address cannot span two descriptors, so their boundaries matter):
    T7 = conventional, T4 = boot services data, T0 = reserved ...;
  * then stalls 90 s so the screen can be photographed.

Idempotent; applied after patches/darwinboot-cepheus.patch.
usage: patch-darwinboot-diag.py <DarwinBootPkg/Driver/DarwinBoot.c>
"""
import sys
from pathlib import Path

MARK = "/* andromeda diag */"
path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
if MARK in text:
    print(f"darwinboot diag: already in {path.name}")
    sys.exit(0)


def replace(old, new, count=1):
    global text
    if text.count(old) != count:
        sys.exit(f"darwinboot diag: expected {count}x {old.strip()[:60]!r}, found {text.count(old)}")
    text = text.replace(old, new)


# 1. The step in progress, as a global the failure path can print.
replace("#include \"DiagFont.h\"\n",
        "#include \"DiagFont.h\"\n" + MARK + "\nSTATIC CONST CHAR16 *mStep = L\"start\";\n")

# 2. The window failure: a merged memory map instead of the first free runs.
old_window = """    UnicodeSPrint (Line, sizeof (Line), L"NO WINDOW %lx BIG RUN %lx @ %lx", WindowSize, BestBytes, BestStart);
    DiagDraw (Line, 0);
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->NumberOfPages >= 0x400) && (Row < 8)) {
        UnicodeSPrint (Line, sizeof (Line), L"FREE %lx + %lx", Descriptor->PhysicalStart, EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
        DiagDraw (Line, Row++);
      }
    }
"""
new_window = """    UINT32 Small = 0;
    UINT64 DStart;
    UINT64 DEnd;
    UnicodeSPrint (Line, sizeof (Line), L"NO WINDOW %lx RUN %lx@%lx", WindowSize, BestBytes, BestStart);
    DiagDraw (Line, 0);
    UnicodeSPrint (Line, sizeof (Line), L"KOFF %lx T7=FREE T4=BSDATA T0=RSV", Kernel->Base & (KERNEL_BLOCK_SIZE - 1));
    DiagDraw (Line, 1);
    Row = 2;
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      DStart = Descriptor->PhysicalStart;
      DEnd = DStart + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);
      if ((DEnd <= 0xB0000000ULL) || (DStart >= 0x210000000ULL)) { continue; }
      // One line per descriptor (>= 1 MiB): AllocateAddress cannot span two
      // map entries, so the entry boundaries are what matters here.
      if (DEnd - DStart < SIZE_1MB) { Small++; continue; }
      if (Row < 29) {
        UnicodeSPrint (Line, sizeof (Line), L"T%u %lx-%lx", Descriptor->Type, DStart, DEnd);
        DiagDraw (Line, Row++);
      }
    }
    if (Small != 0) {
      UnicodeSPrint (Line, sizeof (Line), L"+%u SMALL ENTRIES", Small);
      DiagDraw (Line, Row++);
    }
"""
replace(old_window, new_window)

# 3. Name each step before it runs.
steps = [
    ("  Status = ReadFile (Root, Request->KernelPath, SIZE_128MB, &Image, &ImageSize);\n", "kernel read"),
    ("  Status = ParseKernel (Image, ImageSize, &Kernel);\n", "kernel parse"),
    ("  TreePath = AllocatePool (Length + 11 * sizeof (CHAR16));\n", "adt path"),
    ("  Status = ReadFile (Root, TreePath, SIZE_16MB, &Tree, &TreeSize);\n", "adt read"),
    ("  if (!AdtNode (Tree, TreeSize, &Offset, 0, 3,", "adt parse"),
    ("  Status = ReadFile (Root, TreePath, SIZE_16MB, &TrustCaches, &TrustCacheSize);\n", "trustcache"),
    ("  Status = ReadBootVideo (Video);\n", "video"),
    ("  Status = AllocateWindow (&Kernel, WindowSize, &Address);\n", "window"),
    ("  ArgsAddress = ALIGN_VALUE (Address + Kernel.End - Kernel.Base, SIZE_16KB);\n", "layout"),
]
for anchor, name in steps:
    replace(anchor, f"  mStep = L\"{name}\";\n" + anchor)

# 4. The failure path: say where it failed, then hold the screen.
replace("""  Print (L"DARWINBOOT: boot refused: %r; firmware retained\\n", Status);
""", """  {
    CHAR16 FailLine[96];
    UnicodeSPrint (FailLine, sizeof (FailLine), L"FAIL %r AT %s", Status, mStep);
    DiagDraw (FailLine, 30);
    gBS->Stall (90 * 1000 * 1000);
  }
  Print (L"DARWINBOOT: boot refused: %r; firmware retained\\n", Status);
""")

path.write_text(text, encoding="utf-8")
print(f"darwinboot diag: patched {path}")
