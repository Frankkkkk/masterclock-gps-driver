#ifndef _GPSMCR_DEFS_H
#define _GPSMCR_DEFS_H

#include <pthread.h>
#include <time.h>

#define GPSMCR_VER_MAJ		0
#define GPSMCR_VER_MIN		4
#define GPSMCR_VER_REV		0

#define MAX_SERIAL_DEVICES	64
#define BAUDRATE_AVR 		B115200
#define BAUDRATE_NMEA		B9600
#define JITTER_HISTORY_MAX	20
#define FREQ_HISTORY_MAX	60
#define MAX_IPC_CONN		5

#define MAX_64B_BUFFER		64
#define MAX_256B_BUFFER		256
#define MAX_512B_BUFFER		512
#define MAX_1K_BUFFER		1024
#define MAX_4K_BUFFER		4096
#define MAX_REPLY_FIELDS	64

// configuration flags

#define CFG_DEVICE_PATH		0x0001
#define CFG_SERIAL_PREFIX	0x0002
#define CFG_LOG_FILE		0x0004
#define CFG_JAM				0x0008
#define CFG_TICK_THRESH		0x0010
#define CFG_TICK_ADJ		0x0020
#define CFG_FREQ_THRESH		0x0040
#define CFG_FREQ_ADJ		0x0080
#define CFG_SOCKET_FILE		0x0100
#define CFG_PROCESS_FILE	0x0200
#define CFG_TC_READER		0x0400
#define CFG_TC_HAS_DATE		0x0800
#define CFG_ALL				0x0FFF

#define TC_READER_NONE		0
#define TC_READER_IRIG		1
#define TC_READER_SMPTE		2

#define TC_SOURCE_DATE_NOT_INCLUDED		0x00000007
#define TC_SOURCE_DATE_LEITCH_FORMAT	0x00000008
#define TC_SOURCE_DATE_SMPTE_309M_TZ	0x0000000C
#define TC_SOURCE_DATE_SMPTE_309M_NOTZ	0x0000000D
#define TC_SOURCE_DATE_IRIG_DOY_YY		0x0000000E
#define TC_SOURCE_DATE_IRIG_DOY			0x0000000F

// message output levels (default output level will be MSG_ALWAYS)

#define MSG_ALWAYS			0		// administrative messages that we always want to see logged
#define MSG_DEBUG1			1		// messages considered critical to any diagnostics
#define MSG_DEBUG2			2		// more optional diagnostic messages
#define MSG_DEBUG3			3		// deep diagnostic info
#define MSG_DEBUG4			4		// really deep, log-clogging diagnostics!
#define MSG_DEBUG5			5		// NMEA messages

	
// object to manage top-of-second processing

typedef struct {
	pthread_cond_t		process_tos; 		// condition to flag non-priority top-of-second processing
	pthread_mutex_t		process_tos_mtx;	// mutex to flag non-priority top-of-second processing
	int					fd;					// primary (AVR) serial port file descriptor
	struct timespec		tp;					// last capture of high-performance counter

} pps_state_t;

// object to manage jitter-smoothed differential calculations

typedef struct {
	int64				smoothed;
	int64 				samples[JITTER_HISTORY_MAX];
	int 				sample_cnt;

} jitter_calc_t;

// object to manage tick adjustment calculations

typedef struct {
	long				prior_tick_rate;
	int					tick_state;	// > 0 = clock tick adjustment active, 0 = not active
	int					freq_state;	// 0 = frequency adjustment active, 0 = not active
	int					freq_maint;	// > 0 = frequency maintenance active, 0 = not active

} adj_state_t;

// object to manage frequency adjustment calculations (performed over sampling period FREQ_HISTORY_MAX)

typedef struct {
	int64			total_drift;		// total time drift over sample_period * sample_max
	int64			avg_movement;		// average drift per sample_period
	int64			samples[FREQ_HISTORY_MAX];
	int				samples_populated;	// 1 = sample queue has been populated
	int				sample_period;		// 1 = 1 Hz, etc.
	int				sample_period_cnt;
	int				sample_cnt;
	int				sample_max;

} drift_data_t;


// object to hold time and other information retrieved from card

typedef struct {

	// time/date as read from card

	unsigned char		hr;				
	unsigned char		min;
	unsigned char  		sec;
	unsigned int 		ms;
	unsigned char		mon;
	unsigned char		day;
	unsigned int		yr;

	unsigned char		dt_fresh;

	// time reference lock status:
	// 0 - lock invalid (never locked)
	// 1 - lock not present (had been locked, but lock is lost)
	// 2 - lock lost (one-time notification)
	// 3 - lock restored (one-time notification)
	// 4 - lock valid

	unsigned char		rtc_status; 		// real-time clock, one of lock status
	unsigned char		gps_status;			// GPS, one of lock status
	unsigned char		tc_status; 			// time code, one of lock status
	unsigned char		hso_status;			// HSO, one of lock status
	unsigned char		hso_achievement;	// precision achievement (0 - 16)

	unsigned char		prior_gps;			// prior GPS status, one of lock status
	unsigned char		prior_tc;			// prior time code status, one of lock status

	unsigned char		state_fresh;

} card_state_t;	

// object to manage data reported via the API
//
// note: information here is duplicated from elsewhere, and we're doing that to maintain
// synchronized access to the data as an API user will call for the data at unpredicable
// times

typedef struct {

	unsigned char		rtc_status; 		// real-time clock, one of lock status
	unsigned char		gps_status;			// GPS, one of lock status
	unsigned char		tc_status; 			// time code, one of lock status
	unsigned char		hso_status;			// HSO, one of lock status
	unsigned char		hso_achievement;	// precision achievement (0 - 16)
 
	struct timespec		reftime;
	struct timespec		mono_capture;

	unsigned char		sync_mode;			// synchronization mode, one of SYNC_MODE_xxx
	int64				last_delta;			// last reference/system time delta

	pthread_mutex_t		mutex;				// mutex to synchronize access to the data in this structure

	// NMEA messages

	char				lastGGA[MAX_256B_BUFFER];

} api_report_t;

#endif // _GPSMCR_DEFS_H

