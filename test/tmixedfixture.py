"""Smoke-test the fictional NIC/HID descriptor and native HID reset fixture.

This establishes fixture behavior, not Windows mixed-owner reset coverage.
Use the QEMU build containing usb-rtl8153-hid-test.
"""
import re
from qharn import Harness

h = Harness(['-device', 'usb-ehci,id=hc', '-netdev', 'user,id=n0',
             '-device', 'usb-rtl8153-hid-test,id=mixed,bus=hc.0,netdev=n0'])
def show(lines):
    for line in lines:
        print(line, flush=True)
    return lines
def key(down):
    h.qmp_cmd('qom-set', path='/machine/peripheral/mixed',
              property='test-key-down', value=down)
    lines = show(h.collect(.5))
    assert any('KEY 30 ' + ('down' if down else 'up') in line for line in lines), lines
    assert h.qmp_cmd('qom-get', path='/machine/peripheral/mixed',
                     property='test-key-seen') is True
try:
    show(h.wait('harness: ready', 90)); h.wait('READY')
    lines = show(h.cmd('ls'))
    matches = [re.search(r'DEV (\d+) 0bda:8153', line) for line in lines]
    matches = [m for m in matches if m]
    assert len(matches) == 1, lines
    index = int(matches[0][1])
    for cycle in range(5):
        key(True); key(False)
        lines = show(h.cmd('hidreset ' + str(index)))
        assert 'HIDRESET index=%d result=0' % index in lines, lines
        key(True); key(False)
    h.qmp_cmd('device_del', id='mixed')
    show(h.collect(2))
    assert sum('physical reset' in line for line in h.qemu_lines('usbnt-mixed-test:')) >= 6
    print('RESULT mixed-fixture PASS: five physical HID resets and twenty guest-observed key transitions; Windows owner not exercised')
finally:
    h.close()
