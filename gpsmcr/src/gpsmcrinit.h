#ifndef _GPSMCR_INIT_H
#define _GPSMCR_INIT_H

extern int jam_thresh;
extern int tick_thresh;
extern int tick_adjust;
extern long freq_thresh;
extern int freq_adjust;
extern bool tc_reader;
extern bool tc_has_date;
extern char current_debug_level;
extern char use_log_file;	
extern char do_not_fork;
extern char do_not_sync;
extern int sync_mode;
extern pps_state_t pps_state;
extern card_state_t card_state;
extern jitter_calc_t jitter;
extern drift_data_t	drift_data;
extern adj_state_t adj_status;

extern bool parse_cmdline(int argc, char *argv[]);

#endif // _GPSMCR_INIT_H
