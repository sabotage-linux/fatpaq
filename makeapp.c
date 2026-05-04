#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

int usage(char *a0) {
	dprintf(2,
		"usage: %s [-n] [-h] [-x] EXE IMG CMD OUT\n"
		"EXE: path to launcher.exe\n"
		"IMG: path to squashfs image\n"
		"CMD: command to execute after mounting\n"
		"OUT: filename of created app bundle\n"
		"-n : disable network\n"
		"-h : disable homedir mount\n"
		"-x : disable X11 socket mount (/tmp/.X11-unix)\n"
		, a0);
	return 1;
}

static void die(char* msg) {
	dprintf(2, "fatal: %s\n", msg);
	exit(1);
}

#define BLOCK_SIZE 1024*1024
int main(int argc, char**argv) {
	int c, a;
	int x = 0; int n = 0; int h = 0;
	while((c = getopt(argc, argv, ":nhx")) != -1) switch(c) {
	case 'x': x = 1; break;
	case 'n': n = 1; break;
	case 'h': h = 1; break;
	default: return usage(argv[0]);
	}
	a = optind;
	if(argc < a+4) return usage(argv[0]);
	char *exe = argv[a++];
	char *img = argv[a++];
	char *cmd = argv[a++];
	char *out = argv[a++];
	uint64_t exesz = 0;
	FILE *fin = fopen(exe, "rb");
	if(!fin) die("couldnt open EXE");
	FILE *fout = fopen(out, "wb");
	if(!fout) die("couldnt open OUT");
	unsigned char* buf = malloc(BLOCK_SIZE);
	for(;;) {
		size_t s = fread(buf, 1, BLOCK_SIZE, fin);
		if(s == 0) break;
		exesz += s;
		if(s != fwrite(buf, 1, s, fout)) {
			perror("fwrite launcher");
			return 1;
		}
	}
	fclose(fin);
	fin = fopen(img, "rb");
	if(!fin) die("couldnt open IMG");
	uint64_t imgsz = 0;
	for(;;) {
		size_t s = fread(buf, 1, BLOCK_SIZE, fin);
		if(s == 0) break;
		imgsz += s;
		if(s != fwrite(buf, 1, s, fout)) {
			perror("fwrite img");
			return 1;
		}
	}
	fclose(fin);
	snprintf(buf, BLOCK_SIZE,
	"command=%s\n"
	"squash_start=%llu\n"
	"disable_net=%d\n"
	"disable_home=%d\n"
	"disable_x11=%d\n"
	, cmd, exesz, n, h, x);
	size_t l = strlen(buf);
	fwrite(buf, l, 1, fout);
	uint64_t conf = exesz + imgsz;
	fwrite(&conf, 8, 1, fout);
	fclose(fout);
	chmod(out, 0770);
	printf("app bundle %s successfully created, total size: %llu\n",
		out, conf+8);
	return 0;

}
