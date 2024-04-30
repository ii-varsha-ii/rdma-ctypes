#include "structs.h"
#include "utils.h"

static struct ibv_send_wr client_send_wr, *bad_client_send_wr = NULL;
static struct ibv_recv_wr server_recv_wr, *bad_server_recv_wr = NULL;
static struct ibv_sge client_send_sge, server_recv_sge;

#define CLIENT_HELLO (123)
int connect_server(struct sockaddr_in *s_addr, const char* str_to_send);

/*
 * Create client ID and resolve the destination IP address to RDMA Address
 */
static void resolve_addr(struct sockaddr_in *s_addr) {
    client_res = (struct client_resources *) malloc(sizeof(struct client_resources));

    /* Init Event Channel */
    HANDLE(cm_event_channel = rdma_create_event_channel());
    debug("RDMA CM event channel created: %p \n", cm_event_channel)

    /* Create Client ID with the above Event Channel */
    HANDLE_NZ(rdma_create_id(cm_event_channel, &cm_client_id,
                             NULL,
                             RDMA_PS_TCP));
    client_res->id = cm_client_id;

    /* Resolve IP address to RDMA address and bind to client_id */
    HANDLE_NZ(rdma_resolve_addr(client_res->id, NULL, (struct sockaddr *) s_addr, TIMEOUTMS));
    debug("waiting for cm event: RDMA_CM_EVENT_ADDR_RESOLVED\n")
}

/* Setup client resources like PD, CC, CQ, QP */
static int setup_client_resources(struct sockaddr_in *s_addr) {
    info("Trying to connect to server at : %s port: %d \n",
         inet_ntoa(s_addr->sin_addr),
         ntohs(s_addr->sin_port));

    // Init Protection Domain
    HANDLE(client_res->pd = ibv_alloc_pd(client_res->id->verbs));
    debug("Protection Domain (PD) allocated: %p \n", client_res->pd)

    // Init Completion Channel
    HANDLE(client_res->comp_channel = ibv_create_comp_channel(cm_client_id->verbs));
    debug("Completion channel created: %p \n", client_res->comp_channel)

    // Init Completion Queue
    HANDLE(client_res->cq = ibv_create_cq(client_res->id->verbs /* which device*/,
                                          CQ_CAPACITY /* maximum capacity*/,
                                          NULL /* user context, not used here */,
                                          client_res->comp_channel /* which IO completion channel */,
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

    HANDLE_NZ(rdma_create_qp(client_res->id,
                             client_res->pd,
                             &qp_init_attr));

    client_res->qp = cm_client_id->qp;
    debug("Client QP created: %p \n", client_res->qp)
    return 0;
}

/*
 * Send HELLO message to server
 */
static int send_hello_to_server() {
    debug("Register Client Buffer")
    client_buff.message = malloc(sizeof(struct msg));
    client_buff.message->type = HELLO;
    client_buff.message->data.offset = CLIENT_HELLO;

    HANDLE(client_buff.buffer = rdma_buffer_register(client_res->pd,
                                                     client_buff.message,
                                                     sizeof(struct msg),
                                                     (IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE)));

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

/*
 * Receive HELLO message from server
 */
static int receive_hello_from_server() {
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

/*
 * Create a memory region and set it the frame to send.
 * Register a memory buffer for the local memory region with the pd,
 * memory address, length and permissions.
 * */
static void build_message_buffer(struct memory_region *region, const char* str_to_send) {
    region->memory_region = malloc(DATA_SIZE);
    strcpy(region->memory_region, str_to_send);

    region->memory_region_mr = rdma_buffer_register(client_res->pd,
                                                    region->memory_region,
                                                        DATA_SIZE,
                                                        (IBV_ACCESS_LOCAL_WRITE |
                                                         IBV_ACCESS_REMOTE_READ |
                                                         IBV_ACCESS_REMOTE_WRITE));

    debug("Memory Region registration successful: %p\n", (unsigned long *) region->memory_region);
}

/*
 * Send the registered memory region to the server as a FRAME message type
 */
static int send_message_to_server(struct memory_region *region) {
    struct ibv_wc wc;
    client_buff.message = malloc(sizeof(struct msg));
    client_buff.message->type = FRAME;

    memcpy(&client_buff.message->data.mr, region->memory_region_mr, sizeof(struct ibv_mr));
    client_buff.message->data.mr.addr = (void *) (region->memory_region);

    client_buff.buffer = rdma_buffer_register(client_res->pd,
                                              client_buff.message,
                                              sizeof(struct msg),
                                              (IBV_ACCESS_LOCAL_WRITE |
                                               IBV_ACCESS_REMOTE_READ |
                                               IBV_ACCESS_REMOTE_WRITE));

    show_exchange_buffer(client_buff.message);

    client_send_sge.addr = (uint64_t) client_buff.message;
    client_send_sge.length = (uint32_t) sizeof(struct msg);
    client_send_sge.lkey = client_buff.buffer->lkey;

    bzero(&client_send_wr, sizeof(client_send_wr));
    client_send_wr.sg_list = &client_send_sge;
    client_send_wr.num_sge = 1;
    client_send_wr.opcode = IBV_WR_SEND;
    client_send_wr.send_flags = IBV_SEND_SIGNALED;

    HANDLE_NZ(ibv_post_send(client_res->qp, &client_send_wr, &bad_client_send_wr));
    int ret = process_work_completion_events(client_res->comp_channel, &wc, 1);
    if (ret < 0) {
        return ret;
    }
    info("POST MESSAGE TO SERVER \n");
    return 0;
}

/* Send Connect Request to the server */
static void connect_to_server() {
    struct rdma_conn_param conn_param;
    bzero(&conn_param, sizeof(conn_param));
    conn_param.initiator_depth = 3;
    conn_param.responder_resources = 3;
    conn_param.retry_count = 3;
    HANDLE_NZ(rdma_connect(client_res->id, &conn_param));
}

static int verify_offset_match() {
    if (server_buff.message->type == HELLO && server_buff.message->data.offset == CLIENT_HELLO + 1) {
        info("Offset matches with server \n");
    } else {
        error("Offset mismatch: HELLO communication failed \n");
        return -1;
    }
    return 0;
}

/*
 * Blocking while loop which checks for incoming events and calls the necessary
 * functions based on the received events
 */
static int wait_for_event(struct sockaddr_in *s_addr, const char* str_to_send) {

    struct rdma_cm_event *received_event = NULL;
    struct memory_region *frame = NULL;

    resolve_addr(s_addr);
    while (rdma_get_cm_event(cm_event_channel, &received_event) == 0) {
        struct ibv_wc wc;
        struct rdma_cm_event cm_event;
        memcpy(&cm_event, received_event, sizeof(*received_event));
        info("%s event received \n", rdma_event_str(cm_event.event));
        HANDLE_NZ(rdma_ack_cm_event(received_event));
        switch (cm_event.event) {
            /* RDMA Address Resolution completed successfully */
            case RDMA_CM_EVENT_ADDR_RESOLVED:
                rdma_resolve_route(client_res->id, TIMEOUTMS);
                break;

                /* RDMA Route established successfully */
            case RDMA_CM_EVENT_ROUTE_RESOLVED:
                frame = (struct memory_region *) malloc(sizeof(struct memory_region *));
                setup_client_resources(s_addr);
                receive_hello_from_server();
                build_message_buffer(frame, str_to_send);
                connect_to_server();
                break;

            case RDMA_CM_EVENT_ESTABLISHED:
                send_hello_to_server();
                process_work_completion_events(client_res->comp_channel, &wc, 2);
                int ret = verify_offset_match();
                if (ret == -1) {
                    rdma_disconnect(client_res->id);
                    return 0;
                }
                int count = 0;
                while(count < 5) {
                    int ret = send_message_to_server(frame);
                    if (ret) {
                        info("Send lost. Retry");
                        count += 1;
                        continue;
                    }
                    break;
                }
                rdma_disconnect(client_res->id);
                break;

            case RDMA_CM_EVENT_DISCONNECTED:
                info("%s event received \n", rdma_event_str(cm_event.event));
                disconnect_client(client_res, cm_event_channel, frame, &server_buff, &client_buff);
                return 0;
            default:
                error("Event not found %s", (char *) cm_event.event);
                break;
        }
    }
}


int connect_server(struct sockaddr_in *s_addr, const char* str_to_send) {
    wait_for_event(s_addr, str_to_send);
}


//int main(int argc, char **argv) {
//    struct sockaddr_in server_sockaddr;
//    int ret;
//
//    bzero(&server_sockaddr, sizeof server_sockaddr);
//    server_sockaddr.sin_family = AF_INET;
//    server_sockaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
//
//    ret = get_addr("10.10.1.2", (struct sockaddr *) &server_sockaddr);
//    if (ret) {
//        error("Invalid dst addr");
//        return ret;
//    }
//    server_sockaddr.sin_port = htons(1234);
//    connect_server(&server_sockaddr, "teststring");
//    return ret;
//}
