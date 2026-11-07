#!/usr/bin/env python3
"""Make DarwinBoot's boot_args right for the XNU 8796 (iOS 16) kernel.

DarwinBoot fills the Darwin 23 boot_args (BootArgs.h): CommandLine[1024],
then BootFlags at 0x470 and MemorySizeActual at 0x478. XNU 8796 still has
BOOT_LINE_LENGTH = 608 (pexpert/pexpert/arm64/boot.h): it reads 608 bytes
of command line and takes bootFlags from 0x2d0 and memSizeActual from 0x2d8,
which DarwinBoot fills with characters 612-627 of the command line. On
andromeda a 721-character line turned into hw.memsize, HFS sized its journal
buffer from it and panicked at the first transaction (ANDROMEDA.md).

This patch:
  * refuses a command line longer than 607 characters (one NUL must fit in
    608), with a message on screen, instead of handing the kernel a
    truncated line and garbage memory sizes;
  * also writes BootFlags and MemorySizeActual at the Darwin 22 offsets.
    For a Darwin 23 kernel those bytes are inside its 1024-byte CommandLine,
    after the terminating NUL of a line that is at most 607 characters, so
    they are never read there.

Idempotent; applied after patches/darwinboot-cepheus.patch and
scripts/patch-darwinboot-diag.py.
usage: patch-darwinboot-bootargs.py <DarwinBootPkg/Driver/DarwinBoot.c>
"""
import sys
from pathlib import Path

MARK = "/* andromeda boot_args xnu8796 */"
path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
if MARK in text:
    print(f"darwinboot boot_args: already in {path.name}")
    sys.exit(0)


def replace(old, new):
    global text
    if text.count(old) != 1:
        raise SystemExit(f"darwinboot boot_args: expected one match in {path.name} for:\n{old}")
    text = text.replace(old, new)


replace(
    "#define MAX_SEGMENTS 64\n",
    "#define MAX_SEGMENTS 64\n"
    f"{MARK}\n"
    "//\n"
    "// XNU 8796 (Darwin 22, iOS 16) boot_args: BOOT_LINE_LENGTH 608, so the\n"
    "// command line ends at 0x2cc and bootFlags/memSizeActual sit at 0x2d0/0x2d8.\n"
    "// Darwin 23 moved them behind a 1024-byte line (BootArgs.h).\n"
    "//\n"
    "#define DARWIN22_BOOT_LINE_LENGTH     608\n"
    "#define DARWIN22_BOOT_FLAGS_OFFSET    0x2d0\n"
    "#define DARWIN22_MEMORY_ACTUAL_OFFSET 0x2d8\n",
)
replace(
    "      if ((Index >= sizeof (CommandLine) - 1) || (Request->BootArgs[Index] > 0x7f)) { return EFI_BAD_BUFFER_SIZE; }\n",
    "      if (Index >= DARWIN22_BOOT_LINE_LENGTH - 1) {\n"
    "        Print (L\"DARWINBOOT: boot-args longer than %u characters (XNU 8796 BOOT_LINE_LENGTH); firmware retained\\n\",\n"
    "               (UINT32)(DARWIN22_BOOT_LINE_LENGTH - 1));\n"
    "        return EFI_BAD_BUFFER_SIZE;\n"
    "      }\n"
    "      if (Request->BootArgs[Index] > 0x7f) { return EFI_BAD_BUFFER_SIZE; }\n",
)
replace(
    "  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));\n",
    "  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));\n"
    "  // Same values where XNU 8796 reads them (zeros of CommandLine otherwise).\n"
    "  WriteUnaligned64 ((UINT64 *)((UINT8 *)Args + DARWIN22_BOOT_FLAGS_OFFSET), Args->BootFlags);\n"
    "  WriteUnaligned64 ((UINT64 *)((UINT8 *)Args + DARWIN22_MEMORY_ACTUAL_OFFSET), Args->MemorySizeActual);\n",
)
path.write_text(text, encoding="utf-8")
print(f"darwinboot boot_args: patched {path.name} for XNU 8796")
