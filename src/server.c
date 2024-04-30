#include "utils.h"
#include "structs.h"

static struct ibv_recv_wr client_recv_wr, *bad_client_recv_wr = NULL;
static struct ibv_send_wr server_send_wr, *bad_server_send_wr = NULL;
static struct ibv_sge client_recv_sge, server_send_sge;
static const char* received_frame = NULL;

/* function prototypes */
const char* start_rdma_server(struct sockaddr_in *server_sockaddr);


/* Setup client resources like PD, CC, CQ and QP */
static void setup_client_resources(struct rdma_cm_id *cm_client_id) {
    /* Init the client resources */
    client_res = (struct client_resources *) malloc(sizeof(struct client_resources));
    if (!cm_client_id) {
        error("Client id is still NULL \n");
        return;
    }
    client_res->id = cm_client_id;

    /* Init the Protection Domain for the client */
    HANDLE(client_res->pd = ibv_alloc_pd(cm_client_id->verbs));
    debug("Protection domain (PD) allocated: %p \n", client_res->pd)

    /* Init the Completion Channel for the client */
    HANDLE(client_res->comp_channel = ibv_create_comp_channel(cm_client_id->verbs));
    debug("I/O completion event channel created: %p \n",
          client_res->comp_channel)

    /* Init the Completion Queue for the client */
    HANDLE(client_res->cq = ibv_create_cq(cm_client_id->verbs,
                                          CQ_CAPACITY,
                                          NULL,
                                          client_res->comp_channel,
                                          0));
    debug("Completion queue (CQ) created: %p with %d elements \n",
          client_res->cq, client_res->cq->cqe)

    /* Ask for the event for all activities in the completion queue */
    HANDLE_NZ(ibv_req_notify_cq(client_res->cq,
                                0));
    bzero(&qp_init_attr, sizeof qp_init_attr);
    qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
    qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
    qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
    qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
    qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
    qp_init_attr.recv_cq = client_res->cq;
    qp_init_attr.send_cq = client_res->cq;
    HANDLE_NZ(rdma_create_qp(client_res->id,
                             client_res->pd,
                             &qp_init_attr));
    client_res->qp = cm_client_id->qp;
    debug("Client QP created: %p \n", client_res->qp)
}

/*
 * Receive HELLO message from server
 * Create a buffer for receiving client's message. Register ibv_reg_mr using client's pd,
 * message addr, length of message, and permissions for the buffer.
 * Post the client buffer as a Receive Request (RR) to the Work Queue (WQ)
 * */
static void post_recv_hello() {
    client_buff.message = malloc(sizeof(struct msg));
    HANDLE(client_buff.buffer = rdma_buffer_register(client_res->pd,
                                                     client_buff.message,
                                                     sizeof(struct msg),
                                                     (IBV_ACCESS_LOCAL_WRITE)));

    client_recv_sge.addr = (uint64_t) client_buff.buffer->addr;
    client_recv_sge.length = client_buff.buffer->length;
    client_recv_sge.lkey = client_buff.buffer->lkey;

    bzero(&client_recv_wr, sizeof(client_recv_wr));
    client_recv_wr.sg_list = &client_recv_sge;
    client_recv_wr.num_sge = 1;

    HANDLE_NZ(ibv_post_recv(client_res->qp,
                            &client_recv_wr,
                            &bad_client_recv_wr));

    info("Receive buffer for client OFFSET pre-posting is successful \n");
}

/*
 * Send HELLO message to initiate conversation
 */
static void post_send_hello() {
    struct ibv_wc wc;
    server_buff.message = malloc(sizeof(struct msg));
    server_buff.message->type = HELLO;
    server_buff.message->data.offset = client_buff.message->data.offset + 1;

    server_buff.buffer = rdma_buffer_register(client_res->pd,
                                              server_buff.message,
                                              sizeof(struct msg),
                                              (IBV_ACCESS_LOCAL_WRITE |
                                               IBV_ACCESS_REMOTE_READ |
                                               IBV_ACCESS_REMOTE_WRITE));

    show_exchange_buffer(server_buff.message);

    server_send_sge.addr = (uint64_t) server_buff.message;
    server_send_sge.length = (uint32_t) sizeof(struct msg);
    server_send_sge.lkey = server_buff.buffer->lkey;

    bzero(&server_send_wr, sizeof(server_send_wr));
    server_send_wr.sg_list = &server_send_sge;
    server_send_wr.num_sge = 1;
    server_send_wr.opcode = IBV_WR_SEND;
    server_send_wr.send_flags = IBV_SEND_SIGNALED;

    HANDLE_NZ(ibv_post_send(client_res->qp, &server_send_wr, &bad_server_send_wr));
    process_work_completion_events(client_res->comp_channel,  &wc, 1);

    info("Send request with memory map ADDRESS is successful \n");
}

/*
 * Create Memory Buffer to receive message from client
 */
static void build_message_buffer(struct memory_region *region) {
    region->memory_region = malloc(DATA_SIZE);
    memset(region->memory_region, 0, DATA_SIZE);

    show_memory_map(region->memory_region);
    region->memory_region_mr = rdma_buffer_register(client_res->pd,
                                                    region->memory_region,
                                                  DATA_SIZE,
                                                  (IBV_ACCESS_LOCAL_WRITE |
                                                   IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
    debug("Memory Map address - %p\n", (unsigned long *) region->memory_region);
}

/*
 * Post a QR to receive the message buffer from the client
 */
static int receive_client_message() {
    struct ibv_wc wc;
    debug("Register Client Buffer\n");
    client_buff.message = malloc(sizeof(struct msg));
    HANDLE(client_buff.buffer = rdma_buffer_register(client_res->pd,
                                                     client_buff.message,
                                                     sizeof(struct msg),
                                                     (IBV_ACCESS_LOCAL_WRITE)));

    client_recv_sge.addr = (uint64_t) client_buff.message;
    client_recv_sge.length = (uint32_t) sizeof(struct msg);
    client_recv_sge.lkey = client_buff.buffer->lkey;

    bzero(&client_recv_wr, sizeof(client_recv_wr));
    client_recv_wr.sg_list = &client_recv_sge;
    client_recv_wr.num_sge = 1;

    HANDLE_NZ(ibv_post_recv(client_res->qp /* which QP */,
                            &client_recv_wr /* receive work request*/,
                            &bad_client_recv_wr /* error WRs */));
    process_work_completion_events(client_res->comp_channel, &wc, 1);

    debug("Pre-posting Client Receive Buffer for Address successfull \n");
    return 0;
}

/*
 * Post a QR with RDMA_READ to read_message_buffer
 */
static void read_message_buffer(struct memory_region *region) {
    memcpy(&region->server_mr, &client_buff.message->data.mr, sizeof(region->server_mr));

    server_send_sge.addr = (uintptr_t) (region->memory_region);
    server_send_sge.length = (uint32_t) DATA_SIZE;
    server_send_sge.lkey = region->memory_region_mr->lkey;

    bzero(&server_send_wr, sizeof(server_send_wr));
    server_send_wr.sg_list = &server_send_sge;
    server_send_wr.num_sge = 1;
    server_send_wr.opcode = IBV_WR_RDMA_READ;
    server_send_wr.send_flags = IBV_SEND_SIGNALED;
    server_send_wr.wr.rdma.remote_addr = (uintptr_t) region->server_mr.addr;
    server_send_wr.wr.rdma.rkey = region->server_mr.rkey;
    HANDLE_NZ(ibv_post_send(client_res->qp,
                            &server_send_wr,
                            &bad_server_send_wr));
    debug("RDMA read the remote memory map. \n");
}

/* Establish connection with the client */
static void accept_conn(struct rdma_cm_id *cm_client_id) {
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 3;
    conn_param.responder_resources = 3;

    HANDLE_NZ(rdma_accept(cm_client_id, &conn_param));
    debug("Wait for : RDMA_CM_EVENT_ESTABLISHED event \n")
}

static int wait_for_event() {
    int ret;

    struct rdma_cm_event *received_event = NULL;
    struct memory_region *frame = NULL;

    while (rdma_get_cm_event(cm_event_channel, &received_event) == 0) {
        /* Initialize the received event */
        struct rdma_cm_event cm_event;
        struct ibv_wc wc;
        memcpy(&cm_event, received_event, sizeof(*received_event));
        info("%s event received \n", rdma_event_str(cm_event.event));

        HANDLE_NZ(rdma_ack_cm_event(received_event));
        /* SWITCH case to check what type of event was received */
        switch (cm_event.event) {

            /* Initially Server receives and Client Connect Request */
            case RDMA_CM_EVENT_CONNECT_REQUEST:
                frame = (struct memory_region *) malloc(sizeof(struct memory_region *));
                setup_client_resources(cm_event.id); // send a recv req for client_metadata
                build_message_buffer(frame);
                post_recv_hello();
                accept_conn(cm_event.id);
                break;

            /*  After the client establishes the connection */
            case RDMA_CM_EVENT_ESTABLISHED:
                process_work_completion_events(client_res->comp_channel, &wc, 1);
                post_send_hello();
                receive_client_message();
                read_message_buffer(frame);
                int count = 0;
                while (strcmp(frame->memory_region, "") == 0 && count < 5) {
                    read_message_buffer(frame);
                    sleep(1);
                    count += 1;
                }
                if (count >= 5) {
                    return -1;
                }
                show_memory_map(frame->memory_region);
                received_frame = frame->memory_region;
                break;

            /* Disconnect and Cleanup */
            case RDMA_CM_EVENT_DISCONNECTED:
                info("%s event received \n", rdma_event_str(cm_event.event));
                disconnect_server(client_res, cm_event_channel, cm_server_id);
                return 0;
            default:
                error("Event not found %s", (char *) cm_event.event);
                return -1;
        }
    }
    return ret;
}

const char* start_rdma_server(struct sockaddr_in *server_sockaddr) {
    // Create RDMA Event Channel
    HANDLE(cm_event_channel = rdma_create_event_channel());

    // Using the RDMA EC, create ID to track communication information
    HANDLE_NZ(rdma_create_id(cm_event_channel, &cm_server_id, NULL, RDMA_PS_TCP));

    info("Received at: %s , port: %d \n",
         inet_ntoa(server_sockaddr->sin_addr),
         ntohs(server_sockaddr->sin_port));

    // Using the ID, bind the socket information
    HANDLE_NZ(rdma_bind_addr(cm_server_id, (struct sockaddr *) server_sockaddr));

    // Server Listening...
    HANDLE_NZ(rdma_listen(cm_server_id, 8));
    info("Server is listening successfully at: %s , port: %d \n",
         inet_ntoa(server_sockaddr->sin_addr),
         ntohs(server_sockaddr->sin_port));

    wait_for_event();
    info("%s - received frame \n", received_frame);
    return received_frame;
}
//
//int main(int argc, char **argv) {
//    struct sockaddr_in server_sockaddr;
//
//    bzero(&server_sockaddr, sizeof(server_sockaddr));
//    server_sockaddr.sin_family = AF_INET;
//    server_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
//    int ret = get_addr("10.10.1.2", (struct sockaddr*) &server_sockaddr);
//    if (ret) {
//        error("Invalid IP");
//        return ret;
//    }
//    server_sockaddr.sin_port = htons(1234);
//    start_rdma_server(&server_sockaddr);
//}
