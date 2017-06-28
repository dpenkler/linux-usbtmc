/***************************************************************************
                                 ttmc.c
                                 ------

    Programme to test the linux usbtmc driver against
    Keysight Infinivision 2000 X-series Oscilloscopes.
    This code also serves as an example for how to use
    the driver with  other instruments.

    copyright : (C) 2015 by Dave Penkler
    email     : dpenkler@gmail.com
 ***************************************************************************/

#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
//#include <linux/usb/tmc.h>
#include "tmc.h"

#define NUM_CAPS 9
typedef struct cap_entry {
	char * desc;
	unsigned int mask;
} cap_entryT;

/* Driver encoded capability masks */

const cap_entryT cap_list[NUM_CAPS] = {
	{"TRIGGER      ",  USBTMC488_CAPABILITY_TRIGGER        },
	{"REN_CONTROL  ",  USBTMC488_CAPABILITY_REN_CONTROL    },
	{"GOTO_LOCAL   ",  USBTMC488_CAPABILITY_GOTO_LOCAL     },
	{"LOCAL_LOCKOUT",  USBTMC488_CAPABILITY_LOCAL_LOCKOUT  },
	{"488_DOT_2    ",  USBTMC488_CAPABILITY_488_DOT_2      },
	{"DT1          ",  USBTMC488_CAPABILITY_DT1            },
	{"RL1          ",  USBTMC488_CAPABILITY_RL1            },
	{"SR1          ",  USBTMC488_CAPABILITY_SR1            },
	{"FULL_SCPI    ",  USBTMC488_CAPABILITY_FULL_SCPI      }
};

static int fd;
static const char star[] = {' ','*'};

static void show_caps(unsigned char caps) {
	int i;
	printf("Instrument capabilities: * prefix => supported capability\n");
	for (i=0;i<NUM_CAPS;i++)
		printf("%c%s\n",star[((caps & cap_list[i].mask) != 0)],
			cap_list[i].desc);
}

#define NUM_STAT_BITS 8
typedef struct stat_entry {
	char * desc;
	unsigned int mask;
} stat_entryT;

/*
   Service request enable register (SRE) Bit definitions
 */

#define SRE_Tigger             1
#define SRE_User               2
#define SRE_Message            4
#define SRE_MessageAvailable  16
#define SRE_Event_Status      32
#define SRE_Operation_Status 128

/* need this define to inline bit definitions in SRE command strings */
#define str(s) #s


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

static void show_stb(unsigned char stb) {
	int i;
	printf("STB = ");
	for (i=0;i<NUM_STAT_BITS;i++)
		if (stb & stb_bits[i].mask) {
			printf("%s ",stb_bits[i].desc);
		}
	printf("\n");
}

static int flag=0; /* set to 1 if srq handler called */

void srq_handler(int sig) {
	flag = 1;
}


/* Helper routines */

unsigned int get_stb() {
	unsigned char stb;
	if (0 != ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)) {
		perror("stb ioctl failed");
		exit(1);
	}	return stb;
}

/* Send string to scope */
void sscope(char *msg) {
	write(fd,msg,strlen(msg));
}

/* Read string from scope */
int rscope(char *buf, int max_len) {
	int len = read(fd,buf,max_len);
	buf[len] = 0; /* zero terminate */
	return len;
}

/* Wait for SRQ using poll() */
void wait_for_srq() {
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	poll(&pfd,1,-1);
}

void wait_for_user() {
	char buf[8];
	read(0,buf,1);
}

#define MAX_BL 2048

int main () {
  int rv;
  unsigned int tmp,tmp1,caps,ren;
  int len,n;
  unsigned char stb;
  char buf[MAX_BL];
  int oflags;
  fd_set fdsel[3];
  int i;

  /* Open file */
  if (0 > (fd = open("/dev/usbtmc0",O_RDWR))) {
	  perror("failed to open device");
	  exit(1);
  }

  /* Send device clear */
  ioctl(fd,USBTMC_IOCTL_CLEAR);

  /* Send identity query */
  sscope("*IDN?\n");

  /* Read and print returned identity string */
  rscope(buf,MAX_BL);
  printf(buf);


  /* Get and display instrument capabilities */
  if (0 != ioctl(fd,USBTMC488_IOCTL_GET_CAPS,&caps)) {
	  perror("get caps ioctl failed");
	  exit(1);
  }
  show_caps(caps);

test_stb:

  /* Test read STB ioctl */
  printf("Testing stb ioctl\n");
//  wait_for_user();

  tmp1 = get_stb();

  /* Use IEEE 488.2 *STB? command query to compare */
  sscope("*STB?\n");
  rscope(buf,MAX_BL);
  tmp = atoi(buf);

  if (tmp != tmp1)
	  fprintf(stderr,
		  "Warning: SCPI status byte = %x, ioctl status byte = %x\n",
		  tmp,tmp1);

  show_stb(tmp);
  show_stb(tmp1);


/* Set Message available mask and clear status */
  sscope("*SRE " str(SRE_MessageAvailable) "\n");
  sscope("*SRE?\n");
  rscope(buf,MAX_BL);
  printf("SRE = %s\n",buf);
  sscope("*CLS\n");
  show_stb(get_stb()); // clear srq
  sscope("*TST?\n"); /* Initiate self test */

  while (1) { /* spin on stb until mav bit is set */
	  tmp = get_stb();
	  if (tmp & STB_MAV) { /* Test for MAV */
		  printf("stb test success\n");
		  show_stb(tmp);
		  rscope(buf,MAX_BL);
		  printf(buf);
		  break;
	  }
	  usleep(10000); // 10 ms
  }

tren:

  printf("Testing remote disable, press enter to continue\n");
  wait_for_user();
  ren = 0;
  if (0 > ioctl(fd,USBTMC488_IOCTL_REN_CONTROL,&ren)) {
	  perror("ren clear ioctl failed");
	  exit(1);
  }
  printf("Remote disabled\n");
  sscope("SYSTEM:DSP \"USBTMC_488 driver Test\n\"");
  show_stb(get_stb()); // error ?

  printf("Testing Remote enable, press enter to continue\n");
  wait_for_user();
  ren = 1;
  if (ioctl(fd,USBTMC488_IOCTL_REN_CONTROL,&ren)) {
	  perror("ren set ioctl failed");
	  exit(1);
  }

  printf("Testing local lockout, press enter to continue\n");
  wait_for_user();
  if (ioctl(fd,USBTMC488_IOCTL_LOCAL_LOCKOUT)) {
    perror("llo ioctl failed");
    exit(1);
  }

  printf("Testing goto local, press enter to continue\n");
  wait_for_user();
  if (ioctl(fd,USBTMC488_IOCTL_GOTO_LOCAL)) {
	  perror("gtl ioctl failed");
	  exit(1);
  }
  sscope("*CLS\n");

tsel:
  printf("\n\n\nTesting select\n\n\n");
  memset(fdsel,0,sizeof(fdsel));
/* Set Mav mask and clear status */
  sscope("*SRE " str(SRE_MessageAvailable) "\n");
  sscope("*SRE?\n");
  rscope(buf,MAX_BL);
  printf("SRE = %s\n",buf);
  show_stb(get_stb()); // clear srq
  sscope("*TST?\n");

  FD_SET(fd,&fdsel[0]);
  n = select(fd+1,
	  (fd_set *)(&fdsel[0]),
	  (fd_set *)(&fdsel[1]),
	  (fd_set *)(&fdsel[2]),
	  NULL);
  if (n <= 0) {
	  perror("select\n");
	  exit(1);
  }

  if (FD_ISSET(fd,&fdsel[0])) {
	  printf("Select success\n");
	  show_stb(get_stb(stb));
	  rscope(buf,MAX_BL);
	  printf("Test result is: %s\n",buf);
  }

async:
  printf("Testing async\n");
  sscope(":TRIG:SOURCE CHAN1\n");
  signal(SIGIO, &srq_handler); /* dummy sample; sigaction( ) is better */
  fcntl(fd, F_SETOWN, getpid( ));
  oflags = fcntl(fd, F_GETFL);
  if (0 > fcntl(fd, F_SETFL, oflags | FASYNC)) {
	  perror("fasync fail\n");
	  exit(1);
  }
  sscope(":WAV:POINTS MAX");
  sscope(":TIM:MODE MAIN\n");
  sscope(":ACQ:COMP?;COUNT?;MODE?;POINTS?;SRAT?;TYPE?\n");
  rscope(buf,MAX_BL);
  printf(buf);
  sscope(":WAV:SOURCE CHAN1\n");
  /* enable OPC */
  sscope("*ESE 1\n"); // set operation complete in the event status enable reg
  sscope("*SRE " str(SRE_Event_Status) "\n"); // enable srq on event status reg
  sscope("*SRE?\n");
  rscope(buf,MAX_BL);
  printf("SRE = %s\n",buf);
  sscope("*ESE?\n");
  rscope(buf,MAX_BL);
  printf("ESE = %s\n",buf);
  sscope("*ESR?\n");
  rscope(buf,MAX_BL);
  printf("ESR = %s\n",buf);
  sscope("*CLS\n");
  show_stb(get_stb()); // clear srq
  printf("digitise , press enter to continue\n");
  wait_for_user();
  sscope(":DIG CHAN1\n");
  sscope("*OPC\n");

  if (sleep(5)) {
	  printf("Fasync %s\n",flag?"succeeded":"failed");
	  stb = get_stb();
	  show_stb(stb);
	  sscope(":WAV:PRE?\n");
	  rscope(buf,MAX_BL);
	  printf(buf);
  } else printf("Fail\n");
  show_stb(get_stb());

trigger:
  printf("Testing trigger\n");
  sscope(":TRIG:SOURCE EXT\n");
  sscope("*SRE " str(SRE_Trigger) "\n"); // enable SRQ on trigger
  sscope("*CLS\n");   // clear all status
  printf("press enter to continue\n");
  wait_for_user();

  if (ioctl(fd,USBTMC488_IOCTL_TRIGGER)) {
	  perror("trigger ioctl failed");
	  exit(1);
  }
  wait_for_srq();
  stb = get_stb();
  printf("trigger ioctl %s\n", (stb & STB_TRG) ? "success" : "failure");
  show_stb(stb);
  printf("ttmc: /done\n");
  close(fd);
  exit(0);
}
