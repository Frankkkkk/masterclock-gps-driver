#include <time.h>
#include <string.h>
#include <stdlib.h>
#include "gpsmcrlib.h"

const unsigned short _daysMonthElapsedNonLeapYear[13] = {
	0,
	31,	/* through January									*/
	59,	/* through February (not including any leap year)			*/
	90,	/* through March									*/
	120,	/* through April									*/
	151,	/* through May										*/
	181,	/* through June									*/
	212,	/* through July									*/
	243,	/* through August									*/
	273,	/* through September								*/
	304,	/* through October									*/
	334,	/* through November									*/
	365	/* through December									*/
};

const unsigned short _daysMonthElapsedLeapYear[13] = {
	0,
	31,	/* through January									*/
	60,	/* through February (this and rest include leap			*/
	91,	/* through March									*/
	121,	/* through April									*/
	152,	/* through May										*/
	182,	/* through June									*/
	213,	/* through July									*/
	244,	/* through August									*/
	274,	/* through September								*/
	305,	/* through October									*/
	335,	/* through November									*/
	366	/* through December									*/
};

#if 0

//
//		Name:			clock_difftime()
//
//		Description:		Performs t2 - t1 on time values in 'timespec' structure.
//
//		Notes:			If the difference is negative, both members of 'timespec' will be
//						signed negative.
//

void clock_difftime(struct timespec *t1, struct timespec *t2, struct timespec *result)
{
	result->tv_sec = t2->tv_sec - t1->tv_sec;

	if (t1->tv_sec < t2->tv_sec)
	{
		if (t1->tv_nsec > t2->tv_nsec)
		{ 
			result->tv_nsec = 1000000000 - t1->tv_nsec + t2->tv_nsec;
			result->tv_sec--;
		}
		else
			result->tv_nsec = t2->tv_nsec - t1->tv_nsec;
	}
	else
	if (t1->tv_sec > t2->tv_sec)
	{
		if (t1->tv_nsec < t2->tv_nsec)
		{
			result->tv_nsec = (1000000000 - t2->tv_nsec + t1->tv_nsec) * -1;
			result->tv_sec++;
		}
		else
			result->tv_nsec = t2->tv_nsec - t1->tv_nsec;
	}
	else
	{
		if (t1->tv_nsec > t2->tv_nsec)
			result->tv_nsec = (t1->tv_nsec - t2->tv_nsec) * -1;
		else
			result->tv_nsec = t2->tv_nsec - t1->tv_nsec;

	}
}

#endif

//
//	Name:			parse_fields()
//
//	Description:	Read and parse ASCII reply fields.
//

int parse_fields(char *buf, char *code, char *fields[], int max_fields)
{
	int		t;
	char		*p, *p2;

	if (!strlen(buf)) return(-1);

	p = strchr(buf,'<');

	if (p == buf)
	{
		// odd situation where < is the first character in our buffer, in which case
		// we'll try to find a next occurance

		p = strchr(buf+1,'<');
	}

	if (!p) 
	{
		if (strlen(buf)) *code = buf[0];

		return(0);
	}

	*code = *(p - 1);

	p++; p2 = p; t = 0;

	while((*p != '>') && (*p != '\0') && (t < max_fields))
	{
		p2 = strchr(p,',');

		if (!p2) p2 = strchr(p,'>');

		if (p2)
		{
			*p2 = '\0';

			fields[t] = calloc(1,strlen(p)+1);

			strcpy(fields[t],p);

			t++;

			p = p2 + 1;
		}
		else
			p++;
	}

	return(t);
}


/*
 .		Name:		GetSecondsSince1970()
 .
 .		Description:	Get seconds since 00:00:00 January 1, 1970 to current time/date
 .
 */

time_t GetSecondsSince1970(struct tm *in_time)
{
	time_t retval;

	retval  = GetDaysSince1970(in_time,1) - 1;		/* don't count the current day */

	retval = retval * 86400;						/* convert to seconds */

	retval += ((time_t)in_time->tm_hour * 3600) + 
			((time_t)in_time->tm_min * 60) + 
                (time_t)in_time->tm_sec;	/* add the current time of day */

	return(retval);
}


/*
 .		Name:		GetDaysSince1970()
 .
 .		Description:	Returns number of days elapsed since January 1, 1970.  If
 .					'thisMonth' is FALSE GetDaysSince1970() will not add
 .					the days elapsed in the current month to the value
 .					it returns.
 .
 .		Note:		The current day (although not necessarily elapsed) is also included
 .					in the value returned.
 .
*/

time_t GetDaysSince1970(struct tm *in_time, unsigned short thisMonth)
{
	time_t ulRetval;
	time_t ulDays;
	time_t ulYear;

	if (in_time->tm_mon > 11)
		return(-1);

   /* account for days in years since 1970
	   note: this simple formula works because year 2000 happens to be a leap year
	 */

	ulYear   = (time_t)in_time->tm_year - 70;
	ulDays   = (ulYear * 365) + ((ulYear + 2) >> 2);
	ulRetval = ulDays;

	/* account for days in months this year */

	ulRetval += _daysMonthElapsedNonLeapYear[in_time->tm_mon];

	if (IsLeapYear(in_time->tm_year) && (in_time->tm_mon < 2))
		ulRetval--;

	/* account for days in the current month */

	if (thisMonth)
		ulRetval += in_time->tm_mday;

	return(ulRetval);
}


/*
 .    Name:          IsLeapYear()
 .
 .    Description:   Determines if the year is a leap year
 .
 .    Returns:       0 if not a leap year
 .                   1 if a leap year
 */

int IsLeapYear(unsigned short year)
{
   if ((year % 100) == 0)
   {
      /* the year is the beginning of a new century, must be evenly
         divisible by 400 to be a leap year */

      if ((year % 400) == 0)
         return(1);
      else
         return(0);
   }
   else
   {
      /* the year must be evenly divisible by four to be a leap year */

      if ((year % 4) == 0)
         return(1);
      else
         return(0);
   }
}

