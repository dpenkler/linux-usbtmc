/***************************************************************************
 * An interactive programme to send requests to and read responses from an
 * SCPI compliant USBTMC device.
 *
 * Copyright (C) 2026 Dave Penkler <dpenkler@gmail.com>
 ***************************************************************************/

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#define USER
#include "tmc.h"
#include <limits.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <locale.h>

#include <readline/readline.h>
#include <readline/history.h>

#define USBTMC_TIMEOUT 10000 // 10 seconds
#define MAX_BL 2048

// Global variables
unsigned char buf[MAX_BL];
static int fd;
static int srun = 1;
static int sigwinch_received = 1;
static char *prog;
unsigned int timeout = USBTMC_TIMEOUT;
static int hex_flag =0;
static char hist_file[64] = ".";
static char prompt[64];
static char *dev_file = "/dev/usbtmc0";
static int have_prompt = 0;

#define NUM_STAT_BITS 8
typedef struct stat_entry {
	char * desc;
	unsigned int mask;
} stat_entryT;

typedef enum {SRE = 1, ESR = 2, ESE = 3, STB = 4} regT;
const char *regNames[] = {"none","SRE","ESR","ESE","STB"};

/*
  Service request enable register (SRE) Bit definitions
*/

#define SRE_TRG          1
#define SRE_USR          2
#define SRE_MSG          4
#define SRE_MAV         16
#define SRE_ESB         32
#define SRE_OPER       128

/* need these defines to inline bit definitions in SRE command strings */
#define xstr(s) str(s)
#define str(s) #s

/*
  Event Status Register (ESR) bit definitions
*/

#define ESR_OPC     1
#define ESR_RQL     2
#define ESR_QYE     4
#define ESR_DDE     8
#define ESR_EXE    16
#define ESR_CME    32
#define ESR_URQ    64
#define ESR_PON   128

const stat_entryT esr_bits[NUM_STAT_BITS] = {
	{"PON",  ESR_PON},
	{"URQ",  ESR_URQ},
	{"CME",  ESR_CME},
	{"EXE",  ESR_EXE},
	{"DDE",  ESR_DDE},
	{"QYE",  ESR_QYE},
	{"RQL",  ESR_RQL},
	{"OPC",  ESR_OPC}
};

/*
  Status register bit definitions
*/

#define STB_TRG    1
#define STB_USR    2
#define STB_MSG    4
#define STB_MAV   16
#define STB_ESB   32
#define STB_MSS   64
#define STB_OSR  128

const stat_entryT stb_bits[NUM_STAT_BITS] = {
	{"TRG",  STB_TRG},
	{"USR",  STB_USR},
	{"MSG",  STB_MSG},
	{"__8",  8}, /* not used */
	{"MAV",  STB_MAV},
	{"ESB",  STB_ESB},
	{"MSS",  STB_MSS},
	{"OSR",  STB_OSR}
};

static void show_bits(const stat_entryT table[], unsigned char val) {
	int i;
	for (i=0;i<NUM_STAT_BITS;i++)
		if (val & table[i].mask) {
			printf("%s ", table[i].desc);
		}
}

#define EMES(var) fputs(var,stderr)

static void usage(int abort) {
	EMES("\nUsage:\n");
	EMES(prog);
	EMES(" [-d device_file_name] [-t usbtmc_timeout] [-f history_file_name]\\\n"
	     "\t[-p prompt] [-X] [-h]\n");
	if (abort) {
		fprintf(stderr, "Try '%s -h' for more information.\n", prog);
		fprintf(stderr, "%s: aborted\n", prog);
		exit(1);
	}

}

static const char * help_string =
	"%s help info -\n"
	"An interactive terminal program for sending commands to"
	"an SCPI compliant usbtmc device and printing responses if any.\n"
	"\nOptions:\n"
	"-d device_file_name   (default \"/dev/usbtmc0\")\n"
	"-t usbtmc_timeout     (default %d milliseconds)\n"
	"-f history_file_name  (default \".%s_hist\")\n"
	"-p prompt             (default \"%s> \")\n"
	"-X force hexadecimal\n"
	"-h prints this help info and exits.\n";

static void print_help() {
	fprintf(stderr,help_string, prog, USBTMC_TIMEOUT, prog, prog);
}

/* Print buffer contents in parallel hexadecimal and ASCII */
static const char * hexdig = "0123456789ABCDEF";
static char * template =
  "12345678 |                         | "
             "                        |                 ";
void prhex(unsigned char * buf, int len) {
  int i,j,k,l,m;
  char *c,ob[80];

  for (i=0;i<len;i++)  {
    if ((i % 16) == 0) {
      k = 8; l = 63;
      strcpy(ob,template);
      for (j=0,c=&ob[8],m=i;j<8;m >>= 4,j++)
	*--c = hexdig[m & 0xf];
    }
    k += 1 + 2*((i % 8) == 0);
    ob[k++] = hexdig[(buf[i] & 0xf0)>>4];
    ob[k++] = hexdig[(buf[i] & 0xf)];
    ob[l++] = (buf[i] < 32 || buf[i] > 127) ? ' ' : buf[i];
    if (k == 60) { puts(ob); k = 0; }
   }
  if (k) puts(ob);
}

static void print_response(unsigned char *devbuf, int devdatalen) {
	int printable = 0;
	int i;

	if (!hex_flag) {
		printable  = true;
		for(i = 0; i < devdatalen; ++i) {
			if (!isprint(devbuf[i]) && !isspace(devbuf[i])){
				printable = false;
				break;
			}
		}
	} else printable = false;
	if (printable) {
		fwrite(devbuf,1,devdatalen,stdout); /* print response as ASCII string */
		/* but don't print an extra newline if it ends with LF or LF CR */
		if ((devdatalen > 2 &&
		     ((devbuf[devdatalen-2] != '\n') ||  (devbuf[devdatalen-1] != '\r'))) &&
		    devbuf[devdatalen-1] != '\n')
			putchar('\n');
	} else {
		prhex(devbuf,devdatalen); /* print contents with hex and ascii */
	}
}

/* Send string to device */
void sdev(char *msg) {
	write(fd,msg,strlen(msg));
}

/* Read string from device */
int rdev(unsigned char *buf, int max_len) {
	int len = read(fd,buf,max_len);
	if (len < 0) {
		fprintf(stderr, " rdev err %d\n", errno);
		perror("failed to read from scope");
		ioctl(fd,USBTMC_IOCTL_CLEAR);
		len = 0;
	}
	buf[len] = 0; /* zero terminate */
	return len;
}

void setReg(regT reg, int val) {
	char buf[32];
	snprintf(buf,32,"*%s %d\n",regNames[reg],val);
	sdev(buf);
}

int getReg(regT reg) {
	char buf[32];
	int val;
	snprintf(buf,32,"*%s?\n",regNames[reg]);
	sdev(buf);
	rdev((unsigned char *)buf,32);
	sscanf(buf,"%d",&val);
	return val;
}

static int showReg(regT reg, unsigned char val) {
	printf("%s = ", regNames[reg]);
	switch (reg) {
	case ESE:
	case ESR: show_bits(esr_bits,val);
		break;
	case STB:
	case SRE:
		show_bits(stb_bits,val);
		break;
	}
	printf("\n");
	return val;
}

unsigned int get_stb() {
	unsigned char stb, stb1;
	if (0 != ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)) {
		perror("read stb ioctl failed");
		exit(1);
	}
	return stb;
	if (0 != ioctl(fd,USBTMC_IOCTL_GET_STB,&stb1)) {
		perror("get stb ioctl failed");
		exit(1);
	}
	if (stb & (stb != (stb1 | 0x40))) {
		fprintf(stderr,"Warning stb get %x != stb read %x\n", stb1, stb);
		showReg(STB, stb);
		showReg(STB, stb1);
	}
	return stb;
}

static void sighandler (int sig) {
	sigwinch_received = 1;
}

static void linehandler_cb (char *line) {
	if (line == NULL || strcmp (line, "exit") == 0)	{
		if (line == NULL)
			printf ("\n");
		rl_callback_handler_remove ();
		srun = 0;
	} else {
		if (*line) {
			add_history (line);
			sdev(line);
			free (line);
			have_prompt = 0;
		}
	}
}

int main (int argc, char *argv[]) {
	struct timeval sel_timeout;
	fd_set fdsel[3];
	int need_prompt = 1;
	int esr;
	unsigned int oldtimeout;
	int c, len, n;

	prog = argv[0];
	strncpy(prompt, prog, 60);
	strncat(prompt, "> ", 3);

	strncat(&hist_file[1], prog, 57);
	strncat(hist_file, "_hist", 6);

	while ((c = getopt (argc, argv, "d:t:f:p:Xh")) != -1)
		switch (c)  {
		case 'd': dev_file =  optarg;             break;
		case 't': timeout  = atoi(optarg);        break;
		case 'f': strncpy(hist_file, optarg, 63); break;
		case 'p': strncpy(prompt, optarg, 63);    break;
		case 'X': hex_flag  = 1;                  break;
		case 'h':
			print_help();
			usage(0);
			goto out;
			break;
		default: usage(1);
		}

	/* Open file */
	if (0 > (fd = open(dev_file, O_RDWR))) {
		perror("failed to open device");
		exit(1);
	}

	/* Send device clear */
	if (0 != ioctl(fd,USBTMC_IOCTL_CLEAR)) {
		perror("Dev clear ioctl failed");
		exit(1);
	}

	ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&oldtimeout);
	ioctl(fd,USBTMC_IOCTL_SET_TIMEOUT,&timeout);
	printf("Timeout was %d, now %d\n", oldtimeout, timeout);

	/* Report all errors */

	setReg(ESE, ESR_CME | ESR_EXE | ESR_DDE | ESR_QYE);
	setReg(SRE, SRE_MAV |  SRE_ESB);
	showReg(ESE, getReg(ESE));
	showReg(ESR, getReg(ESR));
	showReg(SRE, getReg(SRE));
	showReg(STB, get_stb());
	sdev("*CLS\n");

	read_history(hist_file);

	setlocale (LC_ALL, "");

	signal (SIGWINCH, sighandler);

	rl_already_prompted = 1;
	rl_callback_handler_install (prompt, linehandler_cb);

	while (srun) {
		if (need_prompt) {
			fwrite(prompt,1,strlen(prompt),stdout);
			fflush(stdout);
			have_prompt = 1;
			need_prompt = 0;
		}
		memset(fdsel,0,sizeof(fdsel)); /* zero out select mask */
		get_stb(); // reset SRQ condition
		sel_timeout.tv_sec = 1;
		sel_timeout.tv_usec = 0;


		FD_SET(0,&fdsel[0]);
		FD_SET(fd,&fdsel[2]);
		n = select(fd+1,
			   (fd_set *)(&fdsel[0]),
			   (fd_set *)(&fdsel[1]),
			   (fd_set *)(&fdsel[2]),
			   &sel_timeout);
		if (n < 0 && errno != EINTR) {
			perror("select\n");
			rl_callback_handler_remove ();
			break;
		}

		if (sigwinch_received) {
			rl_resize_terminal ();
			sigwinch_received = 0;
		}

		if (!n && !have_prompt) {
			need_prompt = 1;
			continue;
		}

		if (FD_ISSET(fd,&fdsel[2])) {
			while (STB_MAV & get_stb()) {
				if (0 < (len = rdev(buf,MAX_BL)))
					print_response(buf, len);
				else break;
			}
			need_prompt = 1;
			while (STB_ESB & get_stb()) {
				esr = getReg(ESR);
				if (esr) {
					// showReg(ESR, esr);
					sdev(":SYST:ERR?\n");
					rdev(buf,MAX_BL);
					printf("Error: %s",buf);
				}
			}
		}

		if (FD_ISSET(0,&fdsel[0])) {
			rl_callback_read_char ();
		}
	}
	setReg(SRE,0);
	write_history(hist_file);
out:
	printf("%s: Done.\n", prog);
	exit(0);
}
