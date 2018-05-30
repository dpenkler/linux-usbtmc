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

/*
   Service request enable register (SRE) Bit definitions
 */

#define SRE_Trigger            1
#define SRE_User               2
#define SRE_Message            4
#define SRE_MessageAvailable  16
#define SRE_Event_Status      32
#define SRE_Operation_Status 128

/* need these defines to inline bit definitions in SRE command strings */
#define xstr(s) str(s)
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

static void show_stb(unsigned char stb) {
	int i;
	printf("%11.6f STB = ",getTS());
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
	//	printf("sscope: %s",msg);
}

void setSRE(int val) {
	char buf[32];
	snprintf(buf,32,"*SRE %d\n",val);
	sscope(buf);
}

/* Read string from scope */
int rscope(char *buf, int max_len) {
	int len = read(fd,buf,max_len);
	if (len < 0) {
	  perror("failed to read from scope");
	  ioctl(fd,USBTMC_IOCTL_CLEAR);
	}
	buf[len] = 0; /* zero terminate */
	return len;
}

/* Wait for SRQ using poll() */
void wait_for_srq() {
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLPRI;
	poll(&pfd,1,-1);
}

void wait_for_user() {
	char buf[8];
	read(0,buf,1);
}

#define MAX_BL 2048

int main () {
  int rv;
  unsigned int tmp,tmp1,ren,timeout;
  unsigned char caps;
  int len,n;
  unsigned char stb;
  char buf[MAX_BL],command;
  int oflags;
  fd_set fdsel[3];
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

  if (0 != ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&timeout)) {
	  perror("Get timeout ioctl failed");
	  exit(1);
  }
  printf("usb timeout is %ums\n",timeout);
  if (timeout != 1000) {
	  timeout = 1000;
	  printf("resetting timeout...");
	  if (0 != ioctl(fd,USBTMC_IOCTL_SET_TIMEOUT,&timeout)) {
		  perror("Set timeout ioctl failed");
		  exit(1);
	  }
	  ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&timeout);
	  printf(" timeout is now %ums\n",timeout);
  }

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
    printf("Enter command: [I]nteractive,  [T]est, [Q]uit:");
    fflush(stdout);

    read(0,buf,MAX_BL);
    command = buf[0];
    switch (command) {

    case 'I':
    case 'i':
      {
	int prompt = 1;
	int srun   = 1;
	setSRE(SRE_MessageAvailable);
	sscope("*CLS\n");
	printf("Enter interactive mode, send Ctrl-D (EOF) to exit\n");
	while (srun) {
	  if (prompt) {
	    printf("Enter string to send: ");
	    fflush(stdout);
	    prompt = 0;
	  }

	  memset(fdsel,0,sizeof(fdsel)); /* zero out select mask */

	  FD_SET(0,&fdsel[0]);
	  FD_SET(fd,&fdsel[2]);
	  n = select(fd+1,
		     (fd_set *)(&fdsel[0]),
		     (fd_set *)(&fdsel[1]),
		     (fd_set *)(&fdsel[2]),
		     NULL);
	  if (n <= 0) {
	    perror("select\n");
	    break;
	  }

	  if (FD_ISSET(fd,&fdsel[2])) {
	    while (STB_MAV & get_stb()) {
	      if (0 < rscope(buf,MAX_BL)) printf("%s",buf);
	      else break;
	    }
	    prompt = 1;
	  }

	  if (FD_ISSET(0,&fdsel[0])) {
	    len = read(0,buf,MAX_BL-1);
	    if (len > 0) {
	      buf[len] = 0;
	      sscope(buf);
	    } else {
	      setSRE(0);
	      printf("\nExit interactive mode\n");
	      break;
	    }
	  }
	}
      }
      break;

    case 'T':
    case 't':
      getTS(); /* initialise time stamp */

    teom:
      { unsigned char eom = 0;
	int res;
	printf("\nTesting eom\n");
	ioctl(fd,USBTMC_IOCTL_EOM_ENABLE,&eom);
	sscope(":MEAS:FREQ?;VR");
	eom = 1;
	ioctl(fd,USBTMC_IOCTL_EOM_ENABLE,&eom);
	sscope("MS?;VPP? CHAN1\n");
	res = rscope(buf,MAX_BL);
	if (res < 0) printf("eom failed\n");
	else printf("eom success, res: %s",buf);
      }

    ttermc:
      { struct usbtmc_termchar termc;
	int fdtc,fdtce;
	int part=1;
	unsigned char old_termchar,old_termce,termchar,termce;
	int res,len,old_termc_enabled,termc_enabled;
	printf("\nTesting TermChar\n");
	fdtc  = open("/sys/class/usbmisc/usbtmc0/device/TermChar",O_RDONLY);
	fdtce = open("/sys/class/usbmisc/usbtmc0/device/TermCharEnabled",O_RDONLY);
	read(fdtc,&old_termchar,1);
	read(fdtce,&old_termce,1);
	old_termc_enabled =  (old_termce == '0') ? 0 : 1;
	printf("Old: TermChar 0x%02x, TermCharEnabled %d\n",
	       (int)old_termchar,old_termc_enabled);
	termc.term_char = ';';
	termc.term_char_enabled = 1;
	ioctl(fd,USBTMC_IOCTL_CONFIG_TERMCHAR,&termc);
	lseek(fdtc,0,SEEK_SET);
	lseek(fdtce,0,SEEK_SET);
	read(fdtc,&termchar,1);
	read(fdtce,&termce,1);
	close(fdtc);
	close(fdtce);
	termc_enabled =  (termce == '0') ? 0 : 1;
	printf("New: TermChar x0%02x, TermCharEnabled %d\n",
		       (int)termchar,termc_enabled);
	setSRE(SRE_MessageAvailable);
	sscope("*CLS\n");
	sscope(":MEAS:FREQ?;VRMS?;VPP? CHAN1\n");
	sleep(1);
	while (STB_MAV & get_stb()) {
	  if (0 < (len = rscope(buf,MAX_BL)))
	    printf("termc part %d is %s\n",part++, buf);
	  else break;
	}
	// Put things back as they were 
	termc.term_char = old_termchar;
	termc.term_char_enabled = old_termc_enabled;
	res = ioctl(fd,USBTMC_IOCTL_CONFIG_TERMCHAR,&termc);
	if (res < 0) {
	  perror("config termchar");
	  res = 0;
	} else {
	  lseek(fdtc,0,SEEK_SET);
	  lseek(fdtce,0,SEEK_SET);
	  read(fdtc,&termchar,1);
	  read(fdtce,&termce,1);
	  close(fdtc);
	  close(fdtce);
	  res = (termchar==old_termchar) && (termce==old_termce);
	}
	printf("TermChar test %s\n",(len >= 0) ? "succeeded":"failed");
	if (part <=2 )
	  printf("However expected number of parts to be greater than 1\n");
      }

    tstb:

      /* Test read STB ioctl */
      printf("\nTesting stb ioctl\n");
      //  wait_for_user();

      tmp1 = get_stb();

      /* Use IEEE 488.2 *STB? command query to compare */
      sscope("*STB?\n");
      rscope(buf,MAX_BL);
      tmp = atoi(buf);

      if (tmp != tmp1) {
	fprintf(stderr,
		"Warning: SCPI status byte = %x, ioctl status byte = %x\n",
		tmp,tmp1);
	show_stb(tmp);
      }
      show_stb(tmp1);

      sscope("*CLS\n");  /* Clear status */
      //      sscope("*TST?\n"); /* Initiate long operation: self test */
      sscope(":MEAS:FREQ?;VRMS?;VPP? CHAN1\n");

      /* Poll  STB until MAV bit is set */
      while (1) {
	tmp = get_stb();
	if (tmp & STB_MAV) { /* Test for MAV */
	  show_stb(tmp);
	  rscope(buf,MAX_BL);
	  printf("stb test success. Scope returned %s",buf);
	  break;
	}
	usleep(10000); // wait 10 ms
      }

      goto tsel; // skip tren

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
      memset(fdsel,0,sizeof(fdsel)); /* zero out select mask */
      printf("\nTesting select\n");
      /* Set Mav mask and clear status */
      setSRE(SRE_MessageAvailable);
      sscope("*CLS\n");
      sscope(":MEAS:FREQ?;VRMS?;VPP? CHAN1\n");
      show_stb(get_stb()); // clear srq

      /* wait here for MAV */

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
	show_stb(get_stb());
	rscope(buf,MAX_BL);
	printf("Measurement result is: %s",buf);
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
      sscope("*ESE 1\n"); // set operation complete in the event status enable reg
      setSRE(SRE_Event_Status); // enable srq on event status reg
      show_stb(get_stb()); // clear srq
      flag = 0;
      sscope(":DIG CHAN1\n");
      sscope("*OPC\n");

      if (sleep(5)) {
	stb = get_stb();
	show_stb(stb);
	sscope(":WAV:POINTS?\n");
	rscope(buf,MAX_BL);
	printf("Points = %s",buf);
	printf("Fasync %s\n",flag?"success":"failed");
      } else printf("Fail\n");

    trigger:
      printf("\nTesting trigger ioctl\n");
      sscope(":TRIG:SOURCE EXT\n");
      sscope("*CLS\n");   // clear all status
      setSRE(SRE_Trigger); // enable SRQ on trigger
      sscope(":DIG CHAN1\n");
      show_stb(get_stb());

      if (ioctl(fd,USBTMC488_IOCTL_TRIGGER)) {
	perror("trigger ioctl failed");
	exit(1);
      }

      wait_for_srq();
      stb = get_stb();
      show_stb(stb);
      printf("trigger ioctl %s\n", (stb & STB_TRG) ? "success" : "failure");
      sscope("*CLS\n");
      sscope(":RUN;:AUTOSCALE\n");
      break;
    case 'Q':
    case 'q':
      printf("ttmc: /done\n");
      close(fd);
      exit(0);
    default: printf("%s : unknown command\n");
      break;
    }
  }
}
