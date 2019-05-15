/**
 * @file chrony.c
 * @brief Exports gpsmcr time information to a chrony socket
 *
 * 2019 - Frank Villaro-Dixon <frank.villaro@infomaniak.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include "timespec.h"
#include "chrony.h"


/* Creates an UNIX socket to send time info to chrony */
int
chrony_init_socket(void)
{
	int fd;

	if(access(CHRONY_SOCK_PATH, F_OK) != 0) {
		perror(CHRONY_SOCK_PATH" does not exist. Launch chronyd first !");
		return -1;
	}

	if((fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0) {
		perror("Could not allocate socket");
		return -1;
	}

	struct sockaddr_un saddr;
	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family = AF_UNIX;
	strncpy(saddr.sun_path, CHRONY_SOCK_PATH, sizeof(saddr.sun_path));

	if(connect(fd, (struct sockaddr *)&saddr, SUN_LEN(&saddr)) < 0) {
		close(fd);
		perror("Could not connect to socket");
	}

	return fd;
}

/* Send the regular timedelta to chrony via the socket */
void
chrony_send_delta(int sock_fd, double delta)
{
    struct timespec offset;
    struct sock_sample sample;
    struct tm tm;


    sample.pulse = 0; /* We are PPS but know date too */
    sample.magic = CHRONY_SOCK_MAGIC;
    sample.leap = 0; //XXX masterclock driver still doesnt support leap seconds ‽


    //system_time: CLOCK_REALTIME
    struct timespec system_time;
    clock_gettime(CLOCK_REALTIME, &system_time);

    TSTOTV(&sample.tv, &system_time);

    //The real deal: at (around) system_time, what was the difference (delta)
    //between reference (GPS) time and (machine) clock time ?
    sample.offset = -delta;

    (void)send(sock_fd, &sample, sizeof(sample), 0);
}

