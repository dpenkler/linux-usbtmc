/*
  Sample usbtmc programme to produce a screendump in png format
  from an SCPI compliant instrument.

  Reads from /dev/usbtmc0 and writes to a file screendump.png in
  the current directory. Reads are done in 1024 byte chunks. The
  ":DISP:DATA?" command may need to be adapted to your
  instrument.
*/

#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/usb/tmc.h>


int fd;
char *sdfn = "screendump.png";

/* Send string to scope */
void sscope(char *msg) {
	write(fd,msg,strlen(msg));
}

/* Read string from scope */
int rscope(char *buf, int max_len) {
	int len = read(fd,buf,max_len);
	if (len < 0) {
		perror("Ffailed to read from scope");
		ioctl(fd,USBTMC_IOCTL_CLEAR);
		return 0;
	}
	buf[len] = 0; /* zero terminate */
	return len;
}


int main () {
	int len,n,count,rlen,done;
	int sfd;
	char buf[256],*gbuf;
	unsigned char stb;

	/* Open instrument file */
	if (0 > (fd = open("/dev/usbtmc0",O_RDWR))) {
		perror("Failed to open device");
		exit(1);
	}

	/* Send device clear */
	if (0 != ioctl(fd,USBTMC_IOCTL_CLEAR)) {
		perror("Device clear ioctl failed");
		goto out;
	}

	sscope("*CLS\n");   // clear status regs
	// set operation complete bit in the Event Status Enable register
	sscope("*ESE 1\n");

	//sscope("SYST:MENU OFF\n");  // turn off soft keys
	//sscope("HARD:INKS 0\n"); // no colour inversion

	//sscope(":DISP:DATA? ON, OFF, PNG;*OPC\n"); // For Rigol scope
	sscope(":DISP:DATA? PNG, COL;*OPC\n"); // For Keysight scope

	rscope(buf,2); // read first 2 bytes of header block
	if (buf[0] != '#') { /* Check that we have a valid header */
		fprintf(stderr, "invalid IEEE488 # binary block header\n");
		goto out;
	}
	n = buf[1] - 48; // get the number of digits in data length
	if (n < 2 || n > 9) {
		fprintf(stderr, "Invalid block length in IEEE488 BB header\n");
		goto out;
	}
	rscope(buf,n);	// read data length
	len = atoi(buf);

	// check status byte for operation complete i.e. ESB set
	if (0 != ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)) {
		perror("Read stb ioctl failed");
		goto out;
	}
	n = 0;
	printf("Waiting for OPC\n");
	while (!(stb & 32)) { /* wait for ESB */
		n++;
		if (n==10) {
			fprintf(stderr,"Timed out waiting for screen dump\n");
			goto out;
		}
		sleep(1);
		if (0 != ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)) {
			perror("Read stb ioctl failed");
			goto out;
		}
	}
	printf("Reading %d bytes of display data\n",len);
	gbuf = malloc(len + 1); // +1 for null termination in rscope
	count = len;
	rlen  = 1024;
	done  = 0;
	while (count > 0) {
		if (count < rlen) rlen = count;
		n = rscope(&gbuf[done], rlen);
		//    printf("%d bytes read\n", n);
		if (n != rlen) {
			fprintf(stderr, "Short read \n");
			goto out;
		}
		done  += n;
		count -= n;
	}
	printf("Total %d bytes read\n",done);
	rscope(buf,1); // read last byte (linefeed)
	if (buf[0] != '\n') fprintf(stderr,"Expected newline at the end\n");

	sfd = open(sdfn, O_RDWR | O_CREAT, 0660); // open png file
	if (sfd < 0) {
		fprintf(stderr,"Failed to open %s\n",sdfn);
		goto out;
	}
	write(sfd, gbuf, len);	// write png data
	close(sfd);

	printf("Screen dumped to %s\n",sdfn);
out:
	close(fd);
}
