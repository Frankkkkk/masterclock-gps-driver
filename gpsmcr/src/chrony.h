/**
 * chrony.h
 * @brief Chrony headers
 * 2019 - Frank Villaro-Dixon <frank.villaro@infomaniak.com>
 */

#ifndef CHRONY_H
#define CHRONY_H

#include <sys/time.h>

#define CHRONY_SOCK_MAGIC 0x534f434b
#define CHRONY_SOCK_PATH "/shared/masterclock.sock"


/* From chrony/refclock_sock.c */
struct sock_sample {
  /* Time of the measurement (system time) */
  struct timeval tv;

  /* Offset between the true time and the system time (in seconds) */
  double offset;

  /* Non-zero if the sample is from a PPS signal, i.e. another source
     is needed to obtain seconds */
  int pulse;

  /* 0 - normal, 1 - insert leap second, 2 - delete leap second */
  int leap;

  /* Padding, ignored */
  int _pad;

  /* Protocol identifier (0x534f434b) */
  int magic;
};


int
chrony_init_socket(void);


void
chrony_send_delta(int sock_fd, double delta);


#endif /* ndef CHRONY_H */

