//
// Created by Varsha Natarajan on 4/16/24.
//

#ifndef RDMA_WITH_PY_COMMON_H
#define RDMA_WITH_PY_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define DEFAULT_RDMA_PORT (12345)
#define MAX_CONNECTION (5)
#define ENABLE_ERROR
#define ENABLE_DEBUG

#define CQ_CAPACITY (16)
#define MAX_SGE (2)
#define MAX_WR (10)
#define TIMEOUTMS (2000)

#define HANDLE(x)  do { if (!(x)) error(#x " failed (returned zero/null).\n"); } while (0)
#define HANDLE_NZ(x) do { if ( (x)) error(#x " failed (returned non-zero)." ); } while (0)

#ifdef ENABLE_ERROR
#define error(msg, args...) do {\
        fprintf(stderr, "%s : %d : ERROR : "msg, __FILE__, __LINE__, ## args);\
    }while(0);
#else
#define error(msg, args...)
#endif

#ifdef ENABLE_DEBUG
#define debug(msg, args...) do {\
    printf("DEBUG: "msg, ## args);\
}while(0);

#else
#define debug(msg, args...)
#endif

#define info(msg, args...) do {\
    fprintf(stdout, "log: "msg, ## args);\
}while(0);

#define DATA_SIZE (1024 * 1024 * 5)
#define BLOCK_SIZE (1024 * 1024)

struct exchange_buffer {
    struct msg* message;
    struct ibv_mr* buffer;
};

struct msg {
    enum {
        HELLO,
        FRAME
    } type;

    union {
        struct ibv_mr mr;
        unsigned long offset;
    } data;
};

int get_addr(char *dst, struct sockaddr *addr);
void show_exchange_buffer(struct msg *attr);
void rdma_buffer_free(struct ibv_mr *mr);
void print_memory_map(const char* memory_region);
//int process_work_completion_events_without_wait(struct ibv_cq *cq_ptr, struct ibv_wc *wc, int max_wc);
struct ibv_mr *rdma_buffer_register(struct ibv_pd *pd,
                                    void *addr,
                                    uint32_t length,
                                    enum ibv_access_flags permission);
void rdma_buffer_deregister(struct ibv_mr *mr);

int process_work_completion_events(struct ibv_comp_channel *comp_channel,
                                   struct ibv_wc *wc,
                                   int max_wc);

#endif //RDMA_WITH_PY_COMMON_H
