/*
 * semtest.c: System V IPC check for trial kernels with CONFIG_SYSVIPC. See
 * BUILD.md ("Trial boot", step 4) for when to run it, and todos/sysvipc.md
 * for the background.
 *
 * It runs on the watch as ceres. It makes raw system calls and needs no
 * libc, so the kernel's own cross compiler in sonic-kernel-builder (a
 * "nolibc" toolchain) builds it as a small static 32-bit ARM binary:
 *
 *   docker run --rm -v "$T":/t sonic-kernel-builder arm-linux-gnueabi-gcc \
 *       -Os -marm -march=armv7-a -static -nostdlib -ffreestanding \
 *       -fno-builtin -fno-stack-protector -fno-pic -no-pie \
 *       -o /t/sonic-semtest /t/sonic/semtest.c
 *
 * What it checks:
 *   - what mkcal does with its database lock: semget by key, first without
 *     and then with IPC_CREAT|IPC_EXCL, then SETALL and GETVAL, then lock and
 *     unlock with semop and SEM_UNDO;
 *   - that a held lock blocks: IPC_NOWAIT, and semtimedop with a timeout;
 *   - the two task_struct members that this kernel keeps at the end of the
 *     struct (todos/sysvipc.md):
 *       sysvsem.undo_list: a child exits holding the lock, and the kernel
 *       must undo it (exit_sem);
 *       sysvshm.shm_clist: a child creates a shared memory segment, writes
 *       to it and exits without removing it (newseg, exit_shm); the parent
 *       then finds the segment by key and reads the child's byte.
 *
 * Every suspend attempt freezes user tasks with a fake signal, and semop
 * and semtimedop are never restarted after a signal (signal(7)). So a wait
 * on a semaphore ends with -EINTR at each attempt, about twice a second
 * off the cable. The test retries such a wait, as mkcal does, and reports
 * how often it had to.
 *
 * It removes every object it creates, and also any that a killed earlier
 * run left behind. It prints one line per step; exit status 0 means that
 * every step passed. On a kernel without SysV IPC, it stops at the first
 * semget with -38 (ENOSYS).
 */

#define NR_exit		1
#define NR_fork		2
#define NR_write	4
#define NR_getpid	20
#define NR_wait4	114
#define NR_getuid32	199
#define NR_clock_gettime 263
#define NR_semop	298
#define NR_semget	299
#define NR_semctl	300
#define NR_shmat	305
#define NR_shmdt	306
#define NR_shmget	307
#define NR_shmctl	308
#define NR_semtimedop	312

#define CLOCK_MONOTONIC	1
#define CLOCK_BOOTTIME	7

#define IPC_CREAT	01000
#define IPC_EXCL	02000
#define IPC_NOWAIT	04000
#define IPC_RMID	0
#define IPC_64		0x0100	/* as glibc passes it */
#define GETVAL		12
#define SETALL		17
#define SEM_UNDO	0x1000

#define ENOENT		2
#define EINTR		4
#define EAGAIN		11
#define ENOSYS		38

/* mkcal derives its key from a file with ftok(); these are fixed instead. */
#define SEM_KEY		0x534f4e43	/* "SONC" */
#define SHM_KEY		0x534f4e44	/* "SOND" */

struct sembuf {
	unsigned short sem_num;
	short sem_op;
	short sem_flg;
};

struct timespec32 {	/* the kernel's 32-bit struct timespec */
	long tv_sec;
	long tv_nsec;
};

static long sc(long n, long a, long b, long c, long d, long e)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;

	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
			 : "memory", "cc");
	return r0;
}

static int is_err(long r)
{
	return (unsigned long)r >= (unsigned long)-4095;
}

static unsigned int slen(const char *s)
{
	unsigned int n = 0;

	while (s[n])
		n++;
	return n;
}

static void out(const char *s)
{
	sc(NR_write, 1, (long)s, slen(s), 0, 0);
}

static void outnum(long v)
{
	char buf[16];
	int i = 15;
	unsigned long u;

	buf[i] = 0;
	u = v < 0 ? -(unsigned long)v : (unsigned long)v;
	do {
		unsigned long q = 0, r = u;

		while (r >= 10) {	/* no division: no libgcc needed */
			r -= 10;
			q++;
		}
		buf[--i] = '0' + r;
		u = q;
	} while (u);
	if (v < 0)
		buf[--i] = '-';
	out(buf + i);
}

static long ms(const struct timespec32 *a, const struct timespec32 *b)
{
	long s = b->tv_sec - a->tv_sec, ns = b->tv_nsec - a->tv_nsec, m = 0;

	if (ns < 0) {
		ns += 1000000000;
		s--;
	}
	while (ns >= 1000000) {	/* no division, as above */
		ns -= 1000000;
		m++;
	}
	return s * 1000 + m;
}

static int fails;

static void check(int ok, const char *what, long got)
{
	out(ok ? "ok    " : "FAIL  ");
	out(what);
	out(" -> ");
	outnum(got);
	out("\n");
	if (!ok)
		fails++;
}

static long semid = -1, shmid = -1;

static long getval(int num)
{
	return sc(NR_semctl, semid, num, GETVAL | IPC_64, 0, 0);
}

static long op(int num, int delta, int flags)
{
	struct sembuf b;

	b.sem_num = num;
	b.sem_op = delta;
	b.sem_flg = flags;
	return sc(NR_semop, semid, (long)&b, 1, 0, 0);
}

static void cleanup(void)
{
	if (semid >= 0)
		check(sc(NR_semctl, semid, 0, IPC_RMID | IPC_64, 0, 0) == 0,
		      "semctl(IPC_RMID) removes the semaphore set", 0);
	if (shmid >= 0)
		check(sc(NR_shmctl, shmid, IPC_RMID | IPC_64, 0, 0, 0) == 0,
		      "shmctl(IPC_RMID) removes the segment", 0);
}

static int run(void)
{
	struct timespec32 ts, m0, m1, b0, b1;
	unsigned short init[2];
	struct sembuf b;
	long r, pid, addr;
	int status = 0, eintr = 0;

	out("uid ");
	outnum(sc(NR_getuid32, 0, 0, 0, 0, 0));
	out(", pid ");
	outnum(sc(NR_getpid, 0, 0, 0, 0, 0));
	out("\n");

	/* A run that was killed half-way may have left its objects behind. */
	r = sc(NR_semget, SEM_KEY, 0, 0, 0, 0);
	if (!is_err(r) && sc(NR_semctl, r, 0, IPC_RMID | IPC_64, 0, 0) == 0)
		out("info  removed a semaphore set left by an earlier run\n");
	r = sc(NR_shmget, SHM_KEY, 0, 0, 0, 0);
	if (!is_err(r) && sc(NR_shmctl, r, IPC_RMID | IPC_64, 0, 0, 0) == 0)
		out("info  removed a segment left by an earlier run\n");

	/* mkcal: look the key up first, create the set only on ENOENT */
	r = sc(NR_semget, SEM_KEY, 2, 0, 0, 0);
	check(r == -ENOENT, "semget(key, 2, 0) before creation: -ENOENT", r);
	if (r == -ENOSYS) {
		out("SysV IPC is not available in this kernel (ENOSYS)\n");
		return 1;
	}
	semid = sc(NR_semget, SEM_KEY, 2, IPC_CREAT | IPC_EXCL | 0600, 0, 0);
	check(!is_err(semid), "semget(key, 2, IPC_CREAT|IPC_EXCL|0600) = id", semid);
	if (is_err(semid)) {
		semid = -1;
		return 1;
	}
	r = sc(NR_semget, SEM_KEY, 2, 0, 0, 0);
	check(r == semid, "semget(key, 2, 0) finds the same set", r);

	init[0] = 1;	/* the lock: free */
	init[1] = 0;
	r = sc(NR_semctl, semid, 0, SETALL | IPC_64, (long)init, 0);
	check(r == 0, "semctl(SETALL {1, 0})", r);
	check(getval(0) == 1, "semctl(0, GETVAL) = 1", getval(0));

	/* lock and unlock, as mkcal's ProcessMutex does */
	r = op(0, -1, SEM_UNDO);
	check(r == 0, "semop(0, -1, SEM_UNDO): lock", r);
	check(getval(0) == 0, "semctl(0, GETVAL) = 0 while locked", getval(0));
	r = op(0, -1, SEM_UNDO | IPC_NOWAIT);
	check(r == -EAGAIN, "semop(0, -1, IPC_NOWAIT) on the held lock: -EAGAIN", r);

	/*
	 * A wait on the held lock times out. -EINTR (a suspend attempt) is
	 * retried, for at most 10 s: in bursts, attempts come faster than the
	 * 200 ms.
	 */
	b.sem_num = 0;
	b.sem_op = -1;
	b.sem_flg = SEM_UNDO;
	sc(NR_clock_gettime, CLOCK_MONOTONIC, (long)&m0, 0, 0, 0);
	do {
		ts.tv_sec = 0;
		ts.tv_nsec = 200000000;
		r = sc(NR_semtimedop, semid, (long)&b, 1, (long)&ts, 0);
		if (r != -EINTR)
			break;
		eintr++;
		sc(NR_clock_gettime, CLOCK_MONOTONIC, (long)&m1, 0, 0, 0);
	} while (ms(&m0, &m1) < 10000);
	check(r == -EAGAIN, "semtimedop(0, -1, 200 ms) on the held lock: -EAGAIN", r);
	out("info  suspend attempts cut the 200 ms wait short (-EINTR, retried) ");
	outnum(eintr);
	out(" time(s)\n");

	/*
	 * The same wait for 3 s, not retried. Nothing sends this process a
	 * signal, so -EINTR can only come from the freezer. CLOCK_BOOTTIME
	 * minus CLOCK_MONOTONIC shows how long the watch slept meanwhile (0 if
	 * the suspend attempt was aborted after the freeze).
	 */
	ts.tv_sec = 3;
	ts.tv_nsec = 0;
	sc(NR_clock_gettime, CLOCK_MONOTONIC, (long)&m0, 0, 0, 0);
	sc(NR_clock_gettime, CLOCK_BOOTTIME, (long)&b0, 0, 0, 0);
	r = sc(NR_semtimedop, semid, (long)&b, 1, (long)&ts, 0);
	sc(NR_clock_gettime, CLOCK_MONOTONIC, (long)&m1, 0, 0, 0);
	sc(NR_clock_gettime, CLOCK_BOOTTIME, (long)&b1, 0, 0, 0);
	check(r == -EAGAIN || r == -EINTR,
	      "semtimedop(0, -1, 3 s) on the held lock, not retried: -EAGAIN or -EINTR", r);
	out("info  it returned after ");
	outnum(ms(&m0, &m1));
	out(" ms awake; the watch was suspended for ");
	outnum(ms(&b0, &b1) - ms(&m0, &m1));
	out(" ms of it\n");

	r = op(0, 1, SEM_UNDO);
	check(r == 0, "semop(0, +1, SEM_UNDO): unlock", r);
	check(getval(0) == 1, "semctl(0, GETVAL) = 1 again", getval(0));

	/* sysvsem.undo_list: a child dies holding the lock */
	pid = sc(NR_fork, 0, 0, 0, 0, 0);
	if (pid == 0) {
		r = op(0, -1, SEM_UNDO);
		op(1, 1, 0);	/* record that it held the lock, no undo */
		sc(NR_exit, r == 0 ? 0 : 1, 0, 0, 0, 0);
	}
	check(pid > 0, "fork() a child that exits holding the lock", pid);
	if (pid > 0) {
		r = sc(NR_wait4, pid, (long)&status, 0, 0, 0);
		check(r == pid && (status & 0xffff) == 0,
		      "child took the lock and exited 0 (wait status)", status);
		check(getval(1) == 1, "semctl(1, GETVAL) = 1: the child held the lock", getval(1));
		check(getval(0) == 1, "semctl(0, GETVAL) = 1: the kernel undid the child's lock at exit",
		      getval(0));
	}

	/* sysvshm.shm_clist: a child creates a segment and exits */
	pid = sc(NR_fork, 0, 0, 0, 0, 0);
	if (pid == 0) {
		long id = sc(NR_shmget, SHM_KEY, 4096, IPC_CREAT | IPC_EXCL | 0600, 0, 0);
		long a;

		if (is_err(id))
			sc(NR_exit, 2, 0, 0, 0, 0);
		a = sc(NR_shmat, id, 0, 0, 0, 0);
		if (is_err(a))
			sc(NR_exit, 3, 0, 0, 0, 0);
		*(volatile unsigned char *)a = 0x5a;
		sc(NR_exit, sc(NR_shmdt, a, 0, 0, 0, 0) == 0 ? 0 : 4, 0, 0, 0, 0);
	}
	check(pid > 0, "fork() a child that creates a segment and exits", pid);
	if (pid > 0) {
		r = sc(NR_wait4, pid, (long)&status, 0, 0, 0);
		check(r == pid && (status & 0xffff) == 0,
		      "child created, wrote and detached the segment (wait status)", status);
		shmid = sc(NR_shmget, SHM_KEY, 4096, 0, 0, 0);
		check(!is_err(shmid), "shmget(key, 4096, 0) finds the orphaned segment", shmid);
		if (is_err(shmid)) {
			shmid = -1;
		} else {
			addr = sc(NR_shmat, shmid, 0, 0, 0, 0);
			check(!is_err(addr), "shmat()", is_err(addr) ? addr : 0);
			if (!is_err(addr)) {
				check(*(volatile unsigned char *)addr == 0x5a,
				      "the segment holds the child's byte 0x5a (90)",
				      *(volatile unsigned char *)addr);
				check(sc(NR_shmdt, addr, 0, 0, 0, 0) == 0, "shmdt()", 0);
			}
		}
	}
	return 0;
}

void _start(void)
{
	run();
	cleanup();
	out(fails ? "RESULT: FAILED\n" : "RESULT: SysV semaphores and shared memory work\n");
	sc(NR_exit, fails ? 1 : 0, 0, 0, 0, 0);
	for (;;)
		;
}
