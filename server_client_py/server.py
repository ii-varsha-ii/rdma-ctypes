import sys

from ctypes import c_ushort, c_int, c_uint, c_ulong, c_size_t, c_void_p, string_at, c_char, c_long, cast
from ctypes import CDLL, POINTER, Structure, pointer, create_string_buffer, memmove

lib = CDLL(f"./rdma_server_lib_{sys.platform}.so")
class RDMAServer:
    def __init__(self):
        pass

    def listen(self):
        pass
