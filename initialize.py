import ctypes
import socket
import sys

UNIX_PATH_MAX = 108
PF_UNIX = socket.AF_UNIX
PF_INET = socket.AF_INET

server_libc = ctypes.CDLL('libs/librdma_server_lib.so')
client_libc = ctypes.CDLL('libs/librdma_client_lib.so')


def SUN_LEN(path):
    """For AF_UNIX the addrlen is *not* sizeof(struct sockaddr_un)"""
    return ctypes.c_int(2 + len(path))


class sockaddr_un(ctypes.Structure):
    _fields_ = [("sa_family", ctypes.c_ushort),  # sun_family
                ("sun_path", ctypes.c_char * UNIX_PATH_MAX)]


class sockaddr_in(ctypes.Structure):
    _fields_ = [("sa_family", ctypes.c_ushort),  # sin_family
                ("sin_port", ctypes.c_ushort),
                ("sin_addr", ctypes.c_byte * 4),
                ("__pad", ctypes.c_byte * 8)]


server_libc.start_rdma_server.argtypes = [ctypes.POINTER(sockaddr_in)]
server_libc.start_rdma_server.restype = ctypes.c_char_p
client_libc.connect_server.argtypes = [ctypes.POINTER(sockaddr_in), ctypes.c_char_p]


def to_sockaddr(family, address, port):
    if family == socket.AF_INET:
        addr = sockaddr_in()
        addr.sa_family = ctypes.c_ushort(family)
        if port:
            addr.sin_port = ctypes.c_ushort(socket.htons(port))
        if address:
            bytes_ = [int(i) for i in address.split('.')]
            addr.sin_addr = (ctypes.c_byte * 4)(*bytes_)
        addr_len = ctypes.c_int(ctypes.sizeof(addr))
    else:
        raise NotImplementedError('Not implemented family %s' % (family,))

    return addr


def server_listen(sockaddr):
    received_frame = server_libc.start_rdma_server(sockaddr)
    print("Received frame: ", received_frame.decode())


def start_client(sockaddr, str_to_send):
    buf = ctypes.create_string_buffer(str_to_send.encode(), len(str_to_send))
    client_libc.connect_server(sockaddr, buf)
