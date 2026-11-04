#!/usr/bin/env python3
"""Host check of the kernel H4/IBS parser (renoir_bluetooth_hci.inc bt_parse) and the SE7 frequency match."""
import subprocess,sys,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
src=(here.parent/'kernel/files/bsd/dev/renoir_bluetooth_hci.inc').read_text()
with tempfile.TemporaryDirectory() as d:
    d=Path(d)
    hexf=src[src.index('static void bt_hex('):src.index('/* UART layer')]
    (d/'parser.inc').write_text(hexf+src[src.index('#define HCI_IBS_SLEEP_IND'):src.index('static void bt_rx_process(void)')])
    (d/'t.c').write_text((here/'bluetooth_h4_parser.c').read_text())
    subprocess.run(['cc','-Wall','-Wno-unused-function','-Wno-format','-o',str(d/'t'),str(d/'t.c')],check=True)
    sys.exit(subprocess.run([str(d/'t')]).returncode)
