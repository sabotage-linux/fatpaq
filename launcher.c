/*
 * launcher.c – squashfs container launcher
 *
 * Usage: launcher <squashfs.img> <mountpoint> <command> [args...]
 *
 * Behaviour:
 *   1. Unshares user + mount namespaces (no host root required).
 *   2. Maps the calling uid/gid to 0:0 inside the user namespace.
 *   3. Mounts the squashfs image via squashfuse low-level FUSE APIs.
 *      /dev/fuse is opened by the FUSE library inside the new namespace,
 *      so no fusermount / fusermount3 helper is needed.
 *   4. Forks a child that:
 *        - waits for a "FUSE loop is live" signal (via a pipe),
 *        - bind-mounts /dev and /proc into the squashfs mountpoint,
 *        - chroots into the mountpoint,
 *        - execs the requested command.
 *   5. The parent runs fuse_session_loop(), supervising the child via
 *      SIGCHLD.  On child exit (or SIGINT/SIGTERM) the FUSE loop is
 *      stopped, the bind mounts and FUSE mount are torn down, and the
 *      child's exit status is propagated.
 *
 * Static linking:
 *   cc -DFUSE_USE_VERSION=26 -D_GNU_SOURCE -o launcher launcher.c \
 *       -I/path/to/squashfuse \
 *       /path/to/squashfuse/.libs/libsquashfuse.a \
 *       /path/to/libfuse/.libs/libfuse.a \
 *       -lz -lpthread -lm
 *
 *   For FUSE 3.x replace FUSE_USE_VERSION=26 with 30 and link libfuse3.a.
 *
 * The mountpoint directory must already exist.
 */

/* ---- FUSE version selection -------------------------------------------- */
#ifndef FUSE_USE_VERSION
# define FUSE_USE_VERSION 26
#endif

/* ---- feature-test macros ------------------------------------------------ */
#define _GNU_SOURCE		/* unshare, MNT_DETACH, pipe2 */

/* ---- standard headers --------------------------------------------------- */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>		/* PATH_MAX */
#include <sched.h>		/* unshare, CLONE_NEW* */
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- squashfuse public headers ------------------------------------------ */
/*
 * squashfuse.h aggregates: dir.h  file.h  fs.h  traverse.h  util.h  xattr.h
 * ll.h   declares: sqfs_ll, sqfs_ll_chan, sqfs_ll_op_*, sqfs_ll_mount/unmount,
 *                  sqfs_ll_open_with_subdir, sqfs_ll_destroy, …
 */
#include "squashfuse.h"
#include "ll.h"

/* =========================================================================
 * Declarations for symbols that live in libsquashfuse.a but are NOT
 * exported through any installed public header (they come from
 * fuseprivate.h / ll.c / fuseprivate.c).
 *
 * We do NOT call any of these directly from this file – they are called
 * by the sqfs_ll_op_* callbacks that are already compiled into the
 * library.  The declarations are reproduced here purely to satisfy the
 * compiler when this file is built outside the squashfuse source tree.
 * ========================================================================= */

/* fuseprivate.h – mount-ready notification characters */
#define NOTIFY_SUCCESS 's'
#define NOTIFY_FAILURE 'f'

/*
 * The following three functions are referenced by the compiled
 * sqfs_ll_op_* routines inside libsquashfuse.a.  Because this
 * translation unit never calls them directly, forward declarations
 * are not strictly required; they are shown here for clarity only.
 *
 *   int  sqfs_listxattr(sqfs *, sqfs_inode *, char *, size_t *);
 *   int  sqfs_statfs(sqfs *, struct statvfs *);
 *   void notify_mount_ready_async(const char *notify_pipe, char status);
 *   int  sqfs_enoattr(void);
 */

/* =========================================================================
 * Global state shared between main() and signal handlers
 * ========================================================================= */

/* PID of the child that runs the user's command. */
static volatile pid_t g_child_pid = -1;

/* The live FUSE session; set before entering fuse_session_loop(). */
static struct fuse_session *g_session = NULL;

/* Set to 1 by SIGCHLD handler once the child has been reaped. */
static volatile sig_atomic_t g_child_done = 0;

/* Propagated exit code of the child process. */
static volatile sig_atomic_t g_exit_status = 0;

/*
 * Write end of the "FUSE loop is live" pipe.
 * Written from launcher_ll_op_init() (called by fuse_session_loop in the
 * parent when it processes the kernel's initial FUSE_INIT handshake).
 * Set to -1 once used.
 */
static int g_fuse_ready_wfd = -1;

/* =========================================================================
 * FUSE init wrapper
 *
 * Replaces ops.init so we can signal the child that the FUSE event loop
 * has started and is ready to serve requests (i.e. bind-mount lookups
 * into the squashfs mountpoint will now succeed).
 * ========================================================================= */
static void launcher_ll_op_init(void *userdata, struct fuse_conn_info *conn)
{
	/* Let squashfuse do its own initialisation first (sends notify_pipe
	 * if one was configured, etc.). */
	sqfs_ll_op_init(userdata, conn);

	/* Unblock the child. */
	if (g_fuse_ready_wfd >= 0) {
		char go = 'g';
		(void)write(g_fuse_ready_wfd, &go, 1);
		close(g_fuse_ready_wfd);
		g_fuse_ready_wfd = -1;
	}
}

/* =========================================================================
 * Signal handlers
 * ========================================================================= */

/*
 * SIGCHLD – reap the child non-blockingly, record its exit status, and
 * ask the FUSE loop to stop.  No SA_RESTART: we rely on EINTR to wake
 * the blocking read() inside fuse_session_loop().
 */
static void sigchld_handler(int sig)
{
	int status;
	pid_t pid;
	(void)sig;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == g_child_pid) {
			g_child_done = 1;
			if (WIFEXITED(status))
				g_exit_status = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				g_exit_status = 128 + WTERMSIG(status);
			if (g_session)
				fuse_session_exit(g_session);
		}
	}
}

/*
 * SIGINT / SIGTERM – forward to the child, then stop the FUSE loop.
 * No SA_RESTART for the same reason as above.
 */
static void sigterm_handler(int sig)
{
	(void)sig;
	if (g_child_pid > 0)
		kill(g_child_pid, SIGTERM);
	if (g_session)
		fuse_session_exit(g_session);
}

static void setup_signals(void)
{
	struct sigaction sa;

	/* SIGCHLD */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP;	/* fire only on exit, not stop/continue */
	/* Intentionally no SA_RESTART so read() is interrupted by EINTR */
	sigaction(SIGCHLD, &sa, NULL);

	/* SIGINT / SIGTERM */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigterm_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;	/* no SA_RESTART */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

/* =========================================================================
 * Namespace helpers
 * ========================================================================= */

/* Write a NUL-terminated string to a file; return 0 on success. */
static int write_file(const char *path, const char *content)
{
	int fd;
	ssize_t n;
	size_t len = strlen(content);

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	n = write(fd, content, len);
	close(fd);
	return (n == (ssize_t) len) ? 0 : -1;
}

/*
 * Map the caller's real uid/gid to uid 0 / gid 0 inside the newly
 * created user namespace, making us "root" within it.  This is
 * required before calling mount(2).
 */
static int setup_idmap(uid_t real_uid, gid_t real_gid)
{
	char buf[64];

	/*
	 * Linux 3.19+: writing gid_map is only allowed after writing "deny"
	 * to setgroups.  Silently ignore errors on older kernels (ENOENT) or
	 * when the write is not permitted for other reasons (EACCES).
	 */
	if (write_file("/proc/self/setgroups", "deny") < 0 &&
	    errno != ENOENT && errno != EACCES) {
		perror("write /proc/self/setgroups");
		return -1;
	}

	snprintf(buf, sizeof(buf), "0 %d 1\n", (int)real_uid);
	if (write_file("/proc/self/uid_map", buf) < 0) {
		perror("write /proc/self/uid_map");
		return -1;
	}

	snprintf(buf, sizeof(buf), "0 %d 1\n", (int)real_gid);
	if (write_file("/proc/self/gid_map", buf) < 0) {
		perror("write /proc/self/gid_map");
		return -1;
	}

	return 0;
}

/* =========================================================================
 * Mount helpers
 * ========================================================================= */

static int bind_mount(const char *src, const char *dst)
{
	if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) < 0) {
		fprintf(stderr, "launcher: bind mount %s -> %s: %s\n",
			src, dst, strerror(errno));
		return -1;
	}
	return 0;
}

static void lazy_umount(const char *path)
{
	/*
	 * MNT_DETACH: detach the mount from the filesystem hierarchy even if
	 * it is busy; the mount point becomes invisible to new accesses.
	 * Ignore EINVAL (not mounted) and ENOENT (path does not exist).
	 */
	if (umount2(path, MNT_DETACH) < 0 && errno != EINVAL && errno != ENOENT)
		fprintf(stderr, "launcher: umount2 %s: %s\n", path,
			strerror(errno));
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char *argv[])
{
	static const char *bmounts[] = { "/dev", "/proc", "/tmp/.X11-unix" };

	const char *image;
	const char *mountpoint;
	char **cmd;
	uid_t real_uid;
	gid_t real_gid;

	sqfs_ll *ll = NULL;
	sqfs_ll_chan ch;
	struct fuse_lowlevel_ops ops;

	int pipefd[2] = { -1, -1 };
	pid_t child = -1;
	int exit_code = 1;

	/* ------------------------------------------------------------------ */
	if (argc < 4) {
		fprintf(stderr,
			"Usage: %s <squashfs.img> <mountpoint> <command> [args...]\n"
			"\n"
			"  Mounts <squashfs.img> at <mountpoint> inside a private\n"
			"  user + mount namespace, then chroots and execs <command>.\n"
			"  <mountpoint> must be a pre-existing directory.\n",
			argv[0]);
		return 1;
	}

	image = argv[1];
	mountpoint = argv[2];
	cmd = &argv[3];		/* cmd[0] = command, rest = arguments */

	real_uid = getuid();
	real_gid = getgid();

	memset(&ch, 0, sizeof(ch));

	/* ==================================================================
	 * Step 1 – User namespace
	 *
	 * Must be done before the mount namespace: an unprivileged process
	 * may only create a new mount namespace once it is root inside a
	 * user namespace.
	 * ================================================================== */
	if (unshare(CLONE_NEWUSER) < 0) {
		perror("launcher: unshare(CLONE_NEWUSER)");
		return 1;
	}

	/* Map ourselves to uid 0 / gid 0 within the new user namespace. */
	if (setup_idmap(real_uid, real_gid) < 0)
		return 1;

	/* ==================================================================
	 * Step 2 – Mount namespace
	 *
	 * We now hold CAP_SYS_ADMIN inside the user namespace, so this
	 * succeeds without any external privilege.
	 * ================================================================== */
	if (unshare(CLONE_NEWNS) < 0) {
		perror("launcher: unshare(CLONE_NEWNS)");
		return 1;
	}

	/*
	 * Make the entire inherited mount tree private so that none of our
	 * subsequent bind mounts or FUSE mounts propagate back to the host.
	 */
	if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
		perror("launcher: mount --make-rprivate /");
		return 1;
	}

	/* ==================================================================
	 * Step 3 – Guard file-descriptor 0-2
	 *
	 * sqfs_ll_open_with_subdir opens the image file; if it were to get
	 * fd 0, 1, or 2 it would later be clobbered.  Ensure those slots are
	 * occupied before we open the image.
	 * ================================================================== */
	for (;;) {
		int fd = open("/dev/null", O_RDONLY);
		if (fd < 0)
			break;	/* /dev/null unavailable – carry on */
		if (fd > 2) {
			close(fd);
			break;
		}
	}

	/* ==================================================================
	 * Step 4 – Open the squashfs image
	 * ================================================================== */
	ll = sqfs_ll_open_with_subdir(image, /*offset= */ 0, /*subdir= */ NULL);
	if (!ll) {
		fprintf(stderr, "launcher: cannot open squashfs image '%s'\n",
			image);
		return 1;
	}

	/* ==================================================================
	 * Step 5 – Build the FUSE low-level ops table
	 *
	 * We install our own init wrapper (launcher_ll_op_init) in place of
	 * the stock sqfs_ll_op_init so that we can signal the child once the
	 * FUSE event loop has processed the initial FUSE_INIT handshake.
	 * Every other operation is the unmodified squashfuse implementation.
	 * ================================================================== */
	memset(&ops, 0, sizeof(ops));
	ops.getattr = sqfs_ll_op_getattr;
	ops.opendir = sqfs_ll_op_opendir;
	ops.releasedir = sqfs_ll_op_releasedir;
	ops.readdir = sqfs_ll_op_readdir;
	ops.lookup = sqfs_ll_op_lookup;
	ops.open = sqfs_ll_op_open;
	ops.create = sqfs_ll_op_create;	/* returns EROFS */
	ops.release = sqfs_ll_op_release;
	ops.read = sqfs_ll_op_read;
	ops.readlink = sqfs_ll_op_readlink;
	ops.listxattr = sqfs_ll_op_listxattr;
	ops.getxattr = sqfs_ll_op_getxattr;
	ops.forget = sqfs_ll_op_forget;
	ops.statfs = stfs_ll_op_statfs;
	ops.init = launcher_ll_op_init;	/* our wrapper */

	/* ==================================================================
	 * Step 6 – Mount the squashfs image via FUSE
	 *
	 * sqfs_ll_mount() opens /dev/fuse and issues mount(2) internally.
	 * Because we hold CAP_SYS_ADMIN within the user namespace the kernel
	 * honours mount(2) without requiring fusermount / fusermount3.
	 *
	 * We pass minimal FUSE args: program name + "-o ro".
	 * fuse_mount (FUSE2) / fuse_session_mount (FUSE3) parse these for
	 * their own option processing; we never call fuse_parse_cmdline
	 * because we have our own argument layout.
	 * ================================================================== */
	{
		char *fuse_argv[] = { argv[0], "-o", "ro", NULL };
		struct fuse_args fargs = FUSE_ARGS_INIT(3, fuse_argv);
		sqfs_err sferr;

		sferr = sqfs_ll_mount(&ch, mountpoint, &fargs,
				      &ops, sizeof(ops), ll);

		/*
		 * fargs.argv points into our stack array of literals; fuse_args
		 * was initialised with allocated=0 so fuse_opt_free_args is a
		 * no-op, but call it for correctness in case the library added
		 * anything.
		 */
		fuse_opt_free_args(&fargs);

		if (sferr != SQFS_OK) {
			fprintf(stderr, "launcher: sqfs_ll_mount failed\n");
			sqfs_ll_destroy(ll);
			free(ll);
			return 1;
		}
	}

	/* Expose session pointer before any signal can arrive. */
	g_session = ch.session;

	/* ==================================================================
	 * Step 7 – "FUSE loop is live" pipe
	 *
	 * The child blocks on read(pipefd[0]) until the parent's init
	 * callback fires (the very first thing fuse_session_loop processes).
	 * Only then does the child perform bind mounts, since those trigger
	 * FUSE lookups that need the loop to be running.
	 * ================================================================== */
	if (pipe(pipefd) < 0) {
		perror("launcher: pipe");
		goto out_unmount;
	}
	g_fuse_ready_wfd = pipefd[1];	/* closed in launcher_ll_op_init */

	/* ==================================================================
	 * Step 8 – Signal handlers
	 *
	 * Install after g_session is set and the pipe is ready, but before
	 * fork() so that the child process inherits default dispositions
	 * (reset explicitly inside the child below).
	 * ================================================================== */
	setup_signals();

	/* ==================================================================
	 * Step 9 – Fork
	 * ================================================================== */
	child = fork();
	if (child < 0) {
		perror("launcher: fork");
		close(pipefd[0]);
		close(pipefd[1]);
		pipefd[0] = pipefd[1] = -1;
		g_fuse_ready_wfd = -1;
		goto out_unmount;
	}

	/* ------------------------------------------------------------------ *
	 * CHILD                                                               *
	 * ------------------------------------------------------------------ */
	if (child == 0) {
		char ready;

		/* Close the write end; we are the sole reader. */
		close(pipefd[1]);

		/* Restore default signal dispositions inherited from the parent. */
		signal(SIGCHLD, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);

		/*
		 * Wait until the parent's FUSE event loop has processed FUSE_INIT
		 * and is ready to serve VFS requests.  Without this gate the bind
		 * mounts below would dead-lock: the kernel would queue FUSE_LOOKUP
		 * requests that nobody was reading yet.
		 */
		if (read(pipefd[0], &ready, 1) < 1) {
			fprintf(stderr,
				"launcher child: FUSE-ready signal not received\n");
			_exit(127);
		}
		close(pipefd[0]);

		/*
		 * Bind-mount the host /dev and /proc into the squashfs image.
		 * Failure is non-fatal: some images legitimately lack these
		 * directories, and the user's command might not need them.
		 */
		for (int i = 0; i < sizeof(bmounts) / sizeof(bmounts[0]); ++i) {
			char dst[PATH_MAX];
			snprintf(dst, sizeof(dst), "%s/%s", mountpoint,
				 bmounts[i]);
			if (bind_mount(bmounts[i], dst) < 0)
				fprintf(stderr,
					"launcher: warning: %s bind mount failed\n",
					bmounts[i]);
		}

		/* Chroot into the squashfs image. */
		if (chroot(mountpoint) < 0) {
			perror("launcher: chroot");
			_exit(127);
		}
		if (chdir("/") < 0) {
			perror("launcher: chdir /");
			_exit(127);
		}

		/* Exec the requested command. */
		execvp(cmd[0], cmd);
		fprintf(stderr, "launcher: execvp %s: %s\n",
			cmd[0], strerror(errno));
		_exit(127);
	}

	/* ------------------------------------------------------------------ *
	 * PARENT                                                              *
	 * ------------------------------------------------------------------ */
	g_child_pid = child;

	/* Close the read end of the pipe; the child owns it now.
	 * The write end (g_fuse_ready_wfd / pipefd[1]) is closed inside
	 * launcher_ll_op_init() once we write the "go" byte. */
	close(pipefd[0]);
	pipefd[0] = -1;

	/* ==================================================================
	 * Step 10 – FUSE event loop
	 *
	 * Blocks here serving squashfs FUSE requests until:
	 *   - SIGCHLD fires (child exited) → sigchld_handler calls
	 *     fuse_session_exit(), which sets the exit flag.  EINTR from
	 *     the signal unblocks the internal read() and the loop checks
	 *     the flag on the next iteration.
	 *   - SIGINT / SIGTERM fires → sigterm_handler kills the child
	 *     and calls fuse_session_exit().
	 *   - The kernel closes the FUSE device (e.g. forced unmount).
	 * ================================================================== */
	fuse_session_loop(ch.session);

	/* ==================================================================
	 * Step 11 – Wait for the child to finish
	 *
	 * If the loop exited due to SIGINT/SIGTERM the child may still be
	 * alive.  Give it a 2-second grace period before escalating.
	 * ================================================================== */
	if (!g_child_done) {
		int status;
		pid_t reaped;

		reaped = waitpid(child, &status, WNOHANG);
		if (reaped == 0) {
			/* SIGTERM was already sent by sigterm_handler; wait 2 s. */
			int i;
			for (i = 0; i < 20 && reaped == 0; i++) {
				usleep(100000);	/* 100 ms */
				reaped = waitpid(child, &status, WNOHANG);
			}
			if (reaped == 0) {
				kill(child, SIGKILL);
				reaped = waitpid(child, &status, 0);
			}
		}
		if (reaped > 0) {
			if (WIFEXITED(status))
				g_exit_status = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				g_exit_status = 128 + WTERMSIG(status);
		}
	}

	exit_code = (int)g_exit_status;

	/* ==================================================================
	 * Step 12 – Teardown
	 *
	 * Unmount bind mounts first (they sit on top of the FUSE filesystem).
	 * Then destroy squashfuse state and unmount FUSE itself.
	 *
	 * lazy_umount (MNT_DETACH) is used throughout so that any lingering
	 * processes (the user's command may have spawned children) do not
	 * prevent the unmount from proceeding.
	 *
	 * It is harmless to call lazy_umount on a path that was never
	 * mounted; umount2 will return EINVAL which we silently ignore.
	 * ================================================================== */
 out_unmount:
	for (int i = 0; i < sizeof(bmounts) / sizeof(bmounts[0]); ++i) {
		char dst[PATH_MAX];
		snprintf(dst, sizeof(dst), "%s/%s", mountpoint, bmounts[i]);
		lazy_umount(dst);
	}

	sqfs_ll_destroy(ll);
	sqfs_ll_unmount(&ch, mountpoint);
	free(ll);

	return exit_code;
}
