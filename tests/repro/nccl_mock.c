// nccl_mock — a small libibverbs reproducer that mocks NCCL's RC/RDMA_WRITE
// usage pattern, parameterized to sweep the dimensions that separate a passing
// single-QP perftest run from NCCL's failing multi-channel run.
//
// NCCL (per its NET/IB debug log) opens N channels = N concurrent RC QPs per
// peer and drives them with RDMA_WRITE. ib_write_bw uses 1 QP and passes on
// usb4_rdma; NCCL hits IBV_WC_RETRY_EXC_ERR. This harness reproduces the
// pattern at the verbs API level so we can isolate which knob (QP count,
// opcode, outstanding depth, inline, MTU) flips pass -> retry-exceeded, on both
// usb4_rdma0 (native TB) and rxe0 (soft-RoCE control) with identical code.
//
// RC, RoCE v2 (GID-based AH). OOB exchange over TCP, perftest-style.
//
// build: gcc -O2 -o nccl_mock nccl_mock.c -libverbs
// run:   server: ./nccl_mock -d usb4_rdma0 -g 1 -q 6 -o write
//        client: ./nccl_mock -d usb4_rdma0 -g 1 -q 6 -o write -c <server-ip>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>

#define PORT 18515
#define MAXQ 32

struct qp_info {            // exchanged over OOB per QP
	uint32_t qpn;
	uint32_t psn;
	uint64_t addr;         // remote buffer base (for RDMA_WRITE target)
	uint32_t rkey;
} __attribute__((packed));

struct ctx {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq[MAXQ];
	struct ibv_qp *qp[MAXQ];
	struct ibv_mr *mr;
	char *buf;
	union ibv_gid gid;
	struct qp_info local[MAXQ], remote[MAXQ];
};

static int g_qps = 6, g_gid = 1, g_size = 65536, g_iters = 64, g_outst = 8;
static int g_port = 1, g_inline = 0, g_mtu = 0; // mtu 0 = device active
static const char *g_dev = "usb4_rdma0";
static const char *g_op = "write";   // write | write_imm | send
static const char *g_server = NULL;  // client mode if set

static enum ibv_wr_opcode op_code(void) {
	if (!strcmp(g_op, "write")) return IBV_WR_RDMA_WRITE;
	if (!strcmp(g_op, "write_imm")) return IBV_WR_RDMA_WRITE_WITH_IMM;
	return IBV_WR_SEND;
}

static struct ibv_context *open_dev(const char *name) {
	int n; struct ibv_device **list = ibv_get_device_list(&n);
	struct ibv_context *c = NULL;
	for (int i = 0; i < n; i++)
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			c = ibv_open_device(list[i]); break;
		}
	ibv_free_device_list(list);
	return c;
}

// minimal OOB: server accepts, client connects; then both exchange the qp_info
// array symmetrically.
static int oob_connect(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(PORT) };
	if (g_server) {
		inet_pton(AF_INET, g_server, &sa.sin_addr);
		while (connect(fd, (void*)&sa, sizeof sa)) { usleep(200000); }
		return fd;
	}
	sa.sin_addr.s_addr = INADDR_ANY;
	if (bind(fd, (void*)&sa, sizeof sa)) { perror("bind"); exit(1); }
	listen(fd, 1);
	int c = accept(fd, NULL, NULL); close(fd); return c;
}

static int xchg(int fd, void *out, void *in, size_t len) {
	// server sends first then recvs; client recvs first then sends — avoids deadlock
	if (g_server) {
		if (read(fd, in, len) != (ssize_t)len) return -1;
		if (write(fd, out, len) != (ssize_t)len) return -1;
	} else {
		if (write(fd, out, len) != (ssize_t)len) return -1;
		if (read(fd, in, len) != (ssize_t)len) return -1;
	}
	return 0;
}

static enum ibv_mtu mtu_enum(int m) {
	switch (m) {
	case 256: return IBV_MTU_256; case 512: return IBV_MTU_512;
	case 1024: return IBV_MTU_1024; case 2048: return IBV_MTU_2048;
	default: return IBV_MTU_4096;
	}
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IONBF, 0);   // unbuffered: survive SIGTERM on hang
	int o;
	while ((o = getopt(argc, argv, "d:g:q:o:s:n:x:c:p:im:")) != -1) {
		switch (o) {
		case 'd': g_dev = optarg; break;
		case 'g': g_gid = atoi(optarg); break;
		case 'q': g_qps = atoi(optarg); break;
		case 'o': g_op = optarg; break;
		case 's': g_size = atoi(optarg); break;
		case 'n': g_iters = atoi(optarg); break;
		case 'x': g_outst = atoi(optarg); break;
		case 'c': g_server = optarg; break;
		case 'p': g_port = atoi(optarg); break;
		case 'i': g_inline = 1; break;
		case 'm': g_mtu = atoi(optarg); break;
		}
	}
	if (g_qps > MAXQ) g_qps = MAXQ;
	struct ctx c; memset(&c, 0, sizeof c);

	c.ctx = open_dev(g_dev);
	if (!c.ctx) { fprintf(stderr, "open %s failed\n", g_dev); return 1; }
	if (ibv_query_gid(c.ctx, g_port, g_gid, &c.gid)) { perror("query_gid"); return 1; }

	struct ibv_port_attr pa;
	ibv_query_port(c.ctx, g_port, &pa);
	enum ibv_mtu mtu = g_mtu ? mtu_enum(g_mtu) : pa.active_mtu;

	c.pd = ibv_alloc_pd(c.ctx);
	size_t bufsz = (size_t)g_size * g_outst + 4096;
	c.buf = aligned_alloc(4096, bufsz);
	memset(c.buf, g_server ? 0 : 0xAB, bufsz);
	c.mr = ibv_reg_mr(c.pd, c.buf, bufsz,
			  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
			  IBV_ACCESS_REMOTE_READ);
	if (!c.mr) { perror("reg_mr"); return 1; }

	for (int i = 0; i < g_qps; i++) {
		c.cq[i] = ibv_create_cq(c.ctx, g_outst * 4 + 16, NULL, NULL, 0);
		struct ibv_qp_init_attr ia = {
			.send_cq = c.cq[i], .recv_cq = c.cq[i],
			.cap = { .max_send_wr = g_outst * 2 + 4,
				 .max_recv_wr = g_outst * 2 + 4,
				 .max_send_sge = 1, .max_recv_sge = 1,
				 .max_inline_data = g_inline ? 256 : 0 },
			.qp_type = IBV_QPT_RC,
		};
		c.qp[i] = ibv_create_qp(c.pd, &ia);
		if (!c.qp[i]) { fprintf(stderr, "create_qp[%d] failed: %m\n", i); return 1; }
		c.local[i].qpn = c.qp[i]->qp_num;
		c.local[i].psn = 0x1000 + i * 0x100;
		c.local[i].addr = (uint64_t)(uintptr_t)c.buf;
		c.local[i].rkey = c.mr->rkey;
	}

	int fd = oob_connect();
	// exchange gid + per-qp info
	if (xchg(fd, &c.gid, &c.remote[0].addr /*scratch*/, 0) ) {} // noop
	union ibv_gid rgid;
	if (xchg(fd, &c.gid, &rgid, sizeof rgid)) { fprintf(stderr, "gid xchg failed\n"); return 1; }
	if (xchg(fd, c.local, c.remote, sizeof(struct qp_info) * g_qps)) {
		fprintf(stderr, "qp xchg failed\n"); return 1;
	}

	// bring each QP to RTS
	for (int i = 0; i < g_qps; i++) {
		struct ibv_qp_attr a = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
			.port_num = g_port,
			.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ };
		if (ibv_modify_qp(c.qp[i], &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
				  IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) { perror("INIT"); return 1; }

		struct ibv_qp_attr r = { .qp_state = IBV_QPS_RTR, .path_mtu = mtu,
			.dest_qp_num = c.remote[i].qpn, .rq_psn = c.remote[i].psn,
			.max_dest_rd_atomic = 1, .min_rnr_timer = 12,
			.ah_attr = { .is_global = 1, .port_num = g_port,
				.grh = { .hop_limit = 64, .sgid_index = g_gid } } };
		memcpy(&r.ah_attr.grh.dgid, &rgid, sizeof rgid);
		if (ibv_modify_qp(c.qp[i], &r, IBV_QP_STATE | IBV_QP_AV |
				  IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
				  IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
			perror("RTR"); return 1; }

		struct ibv_qp_attr s = { .qp_state = IBV_QPS_RTS, .timeout = 14,
			.retry_cnt = 7, .rnr_retry = 7, .sq_psn = c.local[i].psn,
			.max_rd_atomic = 1 };
		if (ibv_modify_qp(c.qp[i], &s, IBV_QP_STATE | IBV_QP_TIMEOUT |
				  IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
				  IBV_QP_MAX_QP_RD_ATOMIC)) { perror("RTS"); return 1; }
	}
	printf("[%s] %s dev=%s qps=%d op=%s size=%d outst=%d mtu=%d gid=%d : QPs RTS\n",
	       g_server ? "client" : "server", g_dev, g_dev, g_qps, g_op, g_size,
	       g_outst, mtu, g_gid);

	// barrier so both sides are RTS before traffic
	{ char b = 'x', r2; xchg(fd, &b, &r2, 1); }

	// Only the client drives RDMA_WRITE into the server's buffer (mock of
	// NCCL's one-directional per-channel send). Server just polls nothing and
	// waits on a final OOB barrier — its CQEs are remote-write (silent for
	// plain WRITE), so the proof of success is client completions + no error.
	int rc = 0;
	if (g_server) {
		// For SEND/WRITE_IMM the receiver consumes one recv per message:
		// post the initial window AND REPOST on every completion, exactly
		// like a real application (NCCL reposts continuously). Without the
		// repost the test starves the responder after the first window and
		// measures the harness, not the driver.
		if (op_code() != IBV_WR_RDMA_WRITE) {
			long need = (long)g_iters * g_qps;
			long handed = 0;
			for (int i = 0; i < g_qps; i++)
				for (int k = 0; k < g_outst; k++) {
					struct ibv_sge sge = { (uintptr_t)c.buf + (size_t)g_size*k,
						g_size, c.mr->lkey };
					struct ibv_recv_wr rw = { .wr_id = k, .sg_list = &sge,
						.num_sge = 1 }, *bad;
					ibv_post_recv(c.qp[i], &rw, &bad);
				}
			while (handed < need) {
				int progress = 0;
				for (int i = 0; i < g_qps; i++) {
					struct ibv_wc wc[8];
					int ne = ibv_poll_cq(c.cq[i], 8, wc);
					for (int j = 0; j < ne; j++) {
						if (wc[j].status != IBV_WC_SUCCESS) {
							fprintf(stderr, "*** recv-side qp%d status=%s\n",
								i, ibv_wc_status_str(wc[j].status));
							handed = need; break;
						}
						handed++; progress++;
						struct ibv_sge sge = { (uintptr_t)c.buf, g_size,
							c.mr->lkey };
						struct ibv_recv_wr rw = { .wr_id = 0, .sg_list = &sge,
							.num_sge = 1 }, *bad;
						ibv_post_recv(c.qp[i], &rw, &bad);
					}
				}
				if (!progress) usleep(1000);
			}
			fprintf(stderr, "[receiver] handled %ld message completions\n", handed);
		}
	} else {
		enum ibv_wr_opcode opc = op_code();
		long done = 0, target = (long)g_iters * g_qps;
		long posted = 0;
		int per_qp = g_iters;
		long *qp_posted = calloc(g_qps, sizeof(long));
		long *qp_done = calloc(g_qps, sizeof(long));
		time_t t0 = time(NULL), tlast = t0;
		while (done < target) {
			time_t now = time(NULL);
			if (now != tlast) { tlast = now;
				fprintf(stderr, "[client %s] heartbeat: posted=%ld done=%ld/%ld\n",
					g_dev, posted, done, target);
				if (now - t0 > 12) { fprintf(stderr,
					"*** [%s] TIMEOUT: %ld posted, only %ld completed, "
					"no error CQE -> completions never returned\n",
					g_dev, posted, done); rc = 4; goto out; }
			}
			for (int i = 0; i < g_qps; i++) {
				while (qp_posted[i] - qp_done[i] < g_outst &&
				       qp_posted[i] < per_qp) {
					int slot = qp_posted[i] % g_outst;
					struct ibv_sge sge = { (uintptr_t)c.buf + (size_t)g_size*slot,
						g_size, c.mr->lkey };
					struct ibv_send_wr w = { .wr_id = ((long)i<<32)|qp_posted[i],
						.sg_list = &sge, .num_sge = 1, .opcode = opc,
						.send_flags = IBV_SEND_SIGNALED };
					if (opc != IBV_WR_SEND) {
						w.wr.rdma.remote_addr = c.remote[i].addr +
							(size_t)g_size*slot;
						w.wr.rdma.rkey = c.remote[i].rkey;
					}
					if (opc == IBV_WR_RDMA_WRITE_WITH_IMM)
						w.imm_data = htonl(0xc0de);
					struct ibv_send_wr *bad;
					if (ibv_post_send(c.qp[i], &w, &bad)) {
						fprintf(stderr, "post_send qp%d: %m\n", i); rc=2; goto out;
					}
					qp_posted[i]++; posted++;
				}
			}
			for (int i = 0; i < g_qps; i++) {
				struct ibv_wc wc[16];
				int ne = ibv_poll_cq(c.cq[i], 16, wc);
				for (int j = 0; j < ne; j++) {
					if (wc[j].status != IBV_WC_SUCCESS) {
						fprintf(stderr, "*** qp%d wr_id=%llu status=%s(%d) "
							"opcode=%d -> FAIL on %s\n", i,
							(unsigned long long)wc[j].wr_id,
							ibv_wc_status_str(wc[j].status),
							wc[j].status, wc[j].opcode, g_dev);
						rc = 3; goto out;
					}
					qp_done[i]++; done++;
				}
			}
		}
		printf("[client] %s OK: %ld completions across %d QPs, no errors\n",
		       g_dev, done, g_qps);
out:    ;
	}
	// final barrier + teardown
	{ char b = 'z', r3; xchg(fd, &b, &r3, 1); }
	close(fd);
	return rc;
}
