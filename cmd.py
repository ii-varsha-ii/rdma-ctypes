import socket
import sys

from initialize import PF_INET, to_sockaddr, server_listen, start_client

if __name__ == '__main__':
    # python server.py -s 10.10.1.2 -p 12345
    # server
    if sys.argv[1] == '-l':
        sock = socket.socket(PF_INET, socket.SOCK_DGRAM)
        af = socket.AF_INET
        bind_addr = sys.argv[2] or "0.0.0.0"
        if len(sys.argv) == 5 and sys.argv[3] == '-p':
            port = int(sys.argv[4])
        else:
            port = 12345
    else:  # python client.py -c 10.10.1.2 -p 12345
        sock = socket.socket(PF_INET, socket.SOCK_DGRAM)
        af = socket.AF_INET
        bind_addr = sys.argv[2] or "0.0.0.0"
        if len(sys.argv) == 5 and sys.argv[3] == '-p':
            port = int(sys.argv[4])
        else:
            port = 12345

    sockaddr = to_sockaddr(af, bind_addr, port)
    if sys.argv[1] == '-l':
        server_listen(sockaddr)
    else:
        start_client(sockaddr, "Helloworld")
