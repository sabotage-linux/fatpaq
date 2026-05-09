#!/bin/sh
usage() {
	echo "$0 LOG DIR"
	echo "LOG is a log of filtrace containing filenames an app accessed"
	echo "DIR is the name of a directory we copy all required libs too"
	echo "it'll become / of something that can be packed up as a rootfs"
	exit 1
}

resolve_symlink_target() {
    file=$1
    target=$2

    # Absolute target: already resolved enough.
    case $target in
        /*) printf '%s\n' "$target"; return 0 ;;
    esac

    # Split file into directory + basename.
    case $file in
        */*) dir=${file%/*} ;;
        *)   dir=. ;;
    esac

    # Remove leading "../" sequences from target while moving up from file's dir.
    while :; do
        case $target in
            ../*)
                case $dir in
                    /*) dir=${dir%/*} ;;
                    .)  dir=. ;;
                    *)  dir=${dir%/*} ;;
                esac
                target=${target#../}
                ;;
            *)
                break
                ;;
        esac
    done

    # Combine and normalize the remaining path.
    printf '%s/%s\n' "$dir" "$target" | awk '
        {
            n = split($0, a, "/")
            out_n = 0
            for (i = 1; i <= n; i++) {
                p = a[i]
                if (p == "" || p == ".") continue
                if (p == "..") {
                    if (out_n > 0) out_n--
                    continue
                }
                out[++out_n] = p
            }
            if ($0 ~ /^\//) printf "/"
            for (i = 1; i <= out_n; i++) {
                printf "%s%s", out[i], (i < out_n ? "/" : "")
            }
            printf "\n"
        }
    '
}

normalize_leading_slashes() {
    s=$1
    while :; do
        case $s in
            //* ) s=${s#/} ;;
            * )   printf '%s\n' "$s"; break ;;
        esac
    done
}

docopy() {
	fn="$1"
	echo "copying $fn..."
	mkdir -p "$DIR""$(dirname "$fn")"
	cp -dp "$fn" "$DIR""$fn"
	if test -L "$fn" ; then
		targ="$(readlink "$fn")"
		docopy "$(resolve_symlink_target "$fn" "$targ")"
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
fn="$(normalize_leading_slashes "$fn")"
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
