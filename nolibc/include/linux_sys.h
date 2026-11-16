/*
 * linux_sys.h - the Linux x86-64 kernel interface: system call numbers,
 * the `syscall` instruction (inline assembly), and the kernel's own
 * structure layouts and constants.  Nothing here comes from a C library.
 *
 * Kernel convention: number in rax, arguments in rdi, rsi, rdx, r10, r8,
 * r9; result in rax (negative errno on failure); rcx and r11 clobbered.
 */
#ifndef NOLIBC_LINUX_SYS_H
#define NOLIBC_LINUX_SYS_H

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_poll 7
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_rt_sigaction 13
#define SYS_ioctl 16
#define SYS_mremap 25
#define SYS_dup2 33
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_execve 59
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_getcwd 79
#define SYS_chdir 80
#define SYS_mkdir 83
#define SYS_unlink 87
#define SYS_readlink 89
#define SYS_getdents64 217
#define SYS_clock_gettime 228
#define SYS_exit_group 231
#define SYS_newfstatat 262
#define SYS_faccessat 269

static inline long sys0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}

static inline long sys1(long n, long a) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}

static inline long sys2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

static inline long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline long sys4(long n, long a, long b, long c, long d) {
    register long r10 __asm__("r10") = d;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long sys5(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

/* errno values (negated in system call results) */
#define K_EINTR 4
#define K_EAGAIN 11
#define K_EEXIST 17

/* open(2) flags */
#define K_O_RDONLY 0
#define K_O_WRONLY 01
#define K_O_RDWR 02
#define K_O_CREAT 0100
#define K_O_TRUNC 01000
#define K_O_APPEND 02000
#define K_O_DIRECTORY 0200000
#define K_O_CLOEXEC 02000000
#define K_O_PATH 010000000
#define K_AT_FDCWD (-100)
#define K_W_OK 2

/* mmap */
#define K_PROT_READ 1
#define K_PROT_WRITE 2
#define K_MAP_PRIVATE 0x02
#define K_MAP_ANONYMOUS 0x20
#define K_MREMAP_MAYMOVE 1

/* signals */
#define K_SIGINT 2
#define K_SIGQUIT 3
#define K_SIGTSTP 20
#define K_SIGWINCH 28
#define K_SA_RESTORER 0x04000000
#define K_SIG_DFL ((void (*)(int))0)
#define K_SIG_IGN ((void (*)(int))1)

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask;
};

/* returns from a signal handler (rt_sigreturn), in start.S */
void __nolibc_restorer(void);

/* terminal (struct termios as the kernel's TCGETS/TCSETS see it) */
#define K_TCGETS 0x5401
#define K_TCSETSF 0x5404
#define K_TIOCGWINSZ 0x5413

struct k_termios {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
};

#define K_BRKINT 0000002
#define K_INPCK 0000020
#define K_ISTRIP 0000040
#define K_ICRNL 0000400
#define K_IXON 0002000
#define K_OPOST 0000001
#define K_CS8 0000060
#define K_ISIG 0000001
#define K_ICANON 0000002
#define K_ECHO 0000010
#define K_IEXTEN 0100000
#define K_VTIME 5
#define K_VMIN 6

struct k_winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

struct k_pollfd {
    int fd;
    short events, revents;
};
#define K_POLLIN 1

struct k_timespec {
    long tv_sec, tv_nsec;
};
#define K_CLOCK_REALTIME 0

/* struct stat on x86-64 */
struct k_stat {
    unsigned long st_dev, st_ino, st_nlink;
    unsigned int st_mode, st_uid, st_gid, pad0;
    unsigned long st_rdev;
    long st_size, st_blksize, st_blocks;
    long st_atime_, st_atime_nsec, st_mtime_, st_mtime_nsec, st_ctime_, st_ctime_nsec;
    long unused[3];
};
#define K_S_IFMT 0170000
#define K_S_IFDIR 0040000

struct k_dirent64 {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};
#define K_DT_UNKNOWN 0
#define K_DT_DIR 4
#define K_DT_LNK 10

/* the process environment, set up by _start */
extern char **__nolibc_environ;

#endif
