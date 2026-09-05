# aboot

aboot is the Linux/Alpha bootloader for SRM. It loads a Linux kernel from an
ext2, ext3, ext4, XFS (v4 and v5), ISO 9660, or UFS filesystem (or a raw disk
partition) and boots it, with support for an `/etc/aboot.conf` configuration
file, kernel argument mapping, and initial ramdisks.

Originally written by David Mosberger and extended by Michael Schwingen and
Will Woods; later maintained by Debian and now upstream at
https://github.com/alphalinux-org/aboot.

For background on SRM and how it loads a bootloader, see the SRM Firmware HOWTO
in `doc/faq/srm.html`.

## Building

- Edit `Makefile` and `include/config.h` to suit your needs; the defaults work
  for most systems. `CONFIG_FILE_PARTITION` in `include/config.h` selects the
  default partition for `/etc/aboot.conf`, but this can also be overridden on
  the boot command line.
- Run `make`. This produces the aboot image, `bootlx`.

## Installing

Write the aboot image to a disk with `swriteboot` (built from the `sdisklabel`
directory):

    # swriteboot -c<boot partition #> <boot device> bootlx

For example, if the kernel images and `aboot.conf` live on `/dev/sda2`:

    # swriteboot -c2 /dev/sda bootlx

Don't run this unless you're sure of the partition layout. If your system
already boots, there's no need to install a new bootloader.

For ISO images, use `isomarkboot` (in `tools/`) to mark an image as bootable;
`netabootwrap` (also in `tools/`) creates images for network booting.

Set the `/etc/aboot.conf` partition with `abootconf`:

    # abootconf /dev/sda 2

`/etc/aboot.conf` needs one line per configuration, e.g.:

    0:2/boot/vmlinux.gz root=/dev/sda3

or, when booting an initramfs:

    0:2/boot/vmlinux.gz initrd=/boot/initramfs root=/dev/sda3

To have SRM boot Linux automatically, set these SRM environment variables (from
Linux, via `/proc/srm_environment/named_variables`, or from the SRM console
itself):

    # cd /proc/srm_environment/named_variables
    # echo -n 0 > boot_osflags
    # echo -n '' > boot_file
    # echo -n 'BOOT' > auto_action
    # echo -n 'dkc100' > bootdef_dev

substituting the actual boot device for `dkc100`. Hit Ctrl+C during boot to get
back to the SRM console if needed.

## Tools

- `sdisklabel/swriteboot` — write aboot to a disk, set the boot partition
- `tools/e2writeboot` — write aboot to an ext2 filesystem
- `tools/abootconf` — query or set the aboot.conf partition number
- `tools/isomarkboot` — mark an ISO 9660 image bootable
- `tools/netabootwrap` — build images for network booting

## Documentation

Man pages are in `doc/man`; the SRM Firmware HOWTO is in `doc/faq`.

## Known issues

- EOF detection on UFS filesystems may not work correctly: reading the
  configuration file can hang if the desired config line isn't found.
- Netboot support and UFS support are not well exercised and may be broken.

## License

See `COPYING`.
