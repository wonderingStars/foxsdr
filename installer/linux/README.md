# Linux USB permissions for FoxSDR's native SDR drivers

FoxSDR's native RTL-SDR, HackRF, RX888 mk2 and Mirics MSi2500 drivers talk to
the radio directly through `/dev/bus/usb/BBB/DDD` (Linux's `usbfs`) - no
libusb, no SoapySDR module, no root required once the rule below is
installed. See `src/usb/usbfs_device.cpp` for the transport itself.

## Install the udev rule (one-time, needs root)

```sh
sudo cp 99-foxsdr-sdr.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Then **unplug and replug the radio**. `udevadm trigger` re-runs rules against
devices that are already present, but a device node's ownership/ACL is only
re-evaluated at that point if the kernel re-announces it - in practice, on
every distro this was checked against, a fresh unplug/replug is the reliable
way to see the new rule take effect immediately rather than at the next boot.

No reboot is needed for either the install or a later removal.

## What the rule does, and why it is needed at all

By default a USB device node under `/dev/bus/usb/` is owned `root:root` with
a restrictive mode, so `open()` from a normal desktop session fails `EACCES`
the instant the dongle is plugged in - this is not a FoxSDR bug, it is what
every udev-less USB device node looks like on Linux. `99-foxsdr-sdr.rules`
grants access two ways at once:

- `TAG+="uaccess"` - hands the device to whichever user is logged in at the
  graphical or console seat, through `systemd-logind`. This is the modern
  mechanism (also how your webcam and most other desktop SDR tools already
  work) and needs nothing further from you: no group, no re-login.
- `MODE="0660", GROUP="plugdev"` - a fallback for a machine with no logind
  seat (a headless box reached only over SSH, an init system without
  `systemd`). Add yourself to the `plugdev` group
  (`sudo usermod -aG plugdev "$USER"`, then log out and back in) and this
  half takes over.

Whichever applies on your system, you do not need to run FoxSDR as root and
should not: nothing about this transport needs root privilege once the rule
is installed, and running the whole application as root to work around a
permissions problem is worse than the problem.

## The RTL-SDR and the DVB-T driver: nothing to do

An RTL2832U dongle ships bound to `dvb_usb_rtl28xxu`, the kernel's own DVB-T
driver for this exact silicon - Linux's version of "needs Zadig" on Windows.
**You do not need to unbind it, blacklist it, or do anything about it before
running FoxSDR.** `openWinUsb()` in `src/usb/usbfs_device.cpp` detaches
whatever kernel driver is bound and claims the interface for itself
automatically, with the `USBDEVFS_DISCONNECT_CLAIM` ioctl - the same
mechanism `libusb`'s own Linux backend uses for the same purpose, so this
matches how every other Linux SDR tool already behaves. That ioctl has been
in the kernel since 4.10 (2017); every currently-supported distribution is
well past it.

If you would still rather the kernel never touch an RTL-SDR at all - a
machine dedicated to SDR use, or another application on the box that keeps
re-binding the driver between FoxSDR sessions - an **optional**
`blacklist-rtl28xxu-optional.conf` is provided:

```sh
sudo cp blacklist-rtl28xxu-optional.conf /etc/modprobe.d/blacklist-rtl28xxu.conf
```

then unplug and replug the dongle (or `sudo rmmod dvb_usb_rtl28xxu` if one is
already attached). This changes nothing about whether FoxSDR can open the
device either way; it only stops the kernel from claiming it in the first
place. Delete the file and re-plug to reverse it.

## Troubleshooting

- **"no RTL-SDR is bound to WinUSB on this machine" / "no HackRF found" /
  etc. even though it is plugged in.** The error text mentions WinUSB because
  it is shared with the Windows build (see the message's own source); on
  Linux it means the device did not appear in the `/sys/bus/usb/devices`
  walk with a matching VID:PID at all. Check `lsusb` shows the device, and
  that its VID:PID appears in `99-foxsdr-sdr.rules` - the file lists exactly
  the ids each driver's own `usbIds()`/`rtlSdrUsbIds()` table matches.
- **"permission denied opening /dev/bus/usb/BBB/DDD"** - the rule above is
  either not installed, not reloaded, or was installed after the dongle was
  already plugged in. Re-run the three `udevadm`/`cp` steps and replug.
- **"the radio is already in use by another program or its kernel driver
  could not be detached"** - something else already has the interface open
  (another FoxSDR instance, an SDR tool using libusb, a DVB-T viewer). Close
  it and replug if needed; USBDEVFS_DISCONNECT_CLAIM cannot take an interface
  away from a process that is actively holding it, by design.

## What was verified, and what was not

This transport and this rules file were written and unit-tested on a machine
with no RTL-SDR, HackRF, RX888 or Mirics device attached (WSL2, which also
exposes no `/sys/bus/usb/devices` at all). The sysfs parser, the enumeration
matching (including a right-vendor, wrong-product device that must be left
out) and the open-of-a-nonexistent-path failure path are exercised by
`tests/test_usb_usbfs.cpp` without hardware; the bounded-read timing check in
that file runs against the shared fake transport, not against a usbfs
handle. **Not verified on this machine, for lack of one:** that
`USBDEVFS_DISCONNECT_CLAIM` actually detaches `dvb_usb_rtl28xxu` on a real
dongle, that interface 0 is the correct claim for all four device families,
that `readBulk()`'s poll bound and `endBulkStream()`'s drain bound behave as
written on a live usbfs file descriptor, and that a real bulk stream delivers
samples end to end. A user or tester with the hardware attached is the check that
closes this out.
