linux-usbtmc driver
-------------------

This is an experimental linux driver for usb test measurement &
control instruments that adds support for missing functions in
USBTMC-USB488 spec and the ability to handle SRQ notifications with
fasync or poll/select. Most of the functions have been incorporated in
the linux kernel starting with version 4.6.  This package is provided
for folks wanting to test or use driver features not yet supported by
the standard usbtmc driver in their kernel.

Presently only the trigger ioctl is not available in the standard
kernel.org releases >= 4.6.

Installation
------------

Prerequisite: You need a prebuilt kernel with the configuration and
kernel header files that were used to build it. Most distros have a
"kernel headers" package for this.

To build the driver simply run "make" in the directory containing the
driver source code.

To install the driver run "make modules_install" as root.

To load the driver execute "rmmod usbtmc; modprobe usbtmc" as root.

To compile your instrument control program ensure that it includes the
tmc.h file from this repo. An example test program for an
Agilent/Keysight scope is also provided. See the file ttmc.c


Features
--------

The new features supported by this driver are based on the
specifications contained in the following document from the USB
Implementers Forum, Inc.

    Universal Serial Bus
    Test and Measurement Class,
    Subclass USB488 Specification
    (USBTMC-USB488)
    Revision 1.0
    April 14, 2003

## Individual feature descriptions

### ioctl to support the USMTMC-USB488 READ_STATUS_BYTE operation.


When performing a read on an instrument that is executing
a function that runs longer than the USB timeout the instrument may
hang and require a device reset to recover. The READ_STATUS_BYTE
operation always returns even when the instrument is busy, permitting
the application to poll for the appropriate condition without blocking
as would  be the case with an "*STB?" query.

Note: The READ_STATUS_BYTE ioctl clears the SRQ condition but it has no effect
on the status byte of the device.


### Support for receiving USBTMC USB488 SRQ notifications with fasync

By configuring an instrument's service request enable register various
conditions can be reported via an SRQ notification.  When the FASYNC
flag is set on the file descriptor corresponding to the usb connected
instrument a SIGIO signal is sent to the owning process when the
instrument asserts a service request.

Example

...C
  signal(SIGIO, &srq_handler); /* dummy sample; sigaction( ) is better */
  fcntl(fd, F_SETOWN, getpid( ));
  oflags = fcntl(fd, F_GETFL);
  if (0 > fcntl(fd, F_SETFL, oflags | FASYNC)) {
	  perror("fcntl to set fasync failed\n");
	  exit(1);
  }
...

### Support for receiving USBTMC USB488 SRQ notifications via poll/select

In many situations operations on multiple instruments need to be
synchronized. poll/select provide a convenient way of waiting on a
number of different instruments and other peripherals simultaneously.
When the instrument sends an SRQ notification the fd becomes readable.
If the MAV (message available) event is enabled the normal semantic of
poll/select on a readable file descriptor is achieved. However many
other conditions can be set to cause SRQ. To reset the poll/select
condition a READ_STATUS_BYTE ioctl must be performed.

Example

...C
  FD_SET(fd,&fdsel[0]);
  n = select(fd+1,
	  (fd_set *)(&fdsel[0]),
	  (fd_set *)(&fdsel[1]),
	  (fd_set *)(&fdsel[2]),
	  NULL);
  
  if (FD_ISSET(fd,&fdsel[0])) {
          ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)
	  if (stb & 16) { /* test for message available bit */
	      len = read(fd,buf,sizeof(buf));
	      /*
	      process buffer
	          ....
	      */
	  }
  }
...


### New ioctls to enable and disable local controls on an instrument

These ioctls provide support for the USBTMC-USB488 control requests
for REN_CONTROL, GO_TO_LOCAL and LOCAL_LOCKOUT

### ioctl to cause a device to trigger

This is equivalent to the IEEE 488 GET (Group Execute Trigger) action.
While a the "*TRG" command can be sent to perform the same operation,
in some situations an instrument will be busy and unable to process
the command immediately in which case the USBTMC488_IOCTL_TRIGGER can
be used. 

### Utility ioctl to retrieve USBTMC-USB488 capabilities

This is a convenience function to obtain an instrument's capabilities
from its file descriptor without having to access sysfs from the user
program. The driver encoded usb488 capability masks are defined in the
tmc.h include file.
