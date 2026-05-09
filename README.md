fatpaq
======

utils to create truly standalone app bundles of linux software, in the
vein of `flatpak`s and `appimage`s.

unlike appimage files, the so created app images are completely standalone,
and can be executed on ANY linux system with the same cpu architecture, as
long as that linux system supports USER_NS namespaces and fuse drivers.

building
--------
- install upx, as well as static libraries of zstd, fuse and squashfuse_ll.
- for best results, install the `musl` package and use `musl-gcc` to compile
  zstd, fuse 2.9.9 and squashfuse **from source** and install them into a
  common prefix. for squashfuse, enable only the zstd option and disable all
  other compression methods.
- run `make`, on success `make pack` to upx-pack the `launcher` binary.
  you can export `CC=musl-gcc` before doing so if you want to use musl-built
  libraries for best size. the launcher binary will be roughly 100KB big
  when statically linked against musl, and the before-mentioned libs.

usage
-----
- install strace, then run `sh trace.sh APP.log APP args...` on the app you want
  to bundle.
  this creates a file called APP.log containing a list of all files opened
  and executed during the program's lifecycle.
- run `sh appbundler.sh APP.log APPDIR`
  this will create a directory called APPDIR containing the directory structure
  with all files expected by the app, ready to be packed into an app image.
- you can now do final touchups in APPDIR, such as maybe editing the copied
  etc/resolv.conf. you could put `nameserver 8.8.8.8` in there to use google
  nameservers in case your own local ones aren't available elsewhere.
- run `mksquashfs APPDIR/* APP.sqfs -comp zstd -b 1M -Xcompression-level 22`
  to create a zstd squashfs `APP.sqfs` image with best compression.
  in my tests with firefox-esr from voidlinux, this produces a 190MB
  squashfs file from a directory > 600MB.
- run `./makeapp ./launcher APP.sqfs /usr/bin/APP APP.exe`.
  this merges the launcher with the squashfs image into the standalone app
  image.
- done - you can now run `./APP.exe` - this will do the following:
  - create a new user namespace, inside of which we are root (pid 0).
  - mount the piggy-backed squashfs image
  - bind-mount host directories like /dev and importantly, the X11 socket
    /tmp/.X11-unix into the rootfs
  - chroot into the new squashfs mountpoint
  - run /usr/bin/APP - as specified during `makeapp`. all additional command
    line args will be passed to APP as if it had been started directly.

unlike other container building solutions, no hacks like `sharun` are required:
because we chroot into the rootfs, the dynamic linker is found naturally in
the expected location.

note that this technique works perfectly with X11 apps, thanks to being able
to bind mount the directory containing X11's unix socket. whether it works
with wayland is unknown. wayland users should reconsider switching to X11,
a tried-and-true technology that just works (and that's precisely why certain
powers push wayland - they don't want a stable linux ecosystem, everything
must change all the time so stuff keeps on breaking).
