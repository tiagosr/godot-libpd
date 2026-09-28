/* fbtest.c — writes bright, cycling colors to /dev/fb0 page 0.
 * No GPU, no EGL. Pure fb0 write test: if the screen changes color,
 * /dev/fb0 is (at least partially) the scanned-out path.
 * Build: gcc -O2 -o fbtest fbtest.c
 * Run:   ./fbtest [seconds, default 15]
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/fb.h>


int main(int argc, char **argv)
{
	int secs = (argc > 1) ? atoi(argv[1]) : 15;
	int fb = open("/dev/fb0", O_RDWR | O_SYNC);
	if (fb < 0) { perror("open /dev/fb0"); return 1; }

	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fb, FBIOGET_VSCREENINFO, &v)) { perror("VSCREENINFO"); return 1; }
	if (ioctl(fb, FBIOGET_FSCREENINFO, &f)) { perror("FSCREENINFO"); return 1; }

	unsigned char *m = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }

	/* test patterns, 0xAARRGGBB little-endian (B,G,R,A in memory) */
	const uint32_t colors[4] = {
		0xFF000044, /* red   */
		0xFF00FF44, /* green */
		0xFFFF0044, /* blue  */
		0xFFDDDDDD, /* light gray */
	};
	const char *names[4] = { "RED", "GREEN", "BLUE", "GRAY" };
	uint32_t w = v.xres, h = v.yres;
	uint32_t stride = f.line_length;
	uint32_t words = stride / 4;
	int t = 0, ci = 0;
	fprintf(stderr, "fbtest: %ux%u stride=%u, running %ds\n",
		(unsigned)w, (unsigned)h, (unsigned)stride, secs);
	fflush(stderr);

	while (t < secs) {
		uint32_t c = colors[ci];
		for (uint32_t y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)(m + (size_t)y * stride);
			for (uint32_t x = 0; x < words; x++)
				row[x] = c;
		}
		fprintf(stderr, "t=%2d %s\n", t, names[ci]);
		fflush(stderr);
		ci = (ci + 1) % 4;
		t += 2;
		usleep(200000); /* ~5 fps */
	}
	fprintf(stderr, "fbtest: done\n");
	return 0;
}
