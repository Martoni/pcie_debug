# pcie_debug
Command line tool to Read/Write to PCIe BARx memory space

# Command file structure

The option -f allow to provide a commands file to pci_debug. The commands file will be executed before give you the hand on the PCI> prompt (if -q option is not).
first line => Must be the BAR concerned by the command file. Must be compliant with the -b option
Other lines => Commands

Example in a file put:
```sh
    bar2
    c32 0 A5
    c32 14 196E
    c32 8 AAB565
```
The -q option allows to quit pci_debug after the execution of the command file. This option allows you to chain several command files in a bash script for example.

Example:

    #!/bin/sh

    #bar 1 configure usart
        echo "bar1" > /tmp/cmd
        echo "c32 0 40" >> /tmp/cmd
        echo "c32 4 196E" >> /tmp/cmd
        echo "c32 8 196E" >> /tmp/cmd
        echo "c32 18 8004" >> /tmp/cmd
        pci_debug -s 01:00.0 -b 1 -f /tmp/cmd -q -v 1

    #bar 0 read conf irq
        echo "bar0" > /tmp/cmd
        echo "d32 40 1" >> /tmp/cmd
        echo "d32 50 1" >> /tmp/cmd
        echo "d32 60 1" >> /tmp/cmd
        pci_debug -s 01:00.0 -b 0 -f /tmp/cmd -q -v 0
        
    #bar 1 start to send words
        echo "bar1" > /tmp/cmd
        echo "c32 14 AA" >> /tmp/cmd
        echo "c32 14 BB" >> /tmp/cmd
        echo "c32 14 CC" >> /tmp/cmd
        echo "c32 14 DD" >> /tmp/cmd
        pci_debug -s 01:00.0 -b 1 -f /tmp/cmd -q -v 1

    #bar 1 start to read words (not very optim but it's for debug)
        echo "bar1" > /tmp/cmd
        echo "d32 14 1" >> /tmp/cmd
        for i in {1. .4}
        do
            pci_debug -s 01:00.0 -b 1 -f /tmp/cmd -q -v 0
        done

    #take the hand on bar 1 
        pci_debug -s 01:00.0 -b 1 

output: 
  ```sh
    Accessing BAR1
    Send: c32 0 40
    Send: c32 4 196E
    Send: c32 8 196E
    Send: c32 18 8004

    Accessing BAR0

    00000040: 00000001


    00000050: 0000FFFF


    00000060: 00000004


    Accessing BAR1
    Send: c32 14 AA
    Send: c32 14 BB
    Send: c32 14 CC
    Send: c32 14 DD

    00000014: 000000CC


    PCI debug
    ---------

    - accessing BAR1
    - region size is 1048576-bytes
    - offset into region is 0-bytes

    ?                         Help
    d[width] addr len         Display memory starting from addr
                                [width]
                                8   - 8-bit access
                                16  - 16-bit access
                                32  - 32-bit access (default)
    c[width] addr val         Change memory at addr to val
    e                         Print the endian access mode
    e[mode]                   Change the endian access mode
                                [mode]
                                b - big-endian (default)
                                l - little-endian
    f[width] addr val len inc  Fill memory
                                addr - start address
                                val  - start value
                                len  - length (in bytes)
                                inc  - increment (defaults to 1)
    q                          Quit

    Notes:
        1. addr, len, and val are interpreted as hex values
        addresses are always byte based

    PCI>

  ```

# Links

This tool is derived from D. W. Hawkins (dwh@ovro.caltech.edu). The original
sources can be found on [altera forum](http://www.alteraforum.com/forum/showthread.php?t=35678).

