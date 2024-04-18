#include "common.h"

static struct rdma_event_channel *cm_event_channel = NULL;
static struct rdma_cm_id *cm_client_id = NULL;
static struct ibv_qp_init_attr qp_init_attr; // client queue pair attributes

static struct ibv_send_wr client_send_wr, *bad_client_send_wr = NULL;
static struct ibv_recv_wr server_recv_wr, *bad_server_recv_wr = NULL;
static struct ibv_sge client_send_sge, server_recv_sge;

static struct exchange_buffer server_buff, client_buff;
static struct per_client_resources *client_res = NULL;

// connection struct
struct memory_region {
    char *local_memory_region;
    struct ibv_mr server_mr;
    struct ibv_mr *local_memory_region_mr;
};

// client resources struct
struct per_client_resources {
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *completion_channel;
    struct ibv_qp *qp;
    struct rdma_cm_id *client_id;
};

/*
 * Create client ID and resolve the destination IP address to RDMA Address
 */
static void resolve_addr(struct sockaddr_in *s_addr) {
    client_res = (struct per_client_resources *) malloc(sizeof(struct per_client_resources));

    /* Init Event Channel */
    HANDLE(cm_event_channel = rdma_create_event_channel());
    debug("RDMA CM event channel created: %p \n", cm_event_channel)

    /* Create Client ID with the above Event Channel */
    HANDLE_NZ(rdma_create_id(cm_event_channel, &cm_client_id,
                             NULL,
                             RDMA_PS_TCP));
    client_res->client_id = cm_client_id;

    /* Resolve IP address to RDMA address and bind to client_id */
    HANDLE_NZ(rdma_resolve_addr(client_res->client_id, NULL, (struct sockaddr *) s_addr, TIMEOUTMS));
    debug("waiting for cm event: RDMA_CM_EVENT_ADDR_RESOLVED\n")
}

static int setup_client_resources(struct sockaddr_in *s_addr) {
    info("Trying to connect to server at : %s port: %d \n",
         inet_ntoa(s_addr->sin_addr),
         ntohs(s_addr->sin_port));

    // Init Protection Domain
    HANDLE(client_res->pd = ibv_alloc_pd(client_res->client_id->verbs));
    debug("Protection Domain (PD) allocated: %p \n", client_res->pd)

    // Init Completion Channel
    HANDLE(client_res->completion_channel = ibv_create_comp_channel(cm_client_id->verbs));
    debug("Completion channel created: %p \n", client_res->completion_channel)

    // Init Completion Queue
    HANDLE(client_res->cq = ibv_create_cq(client_res->client_id->verbs /* which device*/,
                                          CQ_CAPACITY /* maximum capacity*/,
                                          NULL /* user context, not used here */,
                                          client_res->completion_channel /* which IO completion channel */,
                                          0 /* signaling vector, not used here*/));
    debug("CQ created: %p with %d elements \n", client_res->cq, client_res->cq->cqe)

    // Receive notifications from complete queue pair
    HANDLE_NZ(ibv_req_notify_cq(client_res->cq, 0));

    bzero(&qp_init_attr, sizeof qp_init_attr);
    qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
    qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
    qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
    qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
    qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */

    /* We use same completion queue, but one can use different queues */
    qp_init_attr.recv_cq = client_res->cq;
    qp_init_attr.send_cq = client_res->cq;

    HANDLE_NZ(rdma_create_qp(client_res->client_id,
                             client_res->pd,
                             &qp_init_attr));

    client_res->qp = cm_client_id->qp;
    debug("Client QP created: %p \n", client_res->qp)
    return 0;
}

/*
 * Create a server memory buffer for a message to receive the memory map.
 * Post it as the Receive Request (RR) to the Queue.
 */
static int post_recv_server_memory_map() {
    debug("Register Server Buffer\n");
    server_buff.message = malloc(sizeof(struct msg));
    HANDLE(server_buff.buffer = rdma_buffer_register(client_res->pd,
                                                     server_buff.message,
                                                     sizeof(struct msg),
                                                     (IBV_ACCESS_LOCAL_WRITE)));

    server_recv_sge.addr = (uint64_t) server_buff.message;
    server_recv_sge.length = (uint32_t) sizeof(struct msg);
    server_recv_sge.lkey = server_buff.buffer->lkey;

    bzero(&server_recv_wr, sizeof(server_recv_wr));
    server_recv_wr.sg_list = &server_recv_sge;
    server_recv_wr.num_sge = 1;

    HANDLE_NZ(ibv_post_recv(client_res->qp /* which QP */,
                            &server_recv_wr /* receive work request*/,
                            &bad_server_recv_wr /* error WRs */));
    debug("Pre-posting Server Receive Buffer for Address successfull \n");
    return 0;
}

/* Send Connect Request to the server */
static void connect_to_server() {
    struct rdma_conn_param conn_param;
    bzero(&conn_param, sizeof(conn_param));
    conn_param.initiator_depth = 3;
    conn_param.responder_resources = 3;
    conn_param.retry_count = 3;
    HANDLE_NZ(rdma_connect(client_res->client_id, &conn_param));
}

/*
 * Create a client buffer for OFFSET and register a memory buffer
 */
static int post_hello_to_server(int offset) {
    debug("Register Client Buffer")
    client_buff.message = malloc(sizeof(struct msg));
    client_buff.message->type = HELLO;
    client_buff.message->data.offset = offset;

    HANDLE(client_buff.buffer = rdma_buffer_register(client_res->pd,
                                                     client_buff.message,
                                                     sizeof(struct msg),
                                                     (IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE)));

    debug(":: Exchange Buffer:: \n");
    show_exchange_buffer(client_buff.message);

    client_send_sge.addr = (uint64_t) client_buff.buffer->addr;
    client_send_sge.length = (uint32_t) client_buff.buffer->length;
    client_send_sge.lkey = client_buff.buffer->lkey;

    bzero(&client_send_wr, sizeof(client_send_wr));
    client_send_wr.sg_list = &client_send_sge;
    client_send_wr.num_sge = 1;
    client_send_wr.opcode = IBV_WR_SEND;
    client_send_wr.send_flags = IBV_SEND_SIGNALED;

    HANDLE_NZ(ibv_post_send(client_res->qp,
                            &client_send_wr,
                            &bad_client_send_wr));

    info("Pre-posting Send Request with OFFSET is successful \n");
    return 0;
}

static int post_recv_hello_from_server() {
    server_buff.message = malloc(sizeof(struct msg));
    HANDLE(server_buff.buffer = rdma_buffer_register(client_res->pd,
                                                     server_buff.message,
                                                     sizeof(struct msg),
                                                     (IBV_ACCESS_LOCAL_WRITE)));

    server_recv_sge.addr = (uint64_t) server_buff.message;
    server_recv_sge.length = (uint32_t) sizeof(struct msg);
    server_recv_sge.lkey = server_buff.buffer->lkey;

    bzero(&server_recv_wr, sizeof(server_recv_wr));
    server_recv_wr.sg_list = &server_recv_sge;
    server_recv_wr.num_sge = 1;

    HANDLE_NZ(ibv_post_recv(client_res->qp /* which QP */,
                            &server_recv_wr /* receive work request*/,
                            &bad_server_recv_wr /* error WRs */));
    debug("Pre-posting Server Receive Buffer for Address successfull \n");
    return 0;
}

static int disconnect_and_cleanup() {
    int ret = -1;
    ret = rdma_disconnect(client_res->client_id);
    if (ret) {
        error("Failed to disconnect, errno: %d \n", -errno);
    }

    /* Destroy QP */
    rdma_destroy_qp(client_res->client_id);

    /* Destroy client cm id */
    ret = rdma_destroy_id(client_res->client_id);
    if (ret) {
        error("Failed to destroy client id cleanly, %d \n", -errno);
    }
    /* Destroy CQ */
    ret = ibv_destroy_cq(client_res->cq);
    if (ret) {
        error("Failed to destroy completion queue cleanly, %d \n", -errno);
    }
    /* Destroy completion channel */
    ret = ibv_destroy_comp_channel(client_res->completion_channel);
    if (ret) {
        error("Failed to destroy completion channel cleanly, %d \n", -errno);
    }

    rdma_buffer_deregister(server_buff.buffer);
    rdma_buffer_deregister(client_buff.buffer);

    /* Destroy protection domain */
    ret = ibv_dealloc_pd(client_res->pd);
    if (ret) {
        error("Failed to destroy client protection domain cleanly, %d \n", -errno);

    }
    rdma_destroy_event_channel(cm_event_channel);
    printf("Client resource clean up is complete \n");
    return 0;
}

/*
 * Create a local memory region and set it as 0.
 * Register a memory buffer for the local memory region with the pd, memory address, length and permissions
 * */
static void build_memory_map(struct memory_region *conn) {
    debug("Registering Local Memory Region \n");
    conn->local_memory_region = malloc(DATA_SIZE + (8 * (DATA_SIZE / BLOCK_SIZE)));
    memset(conn->local_memory_region, 0, DATA_SIZE + (8 * (DATA_SIZE / BLOCK_SIZE)));

    conn->local_memory_region_mr = rdma_buffer_register(client_res->pd,
                                                        conn->local_memory_region,
                                                        DATA_SIZE + (8 * (DATA_SIZE / BLOCK_SIZE)),
                                                        (IBV_ACCESS_LOCAL_WRITE |
                                                         IBV_ACCESS_REMOTE_READ |
                                                         IBV_ACCESS_REMOTE_WRITE));

    debug("Local Memory Region registration successful: %p\n", conn->local_memory_region)
}

static void read_memory_map(struct memory_region *region) {
    memcpy(&region->server_mr, &server_buff.message->data.mr, sizeof(region->server_mr));

    client_send_sge.addr = (uintptr_t) (region->local_memory_region);
    client_send_sge.length = (uint32_t) DATA_SIZE + (8 * (DATA_SIZE / BLOCK_SIZE));
    client_send_sge.lkey = region->local_memory_region_mr->lkey;

    bzero(&client_send_wr, sizeof(client_send_wr));
    client_send_wr.sg_list = &client_send_sge;
    client_send_wr.num_sge = 1;
    client_send_wr.opcode = IBV_WR_RDMA_READ;
    client_send_wr.send_flags = IBV_SEND_SIGNALED;
    client_send_wr.wr.rdma.remote_addr = (uintptr_t) region->server_mr.addr;
    client_send_wr.wr.rdma.rkey = region->server_mr.rkey;
    HANDLE_NZ(ibv_post_send(client_res->qp,
                            &client_send_wr,
                            &bad_client_send_wr));

    debug("RDMA read the remote memory map. \n");
}

int process_work_completion_events_without_wait(struct ibv_wc *wc, int max_wc) {
    int total_wc, i;
    int ret = -1;
    ret = ibv_req_notify_cq(client_res->cq, 0);
    if (ret) {
        error("Failed to request further notifications %d \n", -errno);
        return -errno;
    }
    total_wc = 0;
    do {
        ret = ibv_poll_cq(client_res->cq /* the CQ, we got notification for */,
                          max_wc - total_wc /* number of remaining WC elements*/,
                          wc + total_wc/* where to store */);
        if (ret < 0) {
            error("Failed to poll cq for wc due to %d \n", ret);
            return ret;
        }
        total_wc += ret;
    } while (total_wc < max_wc);
    debug("%d WC are completed \n", total_wc)
    for (i = 0; i < total_wc; i++) {
        if (wc[i].status != IBV_WC_SUCCESS) {
            error("Work completion (WC) has error status: %s at index %d \n",
                  ibv_wc_status_str(wc[i].status), i);
            /* return negative value */
            return -(wc[i].status);
        }
    }
    ibv_ack_cq_events(client_res->cq, 1);
    return total_wc;
}

static void c_poll_for_completion_events(struct ibv_wc *wc, int num_wc) {
    int total_wc = process_work_completion_events_without_wait(wc, num_wc);

    for (int i = 0; i < total_wc; i++) {
        if (wc[i].opcode & IBV_WC_RECV) {
            if (server_buff.message->type == HELLO) {
                show_exchange_buffer(server_buff.message);
            }
        }
    }
    debug("done\n");
}

static void read_from_memory_map_in_offset(struct memory_region *conn, int offset) {
    memcpy(&conn->server_mr, &server_buff.message->data.mr, sizeof(conn->server_mr));

    client_send_sge.addr = (uintptr_t) (conn->local_memory_region + (8 * (DATA_SIZE / BLOCK_SIZE)) +
                                        (offset * BLOCK_SIZE));
    client_send_sge.length = (uint32_t) BLOCK_SIZE;
    client_send_sge.lkey = conn->local_memory_region_mr->lkey;

    bzero(&client_send_wr, sizeof(client_send_wr));
    client_send_wr.sg_list = &client_send_sge;
    client_send_wr.num_sge = 1;
    client_send_wr.opcode = IBV_WR_RDMA_READ;
    client_send_wr.send_flags = IBV_SEND_SIGNALED;
    client_send_wr.wr.rdma.remote_addr =
            (uintptr_t) conn->server_mr.addr + (8 * (DATA_SIZE / BLOCK_SIZE)) + (offset * BLOCK_SIZE);
    client_send_wr.wr.rdma.rkey = conn->server_mr.rkey;

    HANDLE_NZ(ibv_post_send(client_res->qp,
                            &client_send_wr,
                            &bad_client_send_wr));

    debug("RDMA read the remote memory map \n");
}

static void write_to_memory_map_in_offset(struct memory_region *conn, int offset, const char *string_to_write) {
    strcpy(conn->local_memory_region + (offset * BLOCK_SIZE) + (8 * (DATA_SIZE / BLOCK_SIZE)), string_to_write);

    client_send_sge.addr = (uintptr_t) (conn->local_memory_region + (8 * (DATA_SIZE / BLOCK_SIZE)) +
                                        (offset * BLOCK_SIZE));
    client_send_sge.length = (uint32_t) BLOCK_SIZE;
    client_send_sge.lkey = conn->local_memory_region_mr->lkey;

    bzero(&client_send_wr, sizeof(client_send_wr));
    client_send_wr.sg_list = &client_send_sge;
    client_send_wr.num_sge = 1;
    client_send_wr.opcode = IBV_WR_RDMA_WRITE;
    client_send_wr.send_flags = IBV_SEND_SIGNALED;

    client_send_wr.wr.rdma.rkey = conn->server_mr.rkey;
    client_send_wr.wr.rdma.remote_addr =
            (uintptr_t) conn->server_mr.addr + (8 * (DATA_SIZE / BLOCK_SIZE)) + (offset * BLOCK_SIZE);

    HANDLE_NZ(ibv_post_send(client_res->qp,
                            &client_send_wr,
                            &bad_client_send_wr));

    debug("RDMA write the remote memory map. \n");
}

/*
 * Blocking while loop which checks for incoming events and calls the necessary
 * functions based on the received events
 */
static int wait_for_event(struct sockaddr_in *s_addr) {

    struct rdma_cm_event *received_event = NULL;
    struct memory_region *_region = NULL;

    resolve_addr(s_addr);
    while (rdma_get_cm_event(cm_event_channel, &received_event) == 0) {
        struct ibv_wc wc;
        struct rdma_cm_event cm_event;
        memcpy(&cm_event, received_event, sizeof(*received_event));
        info("%s event received \n", rdma_event_str(cm_event.event));

        switch (cm_event.event) {
            /* RDMA Address Resolution completed successfully */
            case RDMA_CM_EVENT_ADDR_RESOLVED:
                HANDLE_NZ(rdma_ack_cm_event(received_event));
                /* Resolves the RDMA route to establish connection */
                rdma_resolve_route(client_res->client_id, TIMEOUTMS);
                break;

                /* RDMA Route established successfully */
            case RDMA_CM_EVENT_ROUTE_RESOLVED:
                HANDLE_NZ(rdma_ack_cm_event(received_event));
                setup_client_resources(s_addr);
                post_recv_hello_from_server();
                connect_to_server();
                break;

            case RDMA_CM_EVENT_ESTABLISHED:
                HANDLE_NZ(rdma_ack_cm_event(received_event));
                post_hello_to_server(123);
                /* poll for completion of the previous Receive Address and Send Offset */
                c_poll_for_completion_events(&wc, 2);
                post_hello_to_server(125);
                c_poll_for_completion_events(&wc, 1);
                break;
            case RDMA_CM_EVENT_DISCONNECTED:
                HANDLE_NZ(rdma_ack_cm_event(received_event));
                disconnect_and_cleanup();
                break;
            default:
                error("Event not found %s", (char *) cm_event.event);
                break;
        }
    }
}


int main(int argc, char **argv) {
    struct sockaddr_in server_sockaddr;
    int ret, option;

    bzero(&server_sockaddr, sizeof server_sockaddr);
    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    while ((option = getopt(argc, argv, "s:a:p:")) != -1) {
        switch (option) {
            case 'a':
                /* remember, this overwrites the port info */
                ret = get_addr(optarg, (struct sockaddr *) &server_sockaddr);
                if (ret) {
                    error("Invalid IP \n");
                    return ret;
                }
                break;
            case 'p':
                /* passed port to listen on */
                server_sockaddr.sin_port = htons(strtol(optarg, NULL, 0));
                break;
        }
    }
    if (!server_sockaddr.sin_port) {
        server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT);
    }

    wait_for_event(&server_sockaddr);
    return ret;

}
