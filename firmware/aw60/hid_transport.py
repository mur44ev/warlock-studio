"""Windows HIDAPI transport. No USB access occurs until explicitly instantiated."""
import ctypes as c
import os
import time
from pathlib import Path

class Info(c.Structure):
    pass

Info._fields_ = [('path', c.c_char_p), ('vid', c.c_ushort), ('pid', c.c_ushort),
    ('serial', c.c_wchar_p), ('release', c.c_ushort), ('manufacturer', c.c_wchar_p),
    ('product', c.c_wchar_p), ('page', c.c_ushort), ('usage', c.c_ushort),
    ('interface', c.c_int), ('next', c.POINTER(Info))]

def library():
    root = Path(__file__).resolve().parents[2]
    candidates = [Path(os.environ['WARLOCK_HIDAPI'])] if os.environ.get('WARLOCK_HIDAPI') else []
    candidates += [root/'hidapi.dll', root/'dist/studio/hidapi.dll']
    dll = next((p for p in candidates if p.is_file()), None)
    if dll is None:
        raise FileNotFoundError('hidapi.dll missing: extract the firmware release archive, build Studio, or set WARLOCK_HIDAPI')
    h = c.CDLL(str(dll))
    h.hid_enumerate.argtypes = [c.c_ushort, c.c_ushort]; h.hid_enumerate.restype = c.POINTER(Info)
    h.hid_free_enumeration.argtypes = [c.POINTER(Info)]
    h.hid_open_path.argtypes = [c.c_char_p]; h.hid_open_path.restype = c.c_void_p
    h.hid_close.argtypes = [c.c_void_p]
    for name in ('hid_send_feature_report', 'hid_get_feature_report'):
        getattr(h, name).argtypes = [c.c_void_p, c.c_void_p, c.c_size_t]
    h.hid_error.argtypes = [c.c_void_p]; h.hid_error.restype = c.c_wchar_p
    return h

def enumerate_hid(h):
    head = h.hid_enumerate(0x3151, 0); p = head; result = []
    try:
        while p:
            d = p.contents
            result.append(dict(path=d.path.decode(), vid=d.vid, pid=d.pid,
                page=d.page, usage=d.usage, interface=d.interface))
            p = d.next
    finally:
        h.hid_free_enumeration(head)
    return result

def raw_send(h, dev, payload):
    if len(payload) > 64:
        raise ValueError('Oversize report')
    buf = c.create_string_buffer(b'\0'+bytes(payload).ljust(64,b'\0'), 65)
    if h.hid_send_feature_report(dev, buf, 65) != 65:
        raise OSError(h.hid_error(dev))

def raw_get(h, dev):
    buf = c.create_string_buffer(65)
    n = h.hid_get_feature_report(dev, buf, 65)
    if n != 65:
        raise OSError('Short HID response: '+str(n))
    return buf.raw[1:n]

def transact(h, dev, payload):
    p = bytearray(bytes(payload).ljust(64, b'\0'))
    p[7] = (255-sum(p[:7])) & 255
    raw_send(h, dev, p); time.sleep(.15)
    return raw_get(h, dev)
