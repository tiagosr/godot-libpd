/* evdump.c — dumps evdev events from one input device for N seconds.
 * Usage: ./evdump [seconds] [device]   (device default /dev/input/event3)
 */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int secs = (argc > 1) ? atoi(argv[1]) : 25;
	const char *dev = (argc > 2) ? argv[2] : "/dev/input/event3";

	int fd = open(dev, O_RDONLY | O_NONBLOCK);
	if (fd < 0) { perror(dev); return 1; }

	struct input_id id;
	if (ioctl(fd, EVIOCGID, &id) < 0)
		perror("EVIOCGID");
	printf("evdump: %s (id %04x:%04x:%04x), %ds — press buttons now\n", dev, id.vendor, id.product, id.version, secs);
	fflush(stdout);

	long t0 = time(NULL);
	struct input_event ev;
	while (time(NULL) - t0 < secs) {
		struct pollfd p = { fd, POLLIN, 0 };
		int r = poll(&p, 1, 100);
		if (r <= 0)
			continue;
		if (read(fd, &ev, sizeof(ev)) != sizeof(ev))
			continue;
		if (ev.type == EV_KEY || ev.type == EV_ABS || ev.type == EV_REL || ev.type == EV_SYN)
			printf("t=%2ld %s type=%d code=%d value=%d\n",
				(long)(time(NULL) - t0), ev.type == EV_SYN ? "SYN " : (ev.type == EV_ABS ? "ABS " : (ev.type == EV_REL ? "REL " : "KEY ")),
				ev.type, ev.code, ev.value);
		fflush(stdout);
	}
	printf("evdump: done\n");
	return 0;
}
