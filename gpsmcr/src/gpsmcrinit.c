#include <stdio.h>
#include <string.h>
#include "ourtypes.h"
#include "gpsmcrdefs.h"
#include "gpsmcrinit.h"
#include "gpsmcrlib.h"

//
// global configuration parameters
//

int jam_thresh;
int tick_thresh;
int tick_adjust;
long freq_thresh;
int freq_adjust;
bool tc_reader;
bool tc_has_date;
char current_debug_level;
char use_log_file;	
char do_not_fork;
char do_not_sync;

//
// global state management
//

int 				sync_mode;
pps_state_t 		pps_state;
card_state_t 		card_state;
jitter_calc_t		jitter;
drift_data_t		drift_data;
adj_state_t			adj_status;


//
//	Name:			parse_cmdline()
//
//	Description:	Parse command-line parameters.
//
//	Returns:		'true' if program should proceed, 'false' to exit

bool parse_cmdline(int argc, char *argv[])
{
	int		x;
	char	*p;
	
	for(x = 0; x < argc; x++)
	{
		if (!strncmp(argv[x],"-nolog",6))
		{
			use_log_file = 0;

			continue;
		}

		if (!strncmp(argv[x],"-nofork",7))
		{
			do_not_fork = 1;

			continue;
       	}

       	if (!strncmp(argv[x],"-debug=",7))
       	{
			p = strchr(argv[x],'=') + 1;

			current_debug_level = atoi(p);

			continue;
		}

		if (!strncmp(argv[x],"-nosync",7))
		{
			do_not_sync = 1;
			sync_mode = SYNC_DISABLED;

			continue;
		}

		if (!strncmp(argv[x], "-h", 2))
		{
			printf("gpsmcr command-line options:\n");
			printf("  -nofork			- do not fork process (run in current terminal session)\n");
			printf("  -nolog			- do not log (system log will still receive critical messages)\n");
			printf("  -nosync			- do not synchronize system time\n");

			return(false);
		}
   	}

	return(true);
}
