G15DAEMON
=========

G15daemon  takes control of the G15 keyboard, allowing the use of all keys through the uinput device driver.
It  also controls  the use of the keyboard's LCD display, allows multiple, simultaneous client applications
to connect, and gives  the  user the  ability to switch between client apps at the press of a button.

Currently, patches to enable support for the daemon are available for libgraphlcd, which in turn enables
support for lcdproc, vdr, and any other applications able to use that library.

=======
Warning
=======
I'm discontinuing this after someone made a fuzz about a feature he decided he want a decade later. 
And as far I am concerned, Arch Linux's AUR administrators find this behavior just fine, so I'm not wasting my efforts on this anymore.
I can still fix issues as I always did and help via mail, but keep in mind Arch Linux is impossible to be supported.

============
Requirements
============

- libg15
- libg15render

=======
Install
=======

./configure
make && make install

Or use your distribution (Available on AUR, also debian and rpm scripts are provided).

=======
Running
=======

G15daemon must be run as root, as it writes to system logs.

to run : g15daemon
to kill a previously running daemon : g15daemon -k

=============
Using the LCD
=============

Patches are available for libgraphlcd, in the g15daemon patches subdirectory. You'll need a copy of graphlcd
source, which is available from http://graphlcd.berlios.de.
The patches are against the current version, which is 1.3.

To patch the graphlcd source, extract the graphlcd-base archive, then type:

patch -p0 -i "g15daemon_src_folder"/patch-es/graphlcd-1.3_g15daemon_drv.patch

Where 'g15daemon_src_folder' is the directory you extracted the g15daemon sourcecode into...


=====
Notes
=====

As of version 1.2, the MR key is used for switching between LCD clients, and cannot be used for other purposes if the
LCD is being used.
As of 30/10/06 svn, the client switch key can be altered from L1 to MR by specifying -s on the g15daemon commandline.
This is not recommended.

For a number of reasons, by default the keys on the G15 arent usable - especially without g15daemon. With g15daemon running,
the kernel will know all about the new keys, but your 'X' server won't without a bit of help. In the contrib subfolder of the
g15daemon distribution, you'll find an xmodmaprc script.  To have all those new keys working in X11, you'll need to have xmodmap
read that file on every startup. Depending on your distro, xmodmap may automatically load the file if it is copied to
${HOME}/.Xmodmap otherwise you will have to load it yourself. There is an example xmodmap.sh file in the contrib directory that
will do this, if it's placed (for instance) in kde's Autostart folder.

=======
Options
=======
+------------------+---------------------------------------------------------------------------------+
| -k               | Kill a previous incarnation                                                     |
+------------------+---------------------------------------------------------------------------------+
| -K               | Turn off the keyboard backlight on the way out                                  |
+------------------+---------------------------------------------------------------------------------+
| -h               | Shows this help                                                                 |
+------------------+---------------------------------------------------------------------------------+
| -s               | Changes the screen-switch key from L1 to MR (may cause issues)                  |
+------------------+---------------------------------------------------------------------------------+
| -d               | Debug Mode - Stay in foreground and output all debug messages to STDERR         |
+------------------+---------------------------------------------------------------------------------+
| -v               | Show version                                                                    |
+------------------+---------------------------------------------------------------------------------+
| -l               | Set default LCD backlight level                                                 |
+------------------+---------------------------------------------------------------------------------+
| --set-backlight  | Sets backlight individually for currently shown screen.                         |
|                  | Default is to set backlight globally (keyboard default).                        |
+------------------+---------------------------------------------------------------------------------+

===================
Systemd integration
===================

When built with libsystemd available (soft-optional - the daemon still
builds and runs fine without it), g15daemon reports live status via
``sd_notify()``, so ``systemctl status g15daemon`` shows version, uptime,
connected client count, the foreground client, and LCD draw timing instead
of a bare "running". A watchdog ping keeps systemd from silently believing
a hung daemon is still healthy.

For this to actually take effect, the installed unit file needs:

- ``NotifyAccess=main`` - systemd's own default is ``none``, which silently
  discards every status update.
- ``WatchdogSec=30`` (or similar) - enables the watchdog ping.
- ``AmbientCapabilities=CAP_SYS_PTRACE`` and
  ``CapabilityBoundingSet=CAP_SYS_PTRACE CAP_SETUID CAP_SETGID`` - lets
  g15daemon resolve a connected client's process name (shown in the status
  line and logged on connect/disconnect) after it drops root to ``nobody``.
  ``CAP_SETUID``/``CAP_SETGID`` must stay in that list alongside
  ``CAP_SYS_PTRACE``: the bounding set is a replacement, not an addition,
  and g15daemon needs those two to perform the privilege drop itself.

``contrib/init/g15daemon.service`` in this repo has all of the above. Note
that ``make install`` does **not** install the systemd unit - if your distro
package ships its own copy (as Debian's does), you'll need to copy this
repo's version over it yourself and run ``systemctl daemon-reload`` for
these settings to apply.

=======
FreeBSD
=======

G-Keys works a bit flaky (sometimes lags occurs and I don't know how to fix it)
LCD works as expected.
