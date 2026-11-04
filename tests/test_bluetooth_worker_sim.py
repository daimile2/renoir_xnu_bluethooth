#!/usr/bin/env python3
"""Host model of the kernel Bluetooth worker loop using the production functions of renoir_bluetooth_hci.inc."""
import subprocess,sys,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
src=(here.parent/'kernel/files/bsd/dev/renoir_bluetooth_hci.inc').read_text()
def cut(a,b):return src[src.index(a):src.index(b)]
with tempfile.TemporaryDirectory() as d:
    d=Path(d)
    (d/'worker_bu.inc').write_text(cut('#define BU_RXQ','#define POLL_WAIT_TIMEOUT_MSEC')+cut('static struct {\n    vm_offset_t base;','static void bu_rxq_put_locked'))
    (d/'worker.inc').write_text(cut('static void bt_hex(','/* UART layer')+cut('#define HCI_IBS_SLEEP_IND','/* qca_get_baudrate_value */'))
    (d/'t.c').write_text((here/'bluetooth_worker_sim.c').read_text())
    subprocess.run(['cc','-Wall','-Wno-unused-function','-Wno-format','-o',str(d/'t'),str(d/'t.c')],check=True)
    sys.exit(subprocess.run([str(d/'t')]).returncode)
