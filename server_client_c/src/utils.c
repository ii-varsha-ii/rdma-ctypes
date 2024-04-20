#include "utils.h"


void show_memory_map(const char* memory_region) {
    info("-------------------\n")
    info("Memory Map: %s \n", memory_region);
    info("-------------------\n");
}

void show_exchange_buffer(struct msg *attr) {
    info("---------------------------------------------------------\n");
    info("------------EXCHANGE BUFFER------------\n");
    info("---------------------------------------------------------\n");
    info("message %p\n", attr);
    info("message, type: %d\n", attr->type);
    if(attr->type == HELLO) {
        info("message: hello: %lu \n", attr->data.offset);
    }
    if (attr->type == FRAME){
        info("message: data.mr.address: %p \n", attr->data.mr.addr);
    }
    info("---------------------------------------------------------\n");
}

struct ibv_mr* rdma_buffer_alloc(struct ibv_pd *pd, uint32_t size,
                                 enum ibv_access_flags permission)
{
    struct ibv_mr *mr = NULL;
    if (!pd) {
        error("Protection domain is NULL \n");
        return NULL;
    }
    void *buf = calloc(1, size);
    if (!buf) {
        error("failed to allocate buffer, -ENOMEM\n");
        return NULL;
    }
    debug("Buffer allocated: %p , len: %u \n", buf, size);
    mr = rdma_buffer_register(pd, buf, size, permission);
    if(!mr){
        free(buf);
    }
    return mr;
}

struct ibv_mr *rdma_buffer_register(struct ibv_pd *pd,
                                    void *addr, uint32_t length,
                                    enum ibv_access_flags permission)
{
    struct ibv_mr *mr = NULL;
    if (!pd) {
        error("Protection domain is NULL, ignoring \n");
        return NULL;
    }
    mr = ibv_reg_mr(pd, addr, length, permission);
    if (!mr) {
        error("Failed to create mr on buffer, errno: %d \n", -errno);
        return NULL;
    }
    debug("Registered: %p , len: %u , stag: 0x%x \n",
         mr->addr,
         (unsigned int) mr->length,
         mr->lkey);
    return mr;
}

void rdma_buffer_free(struct ibv_mr *mr)
{
    if (!mr) {
        error("Passed memory region is NULL, ignoring\n");
        return ;
    }
    void *to_free = mr->addr;
    rdma_buffer_deregister(mr);
    debug("Buffer %p free'ed\n", to_free);
    free(to_free);
}

void rdma_buffer_deregister(struct ibv_mr *mr)
{
    if (!mr) {
        error("Passed memory region is NULL, ignoring\n");
        return;
    }
    debug("Deregistered: %p , len: %u , stag : 0x%x \n",
         mr->addr,
         (unsigned int) mr->length,
         mr->lkey);
    ibv_dereg_mr(mr);
}

int process_work_completion_events(struct ibv_cq *cq, struct ibv_wc *wc, int max_wc) {
    int total_wc, i;
    int ret = -1;
    ret = ibv_req_notify_cq(cq, 0);
    if (ret) {
        error("Failed to request further notifications %d \n", -errno);
        return -errno;
    }
    total_wc = 0;
    do {
        ret = ibv_poll_cq(cq /* the CQ, we got notification for */,
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
            return -(wc[i].status);
        }
    }
    ibv_ack_cq_events(cq, 1);
    return total_wc;
}

int poll_for_completion_events(struct ibv_cq *cq, struct ibv_wc *wc, int num_wc) {
    int total_wc = process_work_completion_events(cq, wc, num_wc);
    return total_wc;
}

int disconnect_and_cleanup(struct per_client_resources* client_res, struct memory_region* region) {
    int ret = -1;
    ret = rdma_disconnect(client_res->client_id);
    if (ret) {
        error("Failed to disconnect, errno: %d \n", -errno);
    }

    /* Destroy QP */
    rdma_destroy_qp(client_res->client_id);

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
    /* Destroy rdma server id */
    ret = rdma_destroy_id(cm_server_id);
    if (ret) {
        error("Failed to destroy server id cleanly, %d \n", -errno);
    }

    /* Destroy client cm id */
    ret = rdma_destroy_id(client_res->client_id);
    if (ret) {
        error("Failed to destroy client id cleanly, %d \n", -errno);
    }

    rdma_buffer_deregister(server_buff.buffer);
    rdma_buffer_deregister(client_buff.buffer);

    /* Destroy protection domain */
    ret = ibv_dealloc_pd(client_res->pd);
    if (ret) {
        error("Failed to destroy client protection domain cleanly, %d \n", -errno);

    }
    rdma_destroy_event_channel(cm_event_channel);
    free(client_res);
    free(region);
    printf("Client resource clean up is complete \n");
    return 0;
}

/* Code acknowledgment: rping.c from librdmacm/examples */
int get_addr(char *dst, struct sockaddr *addr)
{
    struct addrinfo *res;
    int ret = -1;
    ret = getaddrinfo(dst, NULL, NULL, &res);
    if (ret) {
        error("getaddrinfo failed - invalid hostname or IP address\n");
        return ret;
    }
    memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(res);
    return ret;
}
