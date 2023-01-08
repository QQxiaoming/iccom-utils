#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>

#include "iccom.h"

using namespace std;

#define SOCKET_READ_TIMOUT_MSEC (20*1000)

void print_usage(char *prg)
{
	fprintf(stderr, "%s - send iccom-frames via sockets.\n", prg);
	fprintf(stderr, "\nUsage: %s <frame>.\n", prg);
	fprintf(stderr, "\n<frame>:\n");
	fprintf(stderr, " <ch_id>#{data} for iccom data frames\n");
	fprintf(stderr, "<ch_id>:\n"
	        " 2 byte hex chars\n");
	fprintf(stderr, "{data}:\n"
	        " ASCII hex-values\n");
	fprintf(stderr, "Examples:\n");
	fprintf(stderr, "  15A1#1122334455667788\n\n");
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

	if (len < 5)
		return -1;

	if (cs[4] == '#') {

		idx = 5;
		for (i=0; i<4; i++){
			if ((tmp = asc2nibble(cs[i])) > 0x0F)
				return -1;
			f->ch_id |= (tmp << (3-i)*4);
		}

	} else
		return -1;

	for (i=0, dlen=0; i < maxdlen; i++){

		if(idx >= len)
			break;

		if ((tmp = asc2nibble(cs[idx++])) > 0x0F)
			return -1;
		f->data[i] = (tmp << 4);
		if ((tmp = asc2nibble(cs[idx++])) > 0x0F)
			return -1;
		f->data[i] |= tmp;
		dlen++;
	}
	f->len = dlen;

	return 0;
}

int main(int argc, char **argv)
{
	struct iccom_frame frame;

    if (argc != 2) {
		print_usage(argv[0]);
		return 1;
	}

	parse_frame(argv[1], &frame);
    if(frame.len == 0) {
		print_usage(argv[0]);
		return 1;
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

    std::vector<char> v;
    for(int i=0;i<frame.len;i++) {
        v.push_back(frame.data[i]);
    }
    sk << v;

    if (sk.send() < 0) {
        printf("send on channel %04x failed\n", sk.channel());
    } else {
		printf("send %04x#",sk.channel());
        for(int i = 0;i < frame.len;i++) {
            printf("%02x",frame.data[i]);
        }
        printf("\n");
	}

    sk.close();

    return 0;
}
