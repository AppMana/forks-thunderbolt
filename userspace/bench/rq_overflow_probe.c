// SPDX-License-Identifier: MIT
/*
 * rq_overflow_probe — isolate the usb4_rdma post_recv full-queue return code.
 *
 * No NCCL, no mux, no peer: open usb4_rdma0, make a QP with max_recv_wr=N, then
 * post N+extra recvs WITHOUT draining. The kernel's tbv_post_recv returns
 * -ENOMEM(12) on a full RQ (ibdev.c). If userspace instead sees rc=38 (ENOSYS)
 * the bug is in the provider / legacy-write uverbs ABI, not the kernel logic.
 *
 *   cc -O2 -o rq_overflow_probe rq_overflow_probe.c -libverbs
 *   ./rq_overflow_probe [device] [max_recv_wr]
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

int main(int argc, char **argv) {
	const char *want = argc > 1 ? argv[1] : "usb4_rdma0";
	int qd = argc > 2 ? atoi(argv[2]) : 8;

	int n = 0;
	struct ibv_device **list = ibv_get_device_list(&n);
	struct ibv_device *dev = NULL;
	for (int i = 0; i < n; i++)
		if (!strncmp(ibv_get_device_name(list[i]), want, strlen(want))) { dev = list[i]; break; }
	if (!dev) { fprintf(stderr, "device %s not found\n", want); return 2; }

	struct ibv_context *ctx = ibv_open_device(dev);
	struct ibv_pd *pd = ibv_alloc_pd(ctx);
	struct ibv_cq *cq = ibv_create_cq(ctx, 256, NULL, NULL, 0);
	if (!ctx || !pd || !cq) { fprintf(stderr, "ctx/pd/cq alloc failed errno=%d\n", errno); return 2; }

	struct ibv_qp_init_attr qa; memset(&qa, 0, sizeof(qa));
	qa.send_cq = cq; qa.recv_cq = cq; qa.qp_type = IBV_QPT_RC;
	qa.cap.max_send_wr = qd; qa.cap.max_recv_wr = qd;
	qa.cap.max_send_sge = 1; qa.cap.max_recv_sge = 1;
	struct ibv_qp *qp = ibv_create_qp(pd, &qa);
	if (!qp) { fprintf(stderr, "create_qp failed errno=%d\n", errno); return 2; }
	printf("opened %s qp=%u max_recv_wr=%d (actual cap %d)\n",
	       want, qp->qp_num, qd, qa.cap.max_recv_wr);

	/* move QP to INIT so post_recv is legal */
	struct ibv_qp_attr at; memset(&at, 0, sizeof(at));
	at.qp_state = IBV_QPS_INIT; at.pkey_index = 0; at.port_num = 1;
	at.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
	int mr_rc = ibv_modify_qp(qp, &at, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
	printf("modify->INIT rc=%d\n", mr_rc);

	static char buf[1 << 20]; /* 1 MiB, like NCCL's chunk */
	struct ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(buf), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!mr) { fprintf(stderr, "reg_mr failed errno=%d\n", errno); return 2; }

	/* post recvs past the queue depth without draining; report the overflow rc */
	for (int i = 0; i < qd + 4; i++) {
		struct ibv_sge sge = { .addr = (uintptr_t)buf, .length = sizeof(buf), .lkey = mr->lkey };
		struct ibv_recv_wr wr; memset(&wr, 0, sizeof(wr));
		wr.wr_id = i; wr.sg_list = &sge; wr.num_sge = 1;
		struct ibv_recv_wr *bad = NULL;
		errno = 0;
		int rc = ibv_post_recv(qp, &wr, &bad);
		printf("post_recv i=%d rc=%d errno=%d (%s)%s\n", i, rc, errno, strerror(errno),
		       i >= qd ? "  <-- OVERFLOW (expect ENOMEM=12, NOT ENOSYS=38)" : "");
		if (rc) break;
	}
	return 0;
}
