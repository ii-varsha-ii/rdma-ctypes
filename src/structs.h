//
// Created by nvarsha on 4/20/24.
//

#ifndef RDMA_WITH_PY_STRUCTS_H
#define RDMA_WITH_PY_STRUCTS_H

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

static struct rdma_cm_id *cm_server_id = NULL;
static struct rdma_cm_id *cm_client_id = NULL;
static struct exchange_buffer server_buff, client_buff;
static struct client_resources *client_res = NULL;
static struct rdma_event_channel *cm_event_channel = NULL;
static struct ibv_qp_init_attr qp_init_attr; // client queue pair attributes

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

struct client_resources {
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_channel;
    struct ibv_qp *qp;
    struct rdma_cm_id *id;
};

struct memory_region {
    struct ibv_mr server_mr;
    char *memory_region;
    struct ibv_mr *memory_region_mr;
    unsigned long *mapping_table_start;
};


#endif //RDMA_WITH_PY_STRUCTS_H
