#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>

#include "iccom.h"

using namespace std;

#define SOCKET_READ_TIMOUT_MSEC (20*1000)

void print_usage(char *prg)
{
	fprintf(stderr, "%s - recv iccom-frames via sockets.\n", prg);
	fprintf(stderr, "\nUsage: %s <ch_id>.\n", prg);
	fprintf(stderr, "\n<ch_id>:\n"
	        " 2 byte hex chars\n");
	fprintf(stderr, "Examples:\n");
	fprintf(stderr, "  15A1\n\n");
}

struct iccom_frame {
	uint16_t ch_id;
	uint8_t  len;
	uint8_t  data[64];
};

static int asc2nibble(char c)
{
	if ((c >= '0') && (c <= '9'))
		return c - '0';

	if ((c >= 'A') && (c <= 'F'))
		return c - 'A' + 10;

	if ((c >= 'a') && (c <= 'f'))
		return c - 'a' + 10;

	return 16; /* error */
}

int parse_frame(char *cs, struct iccom_frame *f) {
	int i, idx, dlen, len;
	int maxdlen = 64;
	unsigned char tmp;

	len = strlen(cs);

	memset(f, 0, sizeof(*f));

	if (len != 4)
		return -1;

    for (i=0; i<4; i++){
        if ((tmp = asc2nibble(cs[i])) > 0x0F)
            return -1;
        f->ch_id |= (tmp << (3-i)*4);
    }

	return 0;
}

int main(int argc, char **argv)
{    
    int ret = -1;
	struct iccom_frame frame;
    
    if (argc != 2) {
		print_usage(argv[0]);
		return ret;
	}
	
    if(parse_frame(argv[1], &frame) != 0) {
		print_usage(argv[0]);
		return ret;
    }

    IccomSocket sk {frame.ch_id};

    if (sk.open() < 0) {
        printf("Failed to open socket for channel %04x, aborting\n", sk.channel());
        return -EFAULT;
    }

    if (sk.set_read_timeout(SOCKET_READ_TIMOUT_MSEC) < 0) {
        printf("Could not set the socket timeout, aborting\n");
        sk.close();
        return -EFAULT;
    }

    if (sk.receive() < 0) {
        printf("Receive on channel %04x failed\n", sk.channel());
        goto exit;
    }

    frame.len = sk.input_size();
    if (frame.len < 1) {
        printf("Receive on channel %04x NULL\n", sk.channel());
        goto exit;
    } else {
        printf("recv %04x#",sk.channel());
        for(int i = 0;i < frame.len;i++) {
            frame.data[i] = sk[i];
            printf("%02x",frame.data[i]);
        }
        printf("\n");
        ret = 0;
    }

exit:
    sk.close();
    return ret;
}
