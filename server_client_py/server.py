import sys, os

import ctypes
import os
import socket
import sys

libc = ctypes.CDLL('libc.so.6')


def SUN_LEN(path):
    """For AF_UNIX the addrlen is *not* sizeof(struct sockaddr_un)"""
    return ctypes.c_int(2 + len(path))


UNIX_PATH_MAX = 108
PF_UNIX = socket.AF_UNIX
PF_INET = socket.AF_INET


class sockaddr_un(ctypes.Structure):
    _fields_ = [("sa_family", ctypes.c_ushort),  # sun_family
                ("sun_path", ctypes.c_char * UNIX_PATH_MAX)]


class sockaddr_in(ctypes.Structure):
    _fields_ = [("sa_family", ctypes.c_ushort),  # sin_family
                ("sin_port", ctypes.c_ushort),
                ("sin_addr", ctypes.c_byte * 4),
                ("__pad", ctypes.c_byte * 8)]


def to_sockaddr(family, address=None):
    if family == socket.AF_UNIX:
        addr = sockaddr_un()
        addr.sa_family = ctypes.c_ushort(family)
        if address:
            addr.sun_path = address
            addrlen = SUN_LEN(address)
        else:
            addrlen = ctypes.c_int(ctypes.sizeof(addr))

    elif family == socket.AF_INET:
        addr = sockaddr_in()
        addr.sa_family = ctypes.c_ushort(family)
        if address:
            addr.sin_port = ctypes.c_ushort(socket.htons(address[1]))
            bytes_ = [int(i) for i in address[0].split('.')]
            addr.sin_addr = (ctypes.c_byte * 4)(*bytes_)
        addrlen = ctypes.c_int(ctypes.sizeof(addr))

    else:
        raise NotImplementedError('Not implemented family %s' % (family,))

    return addr, addrlen


# ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
#                const struct sockaddr *dest_addr, socklen_t addrlen);

def sendto(sockfd, data, flags, family, address):

    buf = ctypes.create_string_buffer(data)
    dest_addr, addrlen = to_sockaddr(family, address)
    ret = libc.sendto(sockfd, buf, len(data), flags,
                      ctypes.byref(dest_addr), addrlen)
    return ret


# ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
#                  struct sockaddr *src_addr, socklen_t *addrlen);

def recvfrom(sockfd, length, flags, family):
    buf = ctypes.create_string_buffer(b'', length)  # no need to zero it
    src_addr, addrlen = to_sockaddr(family)
    ret = libc.recvfrom(sockfd, buf, length, flags,
                        ctypes.byref(src_addr), ctypes.byref(addrlen))
    assert ret == len(buf.value)
    return buf.value, from_sockaddr(src_addr)


def from_sockaddr(sockaddr):
    if sockaddr.sa_family == socket.AF_UNIX:
        return sockaddr.sun_path
    elif sockaddr.sa_family == socket.AF_INET:
        return ('%d.%d.%d.%d' % tuple(sockaddr.sin_addr),
                socket.ntohs(sockaddr.sin_port))
    raise NotImplementedError('Not implemented family %s' %
                              (sockaddr.sa_family,))


def echo_server(sock, af, bindaddr):
    sock.bind(bindaddr)
    try:
        while True:
            data, addr = recvfrom(sock.fileno(), 4096, 0, af)
            str_data = data.decode()
            mangled = ''.join(reversed([i for i in str_data]))
            print('Got %r from %r, sending response %r' % (data, addr, mangled))
            print(mangled)
            sendto(sock.fileno(), mangled.encode(), 0, af, addr)
    finally:
        if af == socket.AF_UNIX:
            os.unlink(bindaddr)
        sock.close()


def send_server(sock, af, bindaddr, addr):
    sock.bind(bindaddr)
    try:
        while True:
            try:
                data = input(">> ").encode()
            except EOFError:
                sys.stdout.write('\r')
                break

            sendto(sock.fileno(), data, 0, af, addr)
            data, addr = recvfrom(sock.fileno(), 4096, 0, af)
            print('Got %r from %r' % (data, addr))
    finally:
        if af == socket.AF_UNIX:
            os.unlink(bindaddr)
        sock.close()


if __name__ == '__main__':
    if len(sys.argv) in (3, 4) and sys.argv[1] == '-U':
        print(sys.argv)
        sock = socket.socket(PF_UNIX, socket.SOCK_DGRAM)
        af = socket.AF_UNIX
        bindaddr = sys.argv[2]
        addr = (len(sys.argv) == 4) and sys.argv[3] or None

    elif len(sys.argv) in (3, 4) and sys.argv[1] == '-I':
        print(sys.argv)
        sock = socket.socket(PF_INET, socket.SOCK_DGRAM)
        af = socket.AF_INET
        if len(sys.argv) == 3:
            bindaddr = ('0.0.0.0', int(sys.argv[2])) # server
            addr = None
        else:
            bindaddr = ('0.0.0.0', 0)
            addr = (sys.argv[2], int(sys.argv[3])) # client

    else:
        print('Usage:')
        print('  python ctypes-dgram.py -U ./echosock')
        print('  echo hello unix socket |')
        print('    python ctypes-dgram.py -U ./mysock ./echosock')
        print('or:')
        print('  python ctypes-dgram.py -I 1234  # echoport')
        print('  echo hello internet |')
        print('    python ctypes-dgram.py -I 127.0.0.1 1234')
        sys.exit(1)

    if addr:
        send_server(sock, af, bindaddr, addr.encode()) # client
    else:
        echo_server(sock, af, bindaddr) # server
