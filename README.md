# linux-usbtmc driver

This is an experimental linux driver for usb test measurement &
control instruments. It adds support for missing functions in
USBTMC-USB488 spec, the ability to handle SRQ notifications with
fasync or poll/select and a number of features required to support the
IVI library. This package is provided for folks wanting to test or
use driver features not yet supported by the standard usbtmc driver in
their kernel.

The following functions have not yet been incorporated into
a kernel.org release:
 - module params
 - 32 bit support for IVI USBTMC_IOCTL_CTRL_REQUEST and USBTMC_IOCTL__READ/WRITE on 64 bit platforms.
 
Note: The initial version incorporated into the 14.6.0 kernel release used POLLIN / select readfds for SRQ notifications. This was changed to POLLPRI / select exceptfds in the version incorporated in the 4.19.0 and subsequent kernel release.
 
All the USBTMC-USB488 features are available in the standard kernel.org releases >= 4.19.0

The IVI extensions have been incorporated into kernel releases >= 4.20.0

For details on the IVI extensions please see Guido Kiener's [repo](https://github.com/GuidoKiener/linux-usbtmc)

## Installation

Prerequisite: You need a prebuilt kernel with the configuration and
kernel header files that were used to build it. Most distros have a
"kernel headers" package for this

To obtain the driver source files either clone the repo with
`git clone https://github.com/dpenkler/linux-usbtmc.git linux-usbtmc`
or download the zip file and extract the zip file to a directory linux-usbtmc

To build the driver simply run `make` in the directory containing the
driver source code (linux-usbtmc/ or linux-usbtmc-master/).

To install the driver run `make install` as root.

To load the driver execute `rmmod usbtmc; insmod usbtmc.ko` as root.

Enable debug messages with insmod usbtmc.ko dyndbg=+p and use dmesg to see debug output.

To compile your instrument control program ensure that it includes the
tmc.h file from this repo. An example test program for an
Agilent/Keysight scope is also provided. See the file ttmc.c
To build the provided program run `make ttmc`

To clean the directory of build files run `make clean`

To run usbtmc applications as non-root, insert a file e.g. /etc/udev/rules.d/99-usbtmc.rules with the content:

`KERNEL=="usbtmc[0-9]*", MODE="0660", GROUP="usbtmc"`

and add yourself to the usbtmc group

`sudo usermod -G usbtmc -a LOGIN`

where LOGIN is your username. 

## Features

The new features supported by this driver are based on the
specifications contained in the following document from the USB
Implementers Forum, Inc.

    Universal Serial Bus
    Test and Measurement Class,
    Subclass USB488 Specification
    (USBTMC-USB488)
    Revision 1.0
    April 14, 2003

Individual feature descriptions:

### ioctl to support the USBTMC-USB488 READ_STATUS_BYTE operation.

USBTMC488_IOCTL_READ_STB

When performing a read on an instrument that is executing
a function that runs longer than the USB timeout the instrument may
hang and require a device reset to recover. The READ_STATUS_BYTE
operation always returns even when the instrument is busy, permitting
the application to poll for the appropriate condition without blocking
as would  be the case with an "*STB?" query.

USBTMC488_IOCTL_READ_STB always reads the STB from the device and if
the SRQ condition is asserted in the driver it sets the RQS bit in the
returned STB.
   
Note: The READ_STATUS_BYTE ioctl clears the SRQ condition in the
driver but it has no effect on the status byte of the device.


### Support for receiving USBTMC-USB488 SRQ notifications with fasync

By configuring an instrument's service request enable register various
conditions can be reported via an SRQ notification.  When the FASYNC
flag is set on the file descriptor corresponding to the usb connected
instrument a SIGIO signal is sent to the owning process when the
instrument asserts a service request.

Example
```C
  signal(SIGIO, &srq_handler); /* dummy sample; sigaction( ) is better */
  fcntl(fd, F_SETOWN, getpid( ));
  oflags = fcntl(fd, F_GETFL);
  if (0 > fcntl(fd, F_SETFL, oflags | FASYNC)) {
	  perror("fcntl to set fasync failed\n");
	  exit(1);
  }
```

### Support for receiving USBTMC-USB488 SRQ notifications via poll/select

In many situations operations on multiple instruments need to be
synchronized. poll/select provide a convenient way of waiting on a
number of different instruments and other peripherals simultaneously.
When the instrument sends an SRQ notification the fd is notified of an
exceptional condition. To reset the poll/select condition either a
USBTMC488_IOCTL_READ_STB or USBTMC_IOCTL_GET_SRQ_STB must be
performed.

Example with select()

```C
  FD_SET(fd,&fdsel[0]);
  n = select(fd+1,
	  (fd_set *)(&fdsel[0]),
	  (fd_set *)(&fdsel[1]),
	  (fd_set *)(&fdsel[2]),
	  NULL);
  
  if (FD_ISSET(fd,&fdsel[2])) {
          ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)
	  if (stb & 16) { /* test for message available bit */
	      len = read(fd,buf,sizeof(buf));
	      /*
	      process buffer
	          ....
	      */
	  }
  }
```
Example with poll()

```C
/* Wait for SRQ using poll() */
void wait_for_srq(int fd) {
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLPRI;
	poll(&pfd,1,-1);
}
```

### USBTMC_IOCTL_GET_SRQ_STB

This ioctl, instead of requesting the STB from the device, returns the
STB that was sent by the device in the last SRQ message. If no other
SRQ occurs between two successive calls to USBTMC_IOCTL_GET_SRQ_STB an
ENOMSG error is signaled.

Note: The GET_SRQ_STB ioctl clears the SRQ condition in the driver but
it has no effect on the status byte of the device.

### USBTMC_IOCTL_GET_STB

This ioctl always reads the STB from the device and returns the
unmodified STB in the argument. It does not clear the SRQ condition in
the driver.

### New ioctls to enable and disable local controls on an instrument

These ioctls provide support for the USBTMC-USB488 control requests
for REN_CONTROL, GO_TO_LOCAL and LOCAL_LOCKOUT

### ioctl to cause a device to trigger

This is equivalent to the IEEE 488 GET (Group Execute Trigger) action.
While the "*TRG" command can be sent to perform the same operation,
in some situations an instrument will be busy and unable to process
the command immediately in which case the USBTMC488_IOCTL_TRIGGER can
be used. 

### Utility ioctl to retrieve USBTMC-USB488 capabilities

This is a convenience function to obtain an instrument's capabilities
from its file descriptor without having to access sysfs from the user
program. The driver encoded usb488 capability masks are defined in the
tmc.h include file.

### Two new module parameters

***io_buffer_size*** specifies the size of the buffer in bytes that is
used for usb bulk transfers. The default size is 2048. The minimum
size is 512. Values given for this parameter are automatically rounded
down to the nearest multiple of 4.

***usb_timeout*** specifies the timeout in milliseconds that is used
for usb transfers. The default value is 5000 and the minimum value is 500.

To set the parameters
```
insmod usbtmc.ko [io_buffer_size=nnn] [usb_timeout=nnn]
````
For example to set the buffer size to 256KB:
```
insmod usbtmc.ko io_buffer_size=262144
```

### ioctl's to set/get the usb timeout value

Separate ioctl's to set and get the usb timeout value for a device.
By default the timeout is set to 5000 milliseconds unless changed by
the ***usb_timeout*** module parameter.

USBTMC_IOCTL_SET_TIMEOUT will return with error EINVAL if timeout < 500

Example

```C
	unsigned int timeout, oldtimeout;
....
	ioctl(fd,USBTMC_IOCTL_GET_TIMEOUT,&oldtimeout)
	timeout = 1000;
	ioctl(fd,USBTMC_IOCTL_SET_TIMEOUT,&timeout)

```
### ioctl to send generic usb control requests

Allows user programs to send control messages to a device over the
control pipe.

### ioctl to control setting EOM bit

Enables or disables setting the EOM bit on write.
By default the EOM bit is set on the last transfer of a write.

Will return with error EINVAL if eom is not 0 or 1

Example

```C
	unsigned char eom;
....
	eom = 0; // disable setting of EOM bit on write 
	ioctl(fd,USBTMC_IOCTL_EOM_ENABLE,&eom)

```

### ioctl to configure TermChar and TermCharEnable

Allows enabling/disabling of terminating a read on reception of term_char.
By default TermCharEnabled is false and TermChar is '\n' (0x0a).

Will return with error EINVAL if term_char_enabled is not 0 or 1 or if
attempting to enable term_char when the device does not support terminating
a read when a byte matches the specified term_char.

Example

```C
	struct usbtmc_termc termc;
....
	termc.term_char_enabled = 1; // enable terminating reads on term_char
	termc.term_char = '\n';     
	ioctl(fd,USBTMC_IOCTL_CONFIG_TERMCHAR,&termc)

```


## Issues and enhancement requests

Use the [Issue](https://github.com/dpenkler/linux-usbtmc/issues) feature in github to post requests for enhancements or bugfixes.
