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
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
//#include <linux/usb/tmc.h>
#define __user
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
#define MAX_BL 2048
unsigned char buf[MAX_BL];

static const char *yesno[] = {"no","yes"};

static void show_caps(unsigned char caps) {
	int i;
	printf("\nUSBTMC-488 Capabilities\n");
	for (i=0;i<NUM_CAPS;i++)
	  printf("%s : %s\n",cap_list[i].desc,
		 yesno[((caps & cap_list[i].mask) != 0)]);
}

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

/* Helper routines */

static double main_ts;
double getTS() {
	struct timeval tv;
	double ts,tmp;
	gettimeofday(&tv,NULL);
	tmp = tv.tv_sec + (double)tv.tv_usec/1e6;
	ts = tmp - main_ts;
	main_ts = tmp;
	return ts;
}


static void show_bits(const stat_entryT table[], unsigned char val) {
	int i;
	for (i=0;i<NUM_STAT_BITS;i++)
		if (val & table[i].mask) {
			printf("%s ", table[i].desc);
		}
}

static int flag=0; /* set to 1 if srq handler called */

void srq_handler(int sig) {
	flag = 1;
}

/* Send string to scope */
void sscope(char *msg) {
	write(fd,msg,strlen(msg));
//	printf("sscope: %s",msg);
}

/* Read string from scope */
int rscope(char *buf, int max_len) {
	int len = read(fd,buf,max_len);
	if (len < 0) {
	  perror("failed to read from scope");
	  ioctl(fd,USBTMC_IOCTL_CLEAR);
	  len = 0;
	}
	buf[len] = 0; /* zero terminate */
	return len;
}

unsigned int get_stb() {
	unsigned char stb;
	if (0 != ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)) {
		perror("stb ioctl failed");
		exit(1);
	}
	return stb;
}

unsigned int get_srq_stb() {
	unsigned char stb;
	if (0 != ioctl(fd,USBTMC_IOCTL_GET_SRQ_STB,&stb)) {
		if (errno == ENOMSG) {
			stb = 0;
		} else {
			perror("get_srq_stb ioctl failed");
			exit(1);
		}
	}
	return stb;
}

void setReg(regT reg, int val) {
	char buf[32];
	snprintf(buf,32,"*%s %d\n",regNames[reg],val);
	sscope(buf);
}

int getReg(regT reg) {
	char buf[32];
	int val;
	snprintf(buf,32,"*%s?\n",regNames[reg]);
	sscope(buf);
	rscope(buf,32);
	sscanf(buf,"%d",&val);
	return val;
}

static int showReg(regT reg, unsigned char val) {
	int i;
#ifdef TIMESTAMP
	printf("%11.6f " ,getTS());
#endif
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

void showTER() {
	char buf[32];
	sscope(":TER?\n");
	rscope(buf,32);
	printf("TER=%s",buf);
}

/* Wait for SRQ using poll() */
void wait_for_srq_poll() {
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLPRI;
	poll(&pfd,1,-1);
}

static int wait_for_srq(unsigned int timeout) {
	return ioctl(fd, USBTMC488_IOCTL_WAIT_SRQ, &timeout);
}

void wait_for_user() {
	char buf[8];
	read(0,buf,1);
}

static int testSTB() {
	unsigned int tmp, tmp1;
	int i;

        /* Test read STB ioctl */
	printf("\nTesting stb ioctl\n");
	sscope("*CLS\n");
	setReg(ESE, ESR_OPC); /* Set ESB in STB on OPC */
	setReg(SRE, 0);       /* No SRQ */
	tmp1 = get_stb();     /* ioctl first because SCPI query will set MAV */
	tmp  = getReg(STB);   /* Use IEEE 488.2 *STB? SCPI query */
	if (tmp != tmp1) {
		fprintf(stderr,
			"Warning: SCPI status byte = %x, ioctl status byte = %x\n",
			tmp,tmp1);
		showReg(STB,tmp);
	}
	showReg(STB,tmp1);
	/* Test USBTMC_IOCTL_GET_STB */
	ioctl(fd, USBTMC_IOCTL_GET_STB, &tmp1);
		if (tmp != tmp1) {
		fprintf(stderr,
			"Warning: ioctl read status byte = %x != ioctl getstatus byte = %x\n",
			tmp,tmp1);
	}

	sscope(":MEAS:FREQ?;VRMS?;VPP? CHAN1;*OPC\n"); /* Start "Operation" */

	/* Poll  STB until ESB bit is set for OPC */
	for (i=0;i<100;i++) {
		tmp = get_stb();
		if (tmp & STB_ESB) { /* Test for OPC */
			rscope(buf,MAX_BL);
			printf("Scope returned %s\n",buf);
			printf("STB test success after %d iterations\n",i+1);
			return 1;
		}
		usleep(10000); // wait 10 ms
	}
	sscope("*CLS\n");
	printf("STB test failed.\n");
	return 0;
}
static int test_wait_for_srq() {
	unsigned int stb;
	int ret;
	double delay;
	printf("\nTesting wait_for_srq ioctl\n");
 /* Set Mav mask and clear status */
	setReg(SRE, SRE_MAV);
	sscope("*CLS\n");
        showReg(STB,get_stb()); // clear srq if any
	ret = wait_for_srq(0);
	if (ret != -1 && errno != ETIMEDOUT) {
		printf("Expected timeout, wait_for_srq failed\n");
		return 0;
	}
	getTS();
	ret = wait_for_srq(1000);
	delay = getTS();
	if (ret != -1 || errno != ETIMEDOUT) {
		printf("Expected error %s, wait_for_srq failed \n",
		       strerror(errno));
		return 0;
	}
	if (delay < .9 || delay > 1.1) {
		printf("Expected timeout of 1s, got %f ret %d wait_for_srq failed\n",
		       delay, ret);
		return 0;
	}
	sscope("*IDN?\n");
	ret = wait_for_srq(500);
	if (!ret) {
		stb = get_srq_stb();
		if (stb & STB_MAV) {
			rscope(buf,MAX_BL);
		} else {
			printf("wait_for_srq failed.\n");
			return 0;
		}
	} else {
		printf("Unexpected return %d errno %d wait_for_srq_failed\n",
		       ret, errno);
		return 0;
	}
	sscope("*IDN?\n");
	ret = wait_for_srq(0xffffffffU);
	if (ret) {
		printf("Unexpected error, ret %d errno %d  wait_for_srq failed\n", ret, errno);
		return 0;
	}
	stb = get_srq_stb();
	if (stb & STB_MAV) {
			rscope(buf,MAX_BL);
			printf("Wait_for_srq Succeeded\n");
	} else {
			printf("wait_for_srq failed.\n");
			return 0;
	}
	return 1;
}

static int testSRQ() {
	unsigned int stb;
	int i;

        /* Test assertion and clearing of driver level SRQ */
	printf("\nTesting SRQ with read_stb\n");
 /* Set Mav mask and clear status */
	setReg(SRE, SRE_MAV);
	sscope("*CLS\n");
	printf("get_stb:"); showReg(STB,get_stb()); // clear srq
	printf("get_stb:"); showReg(STB,get_stb()); // and again
	getTS();
	for (i=0;i<100;i++) {
		sscope("*IDN?\n");
		wait_for_srq_poll();
		stb = get_stb();
		if (stb & STB_MAV) {
			//showReg(STB,stb);
			rscope(buf,MAX_BL);
			//printf("Measurement result is: %s\n",buf);
		} else {
			printf("SRQ testwith read_stb failed.\n");
			return 0;
		}
	}
	printf("SRQ with read_stb done: %11.6f\n" ,getTS());
	printf("\nTesting SRQ with get_srq_stb\n");
	printf("get_stb    :"); showReg(STB,get_stb());
	printf("get_stb    :"); showReg(STB,get_stb());
	printf("get_srq_stb:"); showReg(STB,get_srq_stb());
	printf("get_srq_stb:"); showReg(STB,get_srq_stb());
	getTS();
	for (i=0;i<100;i++) {
		sscope("*IDN?\n");
		wait_for_srq_poll();
		stb = get_srq_stb();
		if (stb & STB_MAV) {
			//showReg(STB,stb);
			rscope(buf,MAX_BL);
			//printf("Measurement result is: %s\n",buf);
		} else {
			printf("SRQ test with get_srq_stb failed.\n");
			return 0;
		}
	}
	printf("SRQ with get_srq_stb done: %11.6f " ,getTS());
	printf("SRQ test success\n");
	return 1;
}

int main () {
  int rv;
  unsigned int tmp,tmp1,ren,timeout;
  struct timeval sel_timeout;
  unsigned char caps;
  int len,n;
  unsigned char stb;
  char command;
  int oflags;
  fd_set fdsel[3];
  struct tm *daterec;
  time_t now;
  int i;

  /* Open file */
  if (0 > (fd = open("/dev/usbtmc0",O_RDWR))) {
	  perror("failed to open device");
	  exit(1);
  }

  /* Send device clear */
  if (0 != ioctl(fd,USBTMC_IOCTL_CLEAR)) {
	  perror("Dev clear ioctl failed");
	  exit(1);
  }

  /* Send clear status */
  sscope("*CLS\n");

  /* Send identity query */
  sscope("*IDN?\n");

  /* Read and print returned identity string */
  rscope(buf,MAX_BL);
  printf("*IDN? = %s\n",buf);

  /* set time */

  now = time(NULL);
  daterec = localtime(&now);
  snprintf(buf,MAX_BL,"SYST:DATE %d,%d,%d",daterec->tm_year+1900,daterec->tm_mon+1,daterec->tm_mday);
  sscope(buf);
  snprintf(buf,MAX_BL,"SYST:TIME %d,%d,%d",daterec->tm_hour,daterec->tm_min,daterec->tm_sec);
  sscope(buf);

  sscope("DISP:ANN:TEXT \"USBTMC Driver Test\"");

  printf("Testing setting and getting timeout\n");

  if (0 != ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&timeout)) {
	  perror("Get timeout ioctl failed");
	  exit(1);
  }
  printf("usb default timeout is %ums\n",timeout);

  timeout = 0xffffffffU;
  printf("Trying timeout > INT_MAX...\n");
  if (0 != ioctl(fd,USBTMC_IOCTL_SET_TIMEOUT,&timeout)) {
	  printf("... failed %s\n",strerror(errno));
  } else {
	  ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&timeout);
	  printf(" timeout is now %ums\n",timeout);
  }

  timeout = 0;
  printf("Trying timeout 0 ...\n");
  if (0 != ioctl(fd,USBTMC_IOCTL_SET_TIMEOUT,&timeout)) {
	  printf("... failed %s\n",strerror(errno));
  }  else {
	  ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&timeout);
	  printf(" timeout is now %ums\n",timeout);
  }

  timeout = 5000;
  printf("Trying timeout 5000 ...\n");
  if (0 != ioctl(fd,USBTMC_IOCTL_SET_TIMEOUT,&timeout)) {
	  printf("... failed %s\n",strerror(errno));
  }
  ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&timeout);
	  printf(" timeout is now %ums\n",timeout);

  /* Get and display instrument capabilities */
  if (0 != ioctl(fd,USBTMC488_IOCTL_GET_CAPS,&caps)) {
	  perror("get caps ioctl failed");
	  exit(1);
  }
  show_caps(caps);

  // sscope("*RST\n");
  sscope(":WGEN:FUNC SIN;OUTP 1;FREQ 1000;VOLT 0.5\n");
  sscope(":RUN\n");
  sscope(":AUTOSCALE\n");

  while (1) {
    printf("Enter command: [I]nteractive,  [T]est, [S]tb, s[R]q, [Q]uit:");
    fflush(stdout);

    len = read(0,buf,MAX_BL);
    if (len==0) command = 'q';
    else    command = buf[0];
    switch (command) {

    case 'I':
    case 'i':
      {
        int prompt = 1;
	int srun   = 1;
	int esr;
	int had_prompt = 0;
	/* Report all errors */
	setReg(ESE, ESR_CME | ESR_EXE | ESR_DDE | ESR_QYE);
	setReg(SRE, SRE_MAV |  SRE_ESB);
	showReg(ESE, getReg(ESE));
	showReg(ESR, getReg(ESR));
	showReg(SRE, getReg(SRE));
	showReg(STB, get_stb());
	sscope("*CLS\n");
	printf("Enter interactive mode, send Ctrl-D (EOF) to exit\n");
	while (srun) {
		if (prompt) {
			printf("Enter string to send: ");
			fflush(stdout);
			prompt = 0;
			had_prompt = 1;
		}

		memset(fdsel,0,sizeof(fdsel)); /* zero out select mask */
		get_stb(); // reset SRQ condition
		sel_timeout.tv_sec = 0;
		sel_timeout.tv_usec = 500000;
		FD_SET(0,&fdsel[0]);
		FD_SET(fd,&fdsel[2]);
		n = select(fd+1,
			(fd_set *)(&fdsel[0]),
			(fd_set *)(&fdsel[1]),
			(fd_set *)(&fdsel[2]),
			&sel_timeout);
		if (n < 0) {
			perror("select\n");
			break;
		}

		if (!n && !had_prompt) {
			prompt = 1;
			continue;
		}

		if (FD_ISSET(fd,&fdsel[2])) {
			while (STB_MAV & get_stb()) {
				if (0 < rscope(buf,MAX_BL)) printf("%s",buf);
				else break;
			}
			prompt = 1;
			while (STB_ESB & get_stb()) {
				esr = getReg(ESR);
				if (esr) {
					// showReg(ESR, esr);
					sscope(":SYST:ERR?\n");
					rscope(buf,MAX_BL);
					printf("Error: %s",buf);
				}
			}
		}

		if (FD_ISSET(0,&fdsel[0])) {
			len = read(0,buf,MAX_BL-1);
			had_prompt = 0;
			if (len > 0) {
				buf[len] = 0;
				sscope(buf);
			} else {
				setReg(SRE,0);
				printf("\nExit interactive mode\n");
				break;
			}
		}
	}
      }
      break;

    case 'R':
    case 'r':
	    testSRQ();
	    break;

    case 'S':
    case 's':
	    testSTB();
	    break;

    case 'T':
    case 't':
      getTS(); /* initialise time stamp */

    teom:
      { unsigned char eom = 0;
	int res;
	printf("\nTesting eom\n");
	/* Set OPC mask and clear status */
	sscope("*CLS\n");
	setReg(ESE, ESR_OPC); /* Set ESB in STB on OPC */
	setReg(SRE, SRE_ESB); // enable srq on event status reg
	showReg(STB,get_stb()); // clear srq
	ioctl(fd,USBTMC_IOCTL_EOM_ENABLE,&eom);
	sscope(":MEAS:FREQ?;VR");
	eom = 1;
	ioctl(fd,USBTMC_IOCTL_EOM_ENABLE,&eom);
	sscope("MS?;VPP? CHAN1;*OPC\n");
	wait_for_srq_poll();
	res = rscope(buf,MAX_BL);
	if (res < 0) printf("eom failed\n");
	else printf("eom success, res: %s",buf);
      }

    teom_in:
      { unsigned char attr=0;
        int len;
	printf("\nTesting eom on input\n");
	sscope("*CLS\n");
	setReg(ESE, 0);
	setReg(SRE, 0);
	sscope("*IDN?\n");
	while (!attr) {
		len = rscope(buf, 4);
		if (len < 0) {
			printf("teom_in failed\n");
			break;
		}
		buf[len] = 0;
		printf(buf);
		ioctl(fd, USBTMC_IOCTL_MSG_IN_ATTR, &attr);
	}
	if (len >= 0) printf("teom_in succeeded\n");
      }

    ttermc:
      { struct usbtmc_termchar termc;
	int part=1;
	unsigned char old_termchar;
	int res,len,old_termc_enabled;
	printf("\nTesting TermChar\n");

	old_termchar = '\n';
	old_termc_enabled = 0;
	printf("Old: TermChar 0x%02x, TermCharEnabled %d\n",
	       (int)old_termchar,old_termc_enabled);
	termc.term_char = ';';
	termc.term_char_enabled = 1;
	res = ioctl(fd,USBTMC_IOCTL_CONFIG_TERMCHAR,&termc);
	if (res < 0) {
	  perror("setting termchar config failed");
	  goto ttermc_out;
	}
	printf("New: TermChar 0x%02x, TermCharEnabled %d\n",
	       (int)termc.term_char,termc.term_char_enabled);
	setReg(ESE, ESR_OPC); /* Set ESB in STB on OPC */
	setReg(SRE, SRE_ESB); // enable srq on event status reg
	showReg(STB,get_stb()); // clear srq
	sscope("*CLS\n");
	sscope(":MEAS:FREQ?;VRMS?;VPP? CHAN1;*OPC\n");
	wait_for_srq_poll();
	while (STB_MAV & get_stb()) {
	  if (0 < (len = rscope(buf,MAX_BL)))
	    printf("termc part %d is %s\n",part++, buf);
	  else break;
	}
	// Put things back as they were
	termc.term_char = old_termchar;
	termc.term_char_enabled = old_termc_enabled;
	res = ioctl(fd,USBTMC_IOCTL_CONFIG_TERMCHAR,&termc);
	if (res < 0)
	  perror("restore termchar config failed");
      ttermc_out:
	  printf("TermChar test %s\n",((res >= 0) && (len >= 0)) ? "succeeded":"failed");
	if (part <=2 )
	  printf("However expected number of parts to be greater than 1\n");
      }

    tstb:

      testSTB();
      goto tsel; /* scope does not react to ren/llo */

    tren:

      printf("Testing remote disable, press enter to continue\n");
      wait_for_user();
      ren = 0;
      if (0 > ioctl(fd,USBTMC488_IOCTL_REN_CONTROL,&ren)) {
	perror("ren clear ioctl failed");
	exit(1);
      }
      printf("Remote disabled\n");
      sscope("SYSTEM:DSP \"USBTMC_488 driver Test\"");
      showReg(STB,get_stb()); // error ?

      printf("Testing Remote enable, press enter to continue\n");
      wait_for_user();
      ren = 1;
      if (ioctl(fd,USBTMC488_IOCTL_REN_CONTROL,&ren)) {
	perror("ren set ioctl failed");
	exit(1);
      }
      sscope("SYSTEM:DSP \"USBTMC_488 Remote enabled\"");

      printf("Testing local lockout, press enter to continue\n");
      wait_for_user();
      if (ioctl(fd,USBTMC488_IOCTL_LOCAL_LOCKOUT)) {
	perror("llo ioctl failed");
	exit(1);
      }
      sscope("SYSTEM:DSP \"USBTMC_488 Local locked\"");

      printf("Testing goto local, press enter to continue\n");
      wait_for_user();
      if (ioctl(fd,USBTMC488_IOCTL_GOTO_LOCAL)) {
	perror("gtl ioctl failed");
	exit(1);
      }
      sscope("SYSTEM:DSP \"\"");
      sscope("*CLS\n");

    tsel:
      memset(fdsel,0,sizeof(fdsel)); /* zero out select mask */
      printf("\nTesting select\n");
      /* Set OPC mask and clear status */
      sscope("*CLS\n");
      setReg(ESE, ESR_OPC); /* Set ESB in STB on OPC */
      setReg(SRE, SRE_ESB); // enable srq on event status reg
      showReg(STB,get_stb()); // clear srq
      sscope(":MEAS:FREQ?;VRMS?;VPP? CHAN1;*OPC\n");

      /* wait here for ESB srq */

      FD_SET(fd,&fdsel[2]);
      n = select(fd+1,
		 (fd_set *)(&fdsel[0]),
		 (fd_set *)(&fdsel[1]),
		 (fd_set *)(&fdsel[2]),
		 NULL);
      if (n <= 0) {
	perror("select\n");
	exit(1);
      }

      if (FD_ISSET(fd,&fdsel[2])) {
	showReg(STB,get_stb());
	rscope(buf,MAX_BL);
	printf("Measurement result is: %s\n",buf);
	printf("Select success\n");
      }

    async:
      printf("\nTesting Fasync notification\n");
      sscope(":TRIG:SOURCE CHAN1\n");
      signal(SIGIO, &srq_handler); /* dummy sample; sigaction( ) is better */
      fcntl(fd, F_SETOWN, getpid( ));
      oflags = fcntl(fd, F_GETFL);
      if (0 > fcntl(fd, F_SETFL, oflags | FASYNC)) {
	perror("fasync fail\n");
	exit(1);
      }
      sscope(":WAV:POINTS MAX\n");
      sscope(":TIM:MODE MAIN\n");
      sscope(":WAV:SOURCE CHAN1\n");
      /* enable OPC */
      sscope("*CLS\n");
      setReg(ESE, ESR_OPC); /* Set ESB in STB on OPC */
      setReg(SRE, SRE_ESB); // enable srq on event status reg
      showReg(STB, get_stb()); // clear srq
      flag = 0;
      sscope(":DIG CHAN1\n");
      sscope("*OPC\n");

      if (sleep(5)) {
	stb = get_stb();
	showReg(STB,stb);
	sscope(":WAV:POINTS?\n");
	rscope(buf,MAX_BL);
	printf("Points = %s",buf);
	printf("Fasync %s\n",flag?"success":"failed");
      } else printf("Fail\n");

    trigger:
      printf("\nTesting trigger ioctl\n");
      sscope(":TRIG:SOURCE EXT\n");
      sscope("*CLS\n");   // clear all status
      setReg(SRE, SRE_TRG); // enable SRQ on trigger
      sscope(":DIG CHAN1\n");
      showReg(STB,get_stb());
//      showTER();
      if (ioctl(fd,USBTMC488_IOCTL_TRIGGER)) {
	perror("trigger ioctl failed");
	exit(1);
      }

      wait_for_srq_poll();
      stb = get_srq_stb();
      showReg(STB, stb);
      printf("trigger ioctl test %s\n", (stb & STB_TRG) ? "success" : "failure");
      sscope(":TRIG:SOURCE CHAN1\n");

    twfsrq:
      test_wait_for_srq();

      sscope("*CLS\n");
      sscope(":RUN\n");
      break;
    case 'Q':
    case 'q':
      sscope("DISP:ANN:TEXT \"\"");
      printf("ttmc: /done\n");
      close(fd);
      exit(0);
    default: printf("%c : unknown command\n",command);
      break;
    }
  }
}
