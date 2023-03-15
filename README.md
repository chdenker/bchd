# bchd
## Basic character device

This is a kernel module that adds a new device /dev/bchd, takes user (text) input written to /dev/bchd and stores it in a dynamic storage. 
Whenever a user reads from /dev/bchd, the text data is transferred back to the user.
Furthermore, this module periodically (1 word per sec) writes the stored text data into the kernel log.

The code is based on the scull device driver as described by Corbet et al. in "Linux Device Drivers Third Edition".

This module was tested on a Debian virtual machine running the Linux kernel version 5.10.0-21.

## Using the module

Build the module:
```sh
make
```

Load the module (sudo might be necessary):
```sh
./bchd_load
```

This script creates a new device /dev/bchd and loads the module using the insmod command.
We can check whether it is loaded using
```sh
lsmod | grep bchd
```
which should show the module.

Now, text data can be written to /dev/bchd:
```sh
cat c-song.txt > /dev/bchd
```

and also read from /dev/bchd:
```sh
cat /dev/bchd
```

Writing new text to /dev/bchd overwrites the previous contents of /dev/bchd.

Whenever it is loaded or unloaded, the module writes messages into the kernel log.
Furthermore, each second, one word from the stored data is written into the kernel log.
We can observe this, for example, using
```sh
journalctl -f
```
or using
```sh
dmesg
```

Unload the module:
```sh
./bchd_unload
```

The file "dmesg_output.txt" shows an exemplary kernel log and was generated as follows:
```sh
sudo ./bchd_load
cat c-song.txt > /dev/bchd
sudo ./bchd_unload
dmesg | tail -n 40 > dmesg_output.txt
```

