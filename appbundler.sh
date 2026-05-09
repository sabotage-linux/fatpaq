#!/bin/sh
# need to hook execve and dump all bins started
# readelf -l to get interp
usage() {
	echo "$0 LOG DIR"
	echo "LOG is a log of filtrace containing filenames an app accessed"
	echo "DIR is the name of a directory we copy all required libs too"
	echo "it'll become / of something that can be packed up as a rootfs"
	exit 1
}

docopy() {
	fn="$1"
	mkdir -p "$DIR""$(dirname "$fn")"
	cp -a "$fn" "$DIR""$fn"
	if test -L "$fn" ; then
		targ="$(readlink -f "$fn")"
		docopy "$targ"
	fi
}

test -z "$2" && usage

LOG="$1"
DIR="$2"
mkdir -p "$DIR"
for i in etc home dev sys proc tmp/.X11-unix root; do
	mkdir -p "$DIR"/"$i"
done
ln -sf . "$DIR"/usr
ln -sf lib "$DIR"/lib64

docopy /etc/resolv.conf
while read fn ; do
test -e "$fn" || continue
test -d "$fn" && continue
test -e "$DIR""$fn" && continue
copy=false
case "$fn" in
	/usr/bin/*|/bin/*) copy=true ;;
	/usr/lib/*|/lib/*) copy=true ;;
	/etc/fonts/*) copy=true ;;
	/usr/share/*|/share/*) copy=true ;;
esac
if $copy ; then
	docopy "$fn"
fi
done < "$LOG"
