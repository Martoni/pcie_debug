/* pci_debug.c
 *
 * 6/21/2010 D. W. Hawkins
 *
 * PCI debug registers interface.
 *
 * This tool provides a debug interface for reading and writing
 * to PCI registers via the device base address registers (BARs).
 * The tool uses the PCI resource nodes automatically created
 * by recently Linux kernels.
 *
 * The readline library is used for the command line interface
 * so that up-arrow command recall works. Command-line history
 * is not implemented. Use -lreadline -lcurses when building.
 *
 * ----------------------------------------------------------------
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <byteswap.h>

/* Readline support */
#include <readline/readline.h>
#include <readline/history.h>

/* PCI device */
typedef struct {
	/* Base address region */
	unsigned int bar;

	/* Slot info */
	unsigned int domain;
	unsigned int bus;
	unsigned int slot;
	unsigned int function;

	/* Resource filename */
	char         filename[100];

	/* File descriptor of the resource */
	int          fd;

	/* Memory mapped resource */
	unsigned char *maddr;
	unsigned int   size;
	unsigned int   offset;

	/* PCI physical address */
	unsigned int   phys;

	/* Address to pass to read/write (includes offset) */
	unsigned char *addr;
} device_t;

void display_help(device_t *dev);
void parse_command(device_t *dev);
int process_command(device_t *dev, char *cmd);
int change_mem(device_t *dev, char *cmd);
int fill_mem(device_t *dev, char *cmd);
int display_mem(device_t *dev, char *cmd);
int change_endian(device_t *dev, char *cmd);

/* Endian read/write mode */
static int big_endian = 0;

/* Low-level access functions */
static void
write_8(
	device_t     *dev,
	unsigned int  addr,
	unsigned char data);

static unsigned char
read_8(
	device_t    *dev,
	unsigned int addr);

static void
write_le16(
	device_t          *dev,
	unsigned int       addr,
	unsigned short int data);

static unsigned short int
read_le16(
	device_t    *dev,
	unsigned int addr);

static void
write_be16(
	device_t          *dev,
	unsigned int       addr,
	unsigned short int data);

static unsigned short int
read_be16(
	device_t    *dev,
	unsigned int addr);

static void
write_le32(
	device_t    *dev,
	unsigned int addr,
	unsigned int data);

static unsigned int
read_le32(
	device_t    *dev,
	unsigned int addr);

static void
write_be32(
	device_t    *dev,
	unsigned int addr,
	unsigned int data);

static unsigned int
read_be32(
	device_t    *dev,
	unsigned int addr);

/* Usage */
static void show_usage()
{
	printf("\nUsage: pci_debug -s <device>\n"\
		 "  -h            Help (this message)\n"\
		 "  -s <device>   Slot/device (as per lspci)\n" \
		 "  -b <BAR>      Base address region (BAR) to access, eg. 0 for BAR0\n\n");
}

int main(int argc, char *argv[])
{
	int opt;
	char *slot = 0;
	int status;
	struct stat statbuf;
	device_t device;
	device_t *dev = &device;

	/* Clear the structure fields */
	memset(dev, 0, sizeof(device_t));

	while ((opt = getopt(argc, argv, "b:hs:")) != -1) {
		switch (opt) {
			case 'b':
				/* Defaults to BAR0 if not provided */
				dev->bar = atoi(optarg);
				break;
			case 'h':
				show_usage();
				return -1;
			case 's':
				slot = optarg;
				break;
			default:
				show_usage();
				return -1;
		}
	}
	if (slot == 0) {
		show_usage();
		return -1;
	}

	/* ------------------------------------------------------------
	 * Open and map the PCI region
	 * ------------------------------------------------------------
	 */

	/* Extract the PCI parameters from the slot string */
	status = sscanf(slot, "%2x:%2x.%1x",
			&dev->bus, &dev->slot, &dev->function);
	if (status != 3) {
		printf("Error parsing slot information!\n");
		show_usage();
		return -1;
	}

	/* Convert to a sysfs resource filename and open the resource */
	snprintf(dev->filename, 99, "/sys/bus/pci/devices/%04x:%02x:%02x.%1x/resource%d",
			dev->domain, dev->bus, dev->slot, dev->function, dev->bar);
	dev->fd = open(dev->filename, O_RDWR | O_SYNC);
	if (dev->fd < 0) {
		printf("Open failed for file '%s': errno %d, %s\n",
			dev->filename, errno, strerror(errno));
		return -1;
	}

	/* PCI memory size */
	status = fstat(dev->fd, &statbuf);
	if (status < 0) {
		printf("fstat() failed: errno %d, %s\n",
			errno, strerror(errno));
		return -1;
	}
	dev->size = statbuf.st_size;

	/* Map */
	dev->maddr = (unsigned char *)mmap(
		NULL,
		(size_t)(dev->size),
		PROT_READ|PROT_WRITE,
		MAP_SHARED,
		dev->fd,
		0);
	if (dev->maddr == (unsigned char *)MAP_FAILED) {
//		printf("failed (mmap returned MAP_FAILED)\n");
		printf("BARs that are I/O ports are not supported by this tool\n");
		dev->maddr = 0;
		close(dev->fd);
		return -1;
	}

	/* Device regions smaller than a 4k page in size can be offset
	 * relative to the mapped base address. The offset is
	 * the physical address modulo 4k
	 */
	{
		char configname[100];
		int fd;

		snprintf(configname, 99, "/sys/bus/pci/devices/%04x:%02x:%02x.%1x/config",
				dev->domain, dev->bus, dev->slot, dev->function);
		fd = open(configname, O_RDWR | O_SYNC);
		if (dev->fd < 0) {
			printf("Open failed for file '%s': errno %d, %s\n",
				configname, errno, strerror(errno));
			return -1;
		}

		status = lseek(fd, 0x10 + 4*dev->bar, SEEK_SET);
		if (status < 0) {
			printf("Error: configuration space lseek failed\n");
			close(fd);
			return -1;
		}
		status = read(fd, &dev->phys, 4);
		if (status < 0) {
			printf("Error: configuration space read failed\n");
			close(fd);
			return -1;
		}
		dev->offset = ((dev->phys & 0xFFFFFFF0) % 0x1000);
		dev->addr = dev->maddr + dev->offset;
		close(fd);
	}


	/* ------------------------------------------------------------
	 * Tests
	 * ------------------------------------------------------------
	 */

	printf("\n");
	printf("PCI debug\n");
	printf("---------\n\n");
	printf(" - accessing BAR%d\n", dev->bar);
	printf(" - region size is %d-bytes\n", dev->size);
	printf(" - offset into region is %d-bytes\n", dev->offset);

	/* Display help */
	display_help(dev);

	/* Process commands */
	parse_command(dev);

	/* Cleanly shutdown */
	munmap(dev->maddr, dev->size);
	close(dev->fd);
	return 0;
}

void
parse_command(
	device_t *dev)
{
	char *line;
	int len;
	int status;

	while(1) {
		line = readline("PCI> ");
		/* Ctrl-D check */
		if (line == NULL) {
			printf("\n");
			continue;
		}
		/* Empty line check */
		len = strlen(line);
		if (len == 0) {
			continue;
		}
		/* Process the line */
		status = process_command(dev, line);
		if (status < 0) {
			break;
		}

		/* Add it to the history */
		add_history(line);
		free(line);
	}
	return;
}

/*--------------------------------------------------------------------
 * User interface
 *--------------------------------------------------------------------
 */
void
display_help(
	device_t *dev)
{
	printf("\n");
	printf("  ?                         Help\n");
	printf("  d[width] addr len         Display memory starting from addr\n");
	printf("                            [width]\n");
	printf("                              8   - 8-bit access\n");
	printf("                              16  - 16-bit access\n");
	printf("                              32  - 32-bit access (default)\n");
	printf("  c[width] addr val         Change memory at addr to val\n");
	printf("  e                         Print the endian access mode\n");
	printf("  e[mode]                   Change the endian access mode\n");
	printf("                            [mode]\n");
	printf("                              b - big-endian (default)\n");
	printf("                              l - little-endian\n");
	printf("  f[width] addr val len inc  Fill memory\n");
	printf("                              addr - start address\n");
	printf("                              val  - start value\n");
	printf("                              len  - length (in bytes)\n");
	printf("                              inc  - increment (defaults to 1)\n");
	printf("  q                          Quit\n");
	printf("\n  Notes:\n");
	printf("    1. addr, len, and val are interpreted as hex values\n");
	printf("       addresses are always byte based\n");
	printf("\n");
}

int process_command(device_t *dev, char *cmd)
{
	if (cmd[0] == '\0') {
		return 0;
	}
	switch (cmd[0]) {
		case '?':
			display_help(dev);
			break;
		case 'c':
		case 'C':
			return change_mem(dev, cmd);
		case 'd':
		case 'D':
			return display_mem(dev, cmd);
		case 'e':
		case 'E':
			return change_endian(dev, cmd);
		case 'f':
		case 'F':
			return fill_mem(dev, cmd);
		case 'q':
		case 'Q':
			return -1;
		default:
			break;
	}
	return 0;
}

int display_mem(device_t *dev, char *cmd)
{
	int width = 32;
	int addr = 0;
	int len = 0;
	int status;
	int i;
	unsigned char d8;
	unsigned short d16;
	unsigned int d32;

	/* d, d8, d16, d32 */
	if (cmd[1] == ' ') {
		status = sscanf(cmd, "%*c %x %x", &addr, &len);
		if (status != 2) {
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			return 0;
		}
	} else {
		status = sscanf(cmd, "%*c%d %x %x", &width, &addr, &len);
		if (status != 3) {
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			return 0;
		}
	}
	if (addr > dev->size) {
		printf("Error: invalid address (maximum allowed is %.8X\n", dev->size);
		return 0;
	}
	/* Length is in bytes */
	if ((addr + len) > dev->size) {
		/* Truncate */
		len = dev->size;
	}
	switch (width) {
		case 8:
			for (i = 0; i < len; i++) {
				if ((i%16) == 0) {
					printf("\n%.8X: ", addr+i);
				}
				d8 = read_8(dev, addr+i);
				printf("%.2X ", d8);
			}
			printf("\n");
			break;
		case 16:
			for (i = 0; i < len; i+=2) {
				if ((i%16) == 0) {
					printf("\n%.8X: ", addr+i);
				}
				if (big_endian == 0) {
					d16 = read_le16(dev, addr+i);
				} else {
					d16 = read_be16(dev, addr+i);
				}
				printf("%.4X ", d16);
			}
			printf("\n");
			break;
		case 32:
			for (i = 0; i < len; i+=4) {
				if ((i%16) == 0) {
					printf("\n%.8X: ", addr+i);
				}
				if (big_endian == 0) {
					d32 = read_le32(dev, addr+i);
				} else {
					d32 = read_be32(dev, addr+i);
				}
				printf("%.8X ", d32);
			}
			printf("\n");
			break;
		default:
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			break;
	}
	printf("\n");
	return 0;
}

int change_mem(device_t *dev, char *cmd)
{
	int width = 32;
	int addr = 0;
	int status;
	unsigned char d8;
	unsigned short d16;
	unsigned int d32;

	/* c, c8, c16, c32 */
	if (cmd[1] == ' ') {
		status = sscanf(cmd, "%*c %x %x", &addr, &d32);
		if (status != 2) {
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			return 0;
		}
	} else {
		status = sscanf(cmd, "%*c%d %x %x", &width, &addr, &d32);
		if (status != 3) {
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			return 0;
		}
	}
	if (addr > dev->size) {
		printf("Error: invalid address (maximum allowed is %.8X\n", dev->size);
		return 0;
	}
	switch (width) {
		case 8:
			d8 = (unsigned char)d32;
			write_8(dev, addr, d8);
			break;
		case 16:
			d16 = (unsigned short)d32;
			if (big_endian == 0) {
				write_le16(dev, addr, d16);
			} else {
				write_be16(dev, addr, d16);
			}
			break;
		case 32:
			if (big_endian == 0) {
				write_le32(dev, addr, d32);
			} else {
				write_be32(dev, addr, d32);
			}
			break;
		default:
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			break;
	}
	return 0;
}

int fill_mem(device_t *dev, char *cmd)
{
	int width = 32;
	int addr = 0;
	int len = 0;
	int inc = 0;
	int status;
	int i;
	unsigned char d8;
	unsigned short d16;
	unsigned int d32;

	/* c, c8, c16, c32 */
	if (cmd[1] == ' ') {
		status = sscanf(cmd, "%*c %x %x %x %x", &addr, &d32, &len, &inc);
		if ((status != 3) && (status != 4)) {
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			return 0;
		}
		if (status == 3) {
			inc = 1;
		}
	} else {
		status = sscanf(cmd, "%*c%d %x %x %x %x", &width, &addr, &d32, &len, &inc);
		if ((status != 3) && (status != 4)) {
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			return 0;
		}
		if (status == 4) {
			inc = 1;
		}
	}
	if (addr > dev->size) {
		printf("Error: invalid address (maximum allowed is %.8X\n", dev->size);
		return 0;
	}
	/* Length is in bytes */
	if ((addr + len) > dev->size) {
		/* Truncate */
		len = dev->size;
	}
	switch (width) {
		case 8:
			for (i = 0; i < len; i++) {
				d8 = (unsigned char)(d32 + i*inc);
				write_8(dev, addr+i, d8);
			}
			break;
		case 16:
			for (i = 0; i < len/2; i++) {
				d16 = (unsigned short)(d32 + i*inc);
				if (big_endian == 0) {
					write_le16(dev, addr+2*i, d16);
				} else {
					write_be16(dev, addr+2*i, d16);
				}
			}
			break;
		case 32:
			for (i = 0; i < len/4; i++) {
				if (big_endian == 0) {
					write_le32(dev, addr+4*i, d32 + i*inc);
				} else {
					write_be32(dev, addr+4*i, d32 + i*inc);
				}
			}
			break;
		default:
			printf("Syntax error (use ? for help)\n");
			/* Don't break out of command processing loop */
			break;
	}
	return 0;
}

int change_endian(device_t *dev, char *cmd)
{
	char endian = 0;
	int status;

	/* e, el, eb */
	status = sscanf(cmd, "%*c%c", &endian);
	if (status < 0) {
		/* Display the current setting */
		if (big_endian == 0) {
			printf("Endian mode: little-endian\n");
		} else {
			printf("Endian mode: big-endian\n");
		}
		return 0;
	} else if (status == 1) {
		switch (endian) {
			case 'b':
				big_endian = 1;
				break;
			case 'l':
				big_endian = 0;
				break;
			default:
				printf("Syntax error (use ? for help)\n");
				/* Don't break out of command processing loop */
				break;
		}
	} else {
		printf("Syntax error (use ? for help)\n");
		/* Don't break out of command processing loop */
	}
	return 0;
}

/* ----------------------------------------------------------------
 * Raw pointer read/write access
 * ----------------------------------------------------------------
 */
static void
write_8(
	device_t      *dev,
	unsigned int   addr,
	unsigned char  data)
{
	*(volatile unsigned char *)(dev->addr + addr) = data;
	msync((void *)(dev->addr + addr), 1, MS_SYNC | MS_INVALIDATE);
}

static unsigned char
read_8(
	device_t      *dev,
	unsigned int   addr)
{
	return *(volatile unsigned char *)(dev->addr + addr);
}

static void
write_le16(
	device_t      *dev,
	unsigned int   addr,
	unsigned short int data)
{
	if (__BYTE_ORDER != __LITTLE_ENDIAN) {
		data = bswap_16(data);
	}
	*(volatile unsigned short int *)(dev->addr + addr) = data;
	msync((void *)(dev->addr + addr), 2, MS_SYNC | MS_INVALIDATE);
}

static unsigned short int
read_le16(
	device_t      *dev,
	unsigned int   addr)
{
	unsigned int data = *(volatile unsigned short int *)(dev->addr + addr);
	if (__BYTE_ORDER != __LITTLE_ENDIAN) {
		data = bswap_16(data);
	}
	return data;
}

static void
write_be16(
	device_t      *dev,
	unsigned int   addr,
	unsigned short int data)
{
	if (__BYTE_ORDER == __LITTLE_ENDIAN) {
		data = bswap_16(data);
	}
	*(volatile unsigned short int *)(dev->addr + addr) = data;
	msync((void *)(dev->addr + addr), 2, MS_SYNC | MS_INVALIDATE);
}

static unsigned short int
read_be16(
	device_t      *dev,
	unsigned int   addr)
{
	unsigned int data = *(volatile unsigned short int *)(dev->addr + addr);
	if (__BYTE_ORDER == __LITTLE_ENDIAN) {
		data = bswap_16(data);
	}
	return data;
}

static void
write_le32(
	device_t      *dev,
	unsigned int   addr,
	unsigned int data)
{
	if (__BYTE_ORDER != __LITTLE_ENDIAN) {
		data = bswap_32(data);
	}
	*(volatile unsigned int *)(dev->addr + addr) = data;
	msync((void *)(dev->addr + addr), 4, MS_SYNC | MS_INVALIDATE);
}

static unsigned int
read_le32(
	device_t      *dev,
	unsigned int   addr)
{
	unsigned int data = *(volatile unsigned int *)(dev->addr + addr);
	if (__BYTE_ORDER != __LITTLE_ENDIAN) {
		data = bswap_32(data);
	}
	return data;
}

static void
write_be32(
	device_t      *dev,
	unsigned int   addr,
	unsigned int data)
{
	if (__BYTE_ORDER == __LITTLE_ENDIAN) {
		data = bswap_32(data);
	}
	*(volatile unsigned int *)(dev->addr + addr) = data;
	msync((void *)(dev->addr + addr), 4, MS_SYNC | MS_INVALIDATE);
}

static unsigned int
read_be32(
	device_t      *dev,
	unsigned int   addr)
{
	unsigned int data = *(volatile unsigned int *)(dev->addr + addr);
	if (__BYTE_ORDER == __LITTLE_ENDIAN) {
		data = bswap_32(data);
	}
	return data;
}

