/* v3dclk.c - set the V3D clock through the VideoCore mailbox property interface.
 * usage: v3dclk <clock_id> <MHz>     (5 = V3D, 4 = CORE, 3 = ARM)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define IOCTL_MBOX_PROPERTY _IOWR(100, 0, char*)

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        fprintf(stderr, "usage: %s <clock_id> <MHz>\n", argv[0]);
        return 2;
    }
    unsigned id = (unsigned)atoi(argv[1]);
    unsigned mhz = (unsigned)atoi(argv[2]);

    int fd = open("/dev/vcio", O_RDWR);
    if(fd < 0)
    {
        perror("open /dev/vcio");
        return 2;
    }
    uint32_t* buf;
    if(posix_memalign((void**)&buf, 16, 4096) != 0)
        return 2;
    memset(buf, 0, 4096);

    buf[0] = 9 * 4;      /* total size */
    buf[1] = 0;          /* request */
    buf[2] = 0x00038002; /* SET_CLOCK_RATE */
    buf[3] = 12;         /* value buffer size */
    buf[4] = 8;          /* request/response size */
    buf[5] = id;
    buf[6] = mhz * 1000000u;
    buf[7] = 0;          /* skip turbo */
    buf[8] = 0;          /* end tag */

    if(ioctl(fd, IOCTL_MBOX_PROPERTY, buf) < 0)
    {
        perror("ioctl");
        close(fd);
        return 2;
    }
    printf("clock %u request %u MHz -> response %u Hz (code %u)\n", id, mhz, buf[6], buf[1]);
    close(fd);
    free(buf);
    return 0; /* ioctl accepted; verify with vcgencmd */
}
