#ifndef _GPSMCR_H
#define _GPSMCR_H

void sync_critical_section(int64 reftime, int64 *systime, int64 *delta);
void *pps_monitor(void *arg);
void *remote_io(void *arg);
void manage_connections(void);
void remote_io_process(int sock, char *in_buf);
void synchronize_system_clock(int64 reftime, int64 delta);
void jitter_calculations(jitter_calc_t *j, int64 diff);
void drift_calculations(jitter_calc_t *j, drift_data_t *d);
void catch_signals(int sig);
void process_nmea(char *nmea_buffer, unsigned int nmea_buffer_size);
int get_serial_ports(void);
int enumerate_serial(void);
int enumerate_serial_AVR(void);
int enumerate_serial_GPS(void);
int setup_com_port(const char *port, tcflag_t baud, struct termios *oldtio);
int release_com_port(int fd, struct termios *tio);
int parse_conf(char *conf);
void log_printf(char debug_level, char *fmt, ...);

#endif // _GPSMCR_H
