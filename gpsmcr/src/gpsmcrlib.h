#ifndef _GPSMCRLIB_H
#define _GPSMCRLIB_H

// time reference lock status states

#define LOCK_INVALID		0
#define LOCK_NOT_PRESENT	1
#define LOCK_LOST			2
#define LOCK_RESTORED		3
#define LOCK_PRESENT		4

// daemon reference -> system time synchronization modes

#define SYNC_DISABLED		0		// synchronization disabled (by command-line option)
#define SYNC_WAIT			1		// waiting for locked reference time
#define SYNC_INIT			2		// initializing algorithms
#define SYNC_TICK			3		// clock tick rate adjustment
#define SYNC_FREQ			4		// clock frequency adjustment
#define SYNC_MAINT			5		// clock frequency maintenance (system time accurate to reference)

extern void clock_difftime(struct timespec *t1, struct timespec *t2, struct timespec *result);
int parse_fields(char *buf, char *code, char *fields[], int max_fields);
extern time_t GetSecondsSince1970(struct tm *in_time);
extern time_t GetDaysSince1970(struct tm *in_time, unsigned short thisMonth);
extern int IsLeapYear(unsigned short year);

extern const unsigned short _daysMonthElapsedNonLeapYear[13];
extern const unsigned short _daysMonthElapsedLeapYear[13];

#endif // _GPSMCRLIB_H

