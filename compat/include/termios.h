/* GOPHPER: WASI has no terminal control. Every call fails with ENOTTY,
 * and the types exist so that code asking for a terminal compiles. */
#ifndef _TERMIOS_H
#define _TERMIOS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

#define NCCS 32

struct termios {
	tcflag_t c_iflag;
	tcflag_t c_oflag;
	tcflag_t c_cflag;
	tcflag_t c_lflag;
	cc_t c_line;
	cc_t c_cc[NCCS];
	speed_t __c_ispeed;
	speed_t __c_ospeed;
};

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TIOCGWINSZ 0x5413

#define ECHO   0000010
#define ICANON 0000002
#define ISIG   0000001
#define VMIN   6
#define VTIME  5

int tcgetattr(int, struct termios *);
int tcsetattr(int, int, const struct termios *);

#ifdef __cplusplus
}
#endif

#endif
