#!/usr/bin/env python3
"""Check a flash backup against the chip without reading it all again: the
esptool stub computes MD5 sums of the flash on the chip, 1 MB at a time, and
they are compared with MD5 sums of the local file. Very little goes over the
serial link, so it takes under a minute.

Read-only. The player must be in bootloader mode.

Usage: uvx --from esptool python tools/verify_backup.py <backup.bin> <port>
"""
import hashlib
import sys
from pathlib import Path

import esptool

if len(sys.argv) != 3:
    sys.exit(__doc__)
DUMP = Path(sys.argv[1])
PORT = sys.argv[2]
CHUNK = 0x100000

data = DUMP.read_bytes()
esp = esptool.ESP32ROM(PORT, 115200)
esp.connect()
esp = esp.STUB_CLASS(esp) if esp.sync_stub_detected else esp.run_stub()
esp.flash_spi_attach(0)
esp.flash_set_parameters(len(data))

bad = 0
for off in range(0, len(data), CHUNK):
    local = hashlib.md5(data[off:off + CHUNK]).hexdigest()
    for attempt in range(3):
        try:
            chip = esp.flash_md5sum(off, CHUNK)
            break
        except esptool.FatalError as e:
            chip = f"error: {e}"
    ok = chip == local
    bad += not ok
    print(f"{off:#08x}-{off + CHUNK:#08x}  chip={chip}  file={local}  {'OK' if ok else 'MISMATCH'}")
print("BACKUP MATCHES CHIP" if not bad else f"{bad} region(s) differ")
sys.exit(1 if bad else 0)
