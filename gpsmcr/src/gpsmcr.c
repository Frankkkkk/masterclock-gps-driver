#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <sys/file.h>
#include "ourtypes.h"
#include "gpsmcrdefs.h"
#include "gpsmcr.h"
#include "gpsmcrinit.h"
#include "gpsmcrlib.h"

//
//	GPSMCR
//		
//	Daemon appplication supporting time sychronization in Linux environment for PCIe-series boards.
//
//	Version history:		
//
//	0.1 	- Alpha version released for targeted customer testing.
//			- Does not yet support slew time adjustment which should be added.
//
//	0.2		- Added 'nosync' command-line option for debugging purposes.
//			- Properly implement do-not-fork option.
//			- System time skewing support.
//
//	0.3  	- API interface.		
//			- Correct some issues with error message display.
//			- Add code to prevent running multiple instances of GPSMCR.	
//
//	0.4		- Updates for proper handling of time code that does not include date information.
//          - Fixed issue where NMEA messages might not be correctly enabled resulting in failure
//			  to auto-detect PCIe-GPS.
//			- Update handling of timestamps to 64-bit epochal nanoseconds reducing the complication
//			  of math on time delta.
//
//

char device_path[512];
char serial_prefix[64];
char log_file[1024];
char process_file[1024];
char socket_file[1024];

// global thread management

char run_pps;
char run_main;
char run_remoteio;

char *device_list[MAX_SERIAL_DEVICES];
int device_cnt, device_avr, device_gps;
struct tm start_time;
char card_verMaj;
char card_verMin;
char card_verRev;
char card_hasGPS;
int diff_sample_cnt;
int ipc_sock_fd;
int ipc_conn[MAX_IPC_CONN];
struct sockaddr_un ipc_sock_addr;
int serA, serB;
api_report_t api_report;

int main(int argc, char *argv[])
{
	int 				x, res, process_fd;
	char				buf[MAX_1K_BUFFER], buf2[MAX_1K_BUFFER];
	pthread_t			th_ppsmon;
	pthread_t			th_remoteio;
	struct timespec		timeres;
	int					field_cnt, field_step; 
	char				code;		
	char				*fields[MAX_REPLY_FIELDS];
	struct termios		oldtio;
	struct tm			ref_tm;
	struct timespec 	sr_reftime;
	FILE				*err;
	void				*joinres;
	struct timex		timex_info;
	time_t				st;
	char 				nmea_buffer[MAX_1K_BUFFER];
	int64				reftime, systime, delta;

	memset(device_path,0,sizeof(device_path));
	memset(serial_prefix,0,sizeof(serial_prefix));
	memset(log_file,0,sizeof(log_file));
	memset(socket_file,0,sizeof(socket_file));
	memset(process_file,0,sizeof(process_file));

#ifdef _DEBUG
	current_debug_level = 3;
#else
	current_debug_level = 0;
#endif
	st = time(NULL);
	gmtime_r(&st,&start_time);

	do_not_fork = 0;
	do_not_sync = 0;
	use_log_file = 1;
	diff_sample_cnt = 0;
	sync_mode = SYNC_INIT;

	memset(buf,0,sizeof(buf));
	memset(buf2,0,sizeof(buf2));
	memset(&ipc_conn,0,sizeof(ipc_conn));
	memset(nmea_buffer,0,sizeof(nmea_buffer));

	parse_cmdline(argc, argv);

	//
	// read and parse configuration file
	//

	res = parse_conf(CFG_FILE);

	if (res < 0)
	{
		log_printf(MSG_ALWAYS,"error %d reading configuration file %s",res,CFG_FILE);

		exit(-1);
	}

	//
	// open system and internal log
	//

	openlog("gpsmcr",LOG_PID,LOG_DAEMON);

	// warn if unable to access/write application log file

	if (use_log_file)
	{
		err = fopen(log_file,"at");

		if (err == NULL)
		{
			use_log_file = 0;

			log_printf(MSG_ALWAYS,"unable to open application log file - check LogFile configuration item");
		}
		else
		{
			fclose(err);
		}
	}

	//
	// make sure that we do not have another instance of GPSMCR running
	//

	process_fd = open(process_file, O_CREAT | O_RDWR, 0666);

	if (process_fd == -1)
	{
		log_printf(MSG_ALWAYS,"unexpected failure to create/open process lock file: %s",strerror(errno));
		log_printf(MSG_ALWAYS,"Are you running the process as a sufficiently-privileged user?");

		return(-1);
	}

	res = flock(process_fd, LOCK_EX | LOCK_NB);

	if (res)
	{
    		if (errno == EWOULDBLOCK)
		{
			log_printf(MSG_ALWAYS,"another instance of gpsmcr is currently running, exiting now...");

			return(-1);
		}
	}

	//
	// initialize the inter-process communication socket
	//

	unlink(socket_file);

	memset(&ipc_sock_addr, 0, sizeof(ipc_sock_addr));
	ipc_sock_addr.sun_family = AF_UNIX;
	strncpy(ipc_sock_addr.sun_path, socket_file, sizeof(ipc_sock_addr.sun_path)-1);

	ipc_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);

	res = bind(ipc_sock_fd, (struct sockaddr *)&ipc_sock_addr, sizeof(ipc_sock_addr));

	if (res < 0) 
	{
		log_printf(MSG_ALWAYS,"bind() error for IPC socket: %s",strerror(errno));
		log_printf(MSG_ALWAYS,"Are you running the process as a sufficiently-privileged user?");

		return(-1);
	}

	res = listen(ipc_sock_fd, 5);

	if (res < 0)
	{
		log_printf(MSG_ALWAYS,"listen() error for IPC socket: %s",strerror(errno));

		return(-1);
	}	

	res = fcntl(ipc_sock_fd, F_SETFL, fcntl(ipc_sock_fd, F_GETFL, 0) | O_NONBLOCK);

	if (res < 0)
	{
		log_printf(MSG_ALWAYS,"fcntl() error for IPC socket: %s",strerror(errno));

		return(-1);
	}	

	//
	// begin startup procedure and locate/identify card
	//

	log_printf(MSG_ALWAYS,"GPSMCR startup");

	// read out the current clock tick rate (ticks / second)

	timex_info.modes = 0;
	ntp_adjtime(&timex_info);

	adj_status.tick_state = 0;
	adj_status.freq_state = 0;
	adj_status.freq_maint = 0;
	adj_status.prior_tick_rate = timex_info.tick;

	timex_info.modes = ADJ_TICK;
	res = ntp_adjtime(&timex_info);

	if (res < 0)
	{
		log_printf(MSG_ALWAYS,"error %d testing system clock adjustment privilege",res);
		log_printf(MSG_ALWAYS,"Are you running the process as a sufficiently-privileged user?");

		return(-1);
	}

	log_printf(MSG_DEBUG1,"current clock tick rate = %ld",adj_status.prior_tick_rate);

	clock_getres(CLOCK_MONOTONIC,&timeres);
	log_printf(MSG_DEBUG1,"monotonic clock resolution: s=%llu, ns=%ld",timeres.tv_sec,timeres.tv_nsec);

	clock_getres(CLOCK_REALTIME,&timeres);
	log_printf(MSG_DEBUG1,"real-time clock resolution: s=%llu, ns=%ld",timeres.tv_sec,timeres.tv_nsec);

	card_hasGPS = 0;

	device_cnt = get_serial_ports();

	if (device_cnt < 0)
	{
		log_printf(MSG_ALWAYS,"unexpected error enumerating serial devices - %d",device_cnt);

		exit(-1);
	}

	if (device_cnt == 0)
	{
		log_printf(MSG_ALWAYS,"unable to locate any serial devices matching prefix /%s/%s",
			device_path,serial_prefix);

		exit(-2);
	}

	if (current_debug_level >= MSG_DEBUG1)
	{
		log_printf(MSG_DEBUG1,"located serial devices:");

		for(x = 0; x < device_cnt; x++)
			log_printf(MSG_DEBUG1," -> %s",device_list[x]);
	}

	if (enumerate_serial() < 0)
	{
		log_printf(MSG_ALWAYS,"unable to auto-detect PCIe-GPS/MCR/TCR on any serial port");

		exit(-3);
	}

	if (card_hasGPS)
	{
		log_printf(MSG_ALWAYS,"PCIe-GPS located, firmware version %u.%u.%u",
			card_verMaj,card_verMin,card_verRev);
		log_printf(MSG_ALWAYS,"  primary communications port: %s",device_list[device_avr]);
		log_printf(MSG_ALWAYS,"  GPS communication port: %s",device_list[device_gps]);
	}
	else
	{	
		log_printf(MSG_ALWAYS,"PCIe-%s located, firmware version %u.%u.%u",
			tc_reader ? "TCR" : "MCR", card_verMaj,card_verMin,card_verRev);
		log_printf(MSG_ALWAYS,"  communications port: %s",device_list[device_avr]);
	}
	
	serA = setup_com_port(device_list[device_avr],BAUDRATE_AVR,&oldtio);

	if (serA < 0) 
	{
		log_printf(MSG_ALWAYS,"unexpected error %d opening primary communications port",errno);

		return(-3);
	}

	if (card_hasGPS)
	{
		serB = setup_com_port(device_list[device_gps],BAUDRATE_NMEA,&oldtio);

		if (serB < 0)
		{
			log_printf(MSG_ALWAYS,"unexpected error %d opening secondary communications port",errno);

			return(-3);
		}
	}
	
	if (tc_reader)
	{
		// time code is expected - so set some time code reading configuration relative to configuration file
		
		switch(tc_reader)
		{
			case TC_READER_IRIG:
			
			switch(tc_has_date)
			{
				default:
				case 0: // day of year only (default IRIG format)
				snprintf(buf2,sizeof(buf2)-1,"%u,%u",TC_SOURCE_DATE_NOT_INCLUDED, TC_SOURCE_DATE_IRIG_DOY);
				break;
				
				case 1: // day of year and year
				snprintf(buf2,sizeof(buf2)-1,"%u,%u",TC_SOURCE_DATE_NOT_INCLUDED, TC_SOURCE_DATE_IRIG_DOY_YY);
				break;
			}

			log_printf(MSG_ALWAYS,"PCIe-TCR: configured for IRIG, %s", tc_has_date ? "date enabled" : "date disabled");
						
			break;
			
			case TC_READER_SMPTE:
			
			switch(tc_has_date)
			{
				default:
				case 0: // no date in time code
				snprintf(buf2,sizeof(buf2)-1,"%u,%u",TC_SOURCE_DATE_NOT_INCLUDED, TC_SOURCE_DATE_IRIG_DOY);
				break;
				
				case 1: // Leitch format
				snprintf(buf2,sizeof(buf2)-1,"%u,%u",TC_SOURCE_DATE_LEITCH_FORMAT, TC_SOURCE_DATE_IRIG_DOY);
				break;
				
				case 2: // 309M (w/ time zone)
				snprintf(buf2,sizeof(buf2)-1,"%u,%u",TC_SOURCE_DATE_SMPTE_309M_TZ, TC_SOURCE_DATE_IRIG_DOY);
				break;				
				
				case 3: // 309M (w/o time zone)
				snprintf(buf2,sizeof(buf2)-1,"%u,%u",TC_SOURCE_DATE_SMPTE_309M_NOTZ, TC_SOURCE_DATE_IRIG_DOY);
				break;
			}		

			log_printf(MSG_ALWAYS,"PCIe-TCR: configured for SMPTE, %s", tc_has_date ? "date enabled" : "date disabled");
			
			break;			
		}						
		
		snprintf(buf,sizeof(buf)-1,"m<%s,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0>", buf2);		
		
		write(serA,buf,strlen(buf));
		
		tcdrain(serA); // wait for data to be transmitted
	}

	//
	// successful setup, so fork process
	//

	if (!do_not_fork)
	{
		res = fork();

		if (res == -1)
		{
			log_printf(MSG_ALWAYS,"process failed to fork, err=%d",errno);

			exit(-1);
		}

		if (res > 0)
		{
			// successful fork, parent process can go ahead and exit

			exit(0);
		}
	}

	//
	// initialize signal handling
	//

	if (signal(SIGTERM, catch_signals) == SIG_ERR)
	{
  		log_printf(MSG_ALWAYS,"unexpected failure registering to handle SIGTERM signal");
	}

	//
	// initialize card state status
	//

	card_state.rtc_status = LOCK_INVALID;
	card_state.gps_status = LOCK_INVALID;
	card_state.tc_status = LOCK_INVALID;
	card_state.dt_fresh = false;
	card_state.state_fresh = false;
	card_state.hso_achievement = 0;

	pthread_mutex_init(&api_report.mutex,NULL);

	pthread_mutex_init(&pps_state.process_tos_mtx,NULL);
	pthread_cond_init(&pps_state.process_tos,NULL);

	pps_state.fd = serA;
	run_pps = true;

	pthread_create(&th_ppsmon,NULL,&pps_monitor,&pps_state);

	run_remoteio = true;

	pthread_create(&th_remoteio,NULL,&remote_io,NULL);

	run_main = true;
	jitter.sample_cnt = 0;

	drift_data.total_drift = 0;
	drift_data.samples_populated = 0;
	drift_data.sample_period = 1;
	drift_data.sample_period_cnt = 0;
	drift_data.sample_cnt = 0;
	drift_data.sample_max = 10;

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// main operation thread / loop
	///////////////////////////////////////////////////////////////////////////////////////////////////

	while(run_main)
	{
		pthread_mutex_lock(&pps_state.process_tos_mtx);
		pthread_cond_wait(&pps_state.process_tos,&pps_state.process_tos_mtx);
		pthread_mutex_unlock(&pps_state.process_tos_mtx);

		log_printf(MSG_DEBUG2,"non-priority top of second processing:");

		usleep(10000); // sleep 10ms allowing time for response

		//
		// read and store current time/date
		//

		write(serA,"a",1);

		usleep(10000); // sleep 10ms allowing time for response

		memset(buf,0,sizeof(buf));

		res = read(serA,buf,sizeof(buf)-1);

		if (res > 0)
		{
			log_printf(MSG_DEBUG3,"time msg: %s",buf);

			field_cnt = parse_fields(buf,&code,fields,MAX_REPLY_FIELDS);

			if ((code == 'b') && (field_cnt >= 7))
			{
				// expecting code 'b' and at least 7 fields, success!

				card_state.mon = atoi(fields[0]);
				card_state.day = atoi(fields[1]);
				card_state.yr = atoi(fields[2]);
				card_state.hr = atoi(fields[3]);
				card_state.min = atoi(fields[4]);
				card_state.sec = atoi(fields[5]);
				card_state.ms = atoi(fields[6]);

				card_state.dt_fresh = 1;

				log_printf(MSG_DEBUG2,"card time: %02u:%02u:%02u.%u %02u/%02u/%04u",
					card_state.hr,card_state.min,card_state.sec,card_state.ms,
					card_state.mon,card_state.day,card_state.yr);
			}

			for(field_step = 0; field_step < field_cnt; field_step++) free(fields[field_step]);
		}

		//
		// read and store current operational status
		//

		write(serA,"d",1);

		usleep(10000); // sleep 10ms allowing time for response

		memset(buf,0,sizeof(buf));

		res = read(serA,buf,sizeof(buf)-1);

		if (res > 0)
		{
			log_printf(MSG_DEBUG3,"status msg: %s",buf);

			field_cnt = parse_fields(buf,&code,fields,MAX_REPLY_FIELDS);

			if ((code == 'e') && (field_cnt >= 18))
			{
				// expecting code 'e' and at least 18 fields, success!

				card_state.prior_gps = card_state.gps_status;
				card_state.prior_tc = card_state.tc_status;

				card_state.rtc_status = atoi(fields[1]);
				card_state.gps_status = atoi(fields[2]);
				card_state.tc_status = atoi(fields[3]);
				card_state.hso_status = atoi(fields[4]);
				card_state.hso_achievement = atoi(fields[5]);

				card_state.state_fresh = true;

				log_printf(MSG_DEBUG2,"card state: rtc=%u, gps=%u, tc=%u, hso=%u, hso_achieve=%u",
					card_state.rtc_status,
					card_state.gps_status,
					card_state.tc_status,
					card_state.hso_status,
					card_state.hso_achievement);
			}			

			for(field_step = 0; field_step < field_cnt; field_step++) free(fields[field_step]);			
		}

		//
		// if we don't have fresh time and status information, don't continue processing
		//

		if (!card_state.dt_fresh || !card_state.state_fresh)
			continue;

		//
		// obtain best estimate of reference time factoring latency measurements
		//

		ref_tm.tm_sec = card_state.sec;
		ref_tm.tm_min = card_state.min;
		ref_tm.tm_hour = card_state.hr;
		ref_tm.tm_mon = card_state.mon - 1;
		ref_tm.tm_mday = card_state.day;
		ref_tm.tm_year = card_state.yr - 1900;
		ref_tm.tm_isdst = 0;

		sr_reftime.tv_sec = GetSecondsSince1970(&ref_tm);
		sr_reftime.tv_nsec = 0;
				
		reftime = 1000000000 * sr_reftime.tv_sec;

		// make a copy of reference information for reporting via the API
		// note that subseconds is set to zero here because we are reporting
		// time from the last top-of-second

		pthread_mutex_lock(&api_report.mutex);

		api_report.rtc_status = card_state.rtc_status;
		api_report.gps_status = card_state.gps_status;
		api_report.tc_status = card_state.tc_status;
		api_report.hso_status = card_state.hso_status;
		api_report.hso_achievement = card_state.hso_achievement;

		api_report.reftime.tv_sec = sr_reftime.tv_sec;
		api_report.reftime.tv_nsec = sr_reftime.tv_nsec;
		api_report.mono_capture.tv_sec = pps_state.tp.tv_sec;
		api_report.mono_capture.tv_nsec = pps_state.tp.tv_nsec;

		pthread_mutex_unlock(&api_report.mutex);

		sync_critical_section(reftime, &systime, &delta);
		
		//
		// perform calculations for jitter smoothing and drift tracking
		//

		if (card_hasGPS)
		{
        	if ((card_state.gps_status == LOCK_PRESENT) || (card_state.gps_status == LOCK_RESTORED))
			{
				jitter_calculations(&jitter,delta);
				
				drift_calculations(&jitter,&drift_data);
			}
			
			if (card_state.prior_gps != card_state.gps_status)
			{
				if ((card_state.gps_status == LOCK_RESTORED) || 
			    	(card_state.gps_status == LOCK_PRESENT))
				{
					log_printf(MSG_ALWAYS,"GPS lock restored.");
				}
				else
				{
					log_printf(MSG_ALWAYS,"GPS lock lost.");
					
					sync_mode = SYNC_WAIT;
				}
			}
		}
		else
		if (tc_reader)
		{
        	if ((card_state.tc_status == LOCK_PRESENT) || (card_state.tc_status == LOCK_RESTORED))
			{
				jitter_calculations(&jitter,delta);

				drift_calculations(&jitter,&drift_data);
			}
			
			if (card_state.prior_tc != card_state.tc_status)
			{
				if ((card_state.tc_status == LOCK_RESTORED) || 
			    	(card_state.tc_status == LOCK_PRESENT))
				{
					log_printf(MSG_ALWAYS,"Time code lock restored.");
				}
				else
				{
					log_printf(MSG_ALWAYS,"Time code lock lost.");
					
					sync_mode = SYNC_WAIT;
				}
			}
		}
		else
		{
			jitter_calculations(&jitter,delta);

			drift_calculations(&jitter,&drift_data);
		}			

		pthread_mutex_lock(&api_report.mutex);

		api_report.sync_mode = sync_mode;
		api_report.last_delta = delta;

		pthread_mutex_unlock(&api_report.mutex);

		log_printf(MSG_DEBUG2,"time, reference=%lld, %.03f(us)  system=%lld, %.03f(us)",
			reftime / BILLION, (double)(reftime % BILLION) / 1000.0,
			systime / BILLION, (double)(systime % BILLION) / 1000.0);

		log_printf(MSG_DEBUG2,"absolute time delta=%lld, %.03f(us)",
			delta / BILLION, (double)(delta % BILLION) / 1000.0);

		if (jitter.sample_cnt == JITTER_HISTORY_MAX)
		{
			log_printf(MSG_DEBUG2,"jitter smoothed delta=%lld, %.03f(us)",
				jitter.smoothed / BILLION, (double)(jitter.smoothed % BILLION) / 1000.0);
		}

		if (card_hasGPS)
		{
			//
			// process NMEA messages
			//

			process_nmea(nmea_buffer,sizeof(nmea_buffer));
		}

		card_state.dt_fresh = false;
		card_state.state_fresh = false;
	}
	
	///////////////////////////////////////////////////////////////////////////////////////////////////
	// process shut-down
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// wait for the other threads to exit

	pthread_join(th_ppsmon,&joinres);
	pthread_join(th_remoteio,&joinres);

	// clean up the serial port list

	for(x = 0; x < device_cnt; free(device_list[x++]));

	// close and remove the IPC

	close(ipc_sock_fd);

	unlink(socket_file);

	// close and remove the process instance file

	close(process_fd);

	unlink(process_file);

	log_printf(MSG_DEBUG3,"main() exiting gracefully...");

	exit(0);
}


//
//	Name:			sync_critical_section()
//
//	Description:	Critical section for calculating and performing system clock synchronization.
//				
//					Do not add unnecessary code in this fuction.
//
//	Returns:		calculated system<->reference time differential in nanoseconds
//

void sync_critical_section(int64 reftime, int64 *systime, int64 *delta)
{
	struct timespec		sr_systime;
	struct timespec		sr_interval;
	bool				doSync = false;
	int64				interval;
	
	clock_gettime(CLOCK_REALTIME,&sr_systime);

	clock_gettime(CLOCK_MONOTONIC_RAW,&sr_interval);

	*systime = BILLION * sr_systime.tv_sec + sr_systime.tv_nsec;
		
	interval = BILLION * (sr_interval.tv_sec - pps_state.tp.tv_sec) + sr_interval.tv_nsec - pps_state.tp.tv_nsec;
		
	reftime += interval;
		
	*delta = *systime - reftime;

	if (do_not_sync) return;
	
	if (jitter.sample_cnt == JITTER_HISTORY_MAX)
	{
		if (card_hasGPS)
		{
			// it's a PCIe-GPS
					
			if ((card_state.gps_status == LOCK_PRESENT) || (card_state.gps_status == LOCK_RESTORED))
				doSync = true;
		}
		else
		if (tc_reader)
		{
			// it's a PCIe-TCR
					
			if ((card_state.tc_status == LOCK_PRESENT) || (card_state.tc_status == LOCK_RESTORED))
				doSync = true;
		}
		else
		{
			// it's a PCIe-OSC (we always sync)
					
			doSync = true;
		}
				
		if (doSync)
		{
			synchronize_system_clock(reftime,*delta);
		}
	}
	else
	{
		// still building out our jitter averaging array

		sync_mode = SYNC_INIT;
	}
}


//
//	Name:			pps_monitor()
//
//	Description:	Waits for DSR signal from card which indicates top-of-second.
//
//					Perform time critical top-of-second actions and signal the main thread
//					of the application to perform non-critical top-of-second actions.
//
//	Notes:			This is a separate thread of execution that runs concurrently with the main
//					application thread.
//

void *pps_monitor(void *arg)
{
	unsigned int			 	wait_signals;
	int						old_dsr;
	pps_state_t				*pps_state;
	struct serial_icounter_struct	mbits;

	pps_state = (pps_state_t *)arg;

	wait_signals = TIOCM_DSR;

	ioctl(pps_state->fd,TIOCGICOUNT,&mbits);

	old_dsr = mbits.dsr;

	while(run_pps)
	{
		// wait for serial signal

		if (ioctl(pps_state->fd, TIOCMIWAIT, wait_signals) < 0)
		{
			log_printf(MSG_ALWAYS,"unexpected error pps_monitor() / TIOCMIWAIT");

			run_pps = false;
		}

		// look for line condition change on DSR

		ioctl(pps_state->fd,TIOCGICOUNT,&mbits);

		if (mbits.dsr != old_dsr)
		{
			old_dsr = mbits.dsr;

			// we have top of second - capture high-performance timer
			// (note: we prefer to use CLOCK_MONOTONIC_RAW to measure short intervals
			// because it is not affected by any ongoing clock skew, if this is not available
			// we could use CLOCK_MONOTONIC with only very minor ramifications on precision)

			clock_gettime(CLOCK_MONOTONIC_RAW,&pps_state->tp);

			// signal non-priority top-of-second handling

			pthread_cond_signal(&pps_state->process_tos);
		}
	}

	log_printf(MSG_DEBUG3,"pps_monitor() - gracefully exiting...");

	return(NULL);
}


//
//	Name:			remote_io()
//
//	Description:	Handle I/O for remote clients.
//
//	Notes:			This is a separate thread of execution that runs concurrently with the main
//					application thread.
//

void *remote_io(void *arg)
{
	int		t, rd;
	char		in_buf[MAX_1K_BUFFER];

	memset(in_buf,0,sizeof(in_buf));

	while(run_remoteio)
	{
		manage_connections();

		t = 0;

		do
		{
			if (ipc_conn[t] != 0)
			{
				// this should be an open connection

				rd = read(ipc_conn[t],in_buf,sizeof(in_buf));

				switch(rd)
				{
					case 0:

					log_printf(MSG_DEBUG3,"remote session %d disconnected",t);

					close(ipc_conn[t]);

					ipc_conn[t] = 0;

					break;

					case -1:

					if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
					{
						// no data ready on this socket
					}
					else
					{
						log_printf(MSG_DEBUG3,"socket error %d on session %d",errno,t);

						close(ipc_conn[t]);

						ipc_conn[t] = 0;
					}

					break;

					default:

					remote_io_process(ipc_conn[t],in_buf);
				}
			}

			t++;

		} while(t < MAX_IPC_CONN);

		// sleep for 10ms to allow for context switching
		//
		// note: this is probably not the best way to handle this, later we'll look
		// having this thread sleep until signalled by an incoming connection or 
		// new data on an established connection

		usleep(10000); 
	}

	log_printf(MSG_DEBUG3,"remote_io() - gracefully exiting...");

	return(NULL);
}


//
//	Name:			manage_connections()
//
//	Description:	Manage inter-process socket connections for remote clients.
//
//	Note:			Remote client could be our status tool, or any custom remote client utilizing
//					our documented features such as status and time requests.
//

void manage_connections(void)
{
	int		t, fd, res;

	fd = accept(ipc_sock_fd,NULL,NULL);

	if (fd == -1) return;

	res = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	if (res < 0)
	{
		log_printf(MSG_ALWAYS,"manage_connections(): fcntl() error %d for IPC socket",res);

		return;
	}	

	t = 0;

	do 
	{
		if (ipc_conn[t] == 0)
		{
			ipc_conn[t] = fd;

			log_printf(MSG_DEBUG3,"remote session %d established",t);

			break;
		}

		t++;

	} while(t < MAX_IPC_CONN);
}


//
//	Name:			remote_io_process()
//
//	Description:	Process remote I/O.
//

void remote_io_process(int sock, char *in_buf)
{
	char			out_buf[MAX_1K_BUFFER];
	char			code;
	char			*rio_fields[MAX_REPLY_FIELDS];
	int			field_cnt, field_step;
	int			hour, minute, second, millisecond;
	int			month,day,year;
	char			ser_out[MAX_1K_BUFFER];

	memset(out_buf,0,sizeof(out_buf));
	memset(ser_out,0,sizeof(ser_out));

	field_cnt = parse_fields(in_buf,&code,rio_fields,MAX_REPLY_FIELDS);

	switch(code)
	{
		case 'A':

		snprintf(out_buf,sizeof(out_buf)-1,"A<%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%c>",
			GPSMCR_VER_MAJ,GPSMCR_VER_MIN,GPSMCR_VER_REV,
			card_verMaj,card_verMin,card_verRev,
			start_time.tm_hour,start_time.tm_min,start_time.tm_sec,
			start_time.tm_mon+1,start_time.tm_mday,start_time.tm_year+1900,
			card_hasGPS ? 'Y' : 'N');

		write(sock,out_buf,strlen(out_buf));

		break;

		case 'B':

		// aquire the mutex to synchronize access to this data

		pthread_mutex_lock(&api_report.mutex);

		snprintf(out_buf,sizeof(out_buf)-1,"B<%u,%u,%u,%u,%u>",
			api_report.rtc_status,
			api_report.gps_status,
			api_report.tc_status,
			api_report.hso_status,
			api_report.hso_achievement);

		pthread_mutex_unlock(&api_report.mutex);

		write(sock,out_buf,strlen(out_buf));

		break;

		case 'C':

		// aquire the mutex to synchronize access to this data

		pthread_mutex_lock(&api_report.mutex);

		snprintf(out_buf,sizeof(out_buf)-1,"C<%lu,%lu,%lu>",
			api_report.reftime.tv_sec,
			api_report.mono_capture.tv_sec,
			api_report.mono_capture.tv_nsec);

		pthread_mutex_unlock(&api_report.mutex);

		write(sock,out_buf,strlen(out_buf));

		break;

		case 'D':

		// aquire the mutex to synchronize access to this data

		pthread_mutex_lock(&api_report.mutex);

		snprintf(out_buf,sizeof(out_buf)-1,"D<%u,%lld>",
			api_report.sync_mode,
			api_report.last_delta);

		pthread_mutex_unlock(&api_report.mutex);

		write(sock,out_buf,strlen(out_buf));

		case 'E':

		// aquire the mutex to synchronize access to this data

		pthread_mutex_lock(&api_report.mutex);

		snprintf(out_buf,sizeof(out_buf)-1,"E<%s>",
			api_report.lastGGA);

		pthread_mutex_unlock(&api_report.mutex);

		write(sock,out_buf,strlen(out_buf));

		case 'F':

		if (field_cnt < 7)
		{
			// reject - not enough parameters

			char *reject = "Z<F,4>";

			write(sock,reject,strlen(reject));
	
			break;
		}
		
		month = atoi(rio_fields[0]);
		day = atoi(rio_fields[1]);
		year = atoi(rio_fields[2]);
		hour = atoi(rio_fields[3]);
		minute = atoi(rio_fields[4]);
		second = atoi(rio_fields[5]);
		millisecond = atol(rio_fields[6]);

		// range check all values

		if ((month < 1) || (month > 12) ||
			(day < 1) || (day > 31) ||
			(year < 1900) || (year > 2200) ||
			(hour < 0) || (hour > 23) ||
			(minute < 0) || (minute > 59) ||
			(second < 0) || (second > 59) ||
			(millisecond < 0) || (millisecond > 999) )
		{
			// one or more values out of range

			char *reject = "Z<F,2>";		
			
			write(sock,reject,strlen(reject));

			break;
		}

		snprintf(ser_out,sizeof(ser_out)-1,"c<%u,%u,%u,%u,%u,%u,%u>",
			month,day,year,hour,minute,second,millisecond);

		log_printf(MSG_DEBUG4,"AVR time set request: %s",ser_out);

		write(serA,ser_out,strlen(ser_out));

		break;
	}

	for(field_step = 0; field_step < field_cnt; field_step++) free(rio_fields[field_step]);
}


//
//	Name:			synchronize_system_clock()
//
//	Description:	Synchronizes system clock to reference time.  
//
//	Notes:			Synchronization method will be either jam sync or frequency adjustment,
//					relative to time differential and software configuration.
//

void synchronize_system_clock(int64 reftime, int64 delta)
{
	struct timex		timex_info;
	long				frequency, tick;
	struct timespec		lreftime;
	
	if (jam_thresh && ((llabs(delta) / BILLION) > jam_thresh))
	{
		//
		// here, we will jam sync the system clock
		//
		
		lreftime.tv_sec = reftime / BILLION;
		lreftime.tv_nsec = reftime % BILLION;

		clock_settime(CLOCK_REALTIME,&lreftime);

		sync_mode = SYNC_INIT;

		log_printf(MSG_ALWAYS,"jam sync: %02u:%02u:%02u:%uus %02u/%02u/%04u",
			card_state.hr,card_state.min,card_state.sec,(reftime % BILLION) / 1000,
			card_state.mon,card_state.day,card_state.yr);

		// remove any clock tick and frequency adjustments

		adj_status.tick_state = 0;

		timex_info.modes = ADJ_TICK | ADJ_FREQUENCY;
		timex_info.tick = adj_status.prior_tick_rate;
		timex_info.freq = 0;

		ntp_adjtime(&timex_info);
	}
	else
	if (tick_thresh && ((llabs(delta) / BILLION) > tick_thresh))
	{
		//
		// Here, we adjust the system clock tick rate.  
		// At this stage we are only interested in getting within our maintenance window,
		// so we are not looking at making stabilization refinements here.
		//
		// The maximum adjustment that can be applied here (+/-1000) results in a
		// change of +/-100 ms / second, this allows for a relatively rapid
		// frequency skew correction of time.

		if (!adj_status.tick_state)
		{
			if (delta < 0)
				tick = adj_status.prior_tick_rate - tick_adjust;
			else
				tick = adj_status.prior_tick_rate + tick_adjust;

			timex_info.modes = ADJ_TICK;
			timex_info.tick = tick;

			ntp_adjtime(&timex_info);

			sync_mode = SYNC_TICK;

			log_printf(MSG_DEBUG1,"clock tick rate adjustment to: %ld",tick);

			adj_status.tick_state = 50; // allow up to 50 corrections before we re-evaluate
		}
		else
			adj_status.tick_state--;		
	}
	else
	{
		if (adj_status.tick_state)
		{
			// remove any clock tick adjustments

			adj_status.tick_state = 0;

			timex_info.modes = ADJ_TICK;
			timex_info.tick = adj_status.prior_tick_rate;

			ntp_adjtime(&timex_info);

			log_printf(MSG_DEBUG1,"removed clock tick rate adjustment");
		}

		if (delta > freq_thresh)
		{	
			//
			// Here, we adjust system clock frequency offset to move system clock to reference.
			// At this stage we are only interested in getting within our maintenance window,
			// so we are not looking at making stabilization refinements here.
			//
			// The maximum frequency offset adjustment that can be applied (+/-500ppm) results
			// in a change of +/-500 us / second, this allows for slower and finer granularity
			// frequency skew correction of time.

			timex_info.modes = ADJ_FREQUENCY;

			if (llabs(delta) > BILLION)
				frequency = freq_adjust;
			else
				frequency = (llabs(delta) / 250000) + 100;
				
			if (delta > 0)
				timex_info.freq = (frequency << 16) * -1; 
			else
				timex_info.freq = (frequency << 16);

			timex_info.modes = ADJ_FREQUENCY;

			ntp_adjtime(&timex_info);

			sync_mode = SYNC_FREQ;

			log_printf(MSG_DEBUG1,"clock frequency adjustment to: %ld us/sec",timex_info.freq >> 16);
		}
		else
		{
			//
			// here, we are within our maintenance window, so the goal is to learn what frequency offset
			// will keep our delta closest to zero
			//

			// remove any clock tick adjustments

			adj_status.tick_state = 0;

			timex_info.modes = ADJ_TICK;
			timex_info.tick = adj_status.prior_tick_rate;

			ntp_adjtime(&timex_info);

			if (!adj_status.freq_maint)
			{
				adj_status.freq_maint = drift_data.sample_period * drift_data.sample_max;

				frequency = llabs(delta) / (drift_data.sample_period * drift_data.sample_max) / 2000;

				timex_info.modes = ADJ_FREQUENCY;

				if (delta > 0)
					timex_info.freq = (frequency << 16) * -1;
				else
					timex_info.freq = frequency << 16;

				ntp_adjtime(&timex_info);

				sync_mode = SYNC_MAINT;
				
				log_printf(MSG_DEBUG1,"maintenance: JSD=%lld, %.03f(us), drift(%ld)=%lld, %.03f(us), freq adjust/sec=%ld(us)",
					delta / BILLION, (double)(delta % BILLION) / 1000.0,
					drift_data.sample_period * drift_data.sample_max,
					drift_data.total_drift / BILLION, (double)(drift_data.total_drift % BILLION) / 1000.0,
					frequency);
			}
			else
				adj_status.freq_maint--;
		}
	}
}


//
//	Name:			jitter_calculations()
//
//	Description:	Performs jitter-related calculations on reference/system time differential.
//

void jitter_calculations(jitter_calc_t *j, int64 diff)
{
	int 	t;

	if (j->sample_cnt < JITTER_HISTORY_MAX)
	{
		j->samples[j->sample_cnt] = diff;

		j->sample_cnt++;
	}

	if (j->sample_cnt == JITTER_HISTORY_MAX)
	{
		// rotate the jitter history samples

		for(t = 0; t < JITTER_HISTORY_MAX-1; t++)
		{
			j->samples[t] = j->samples[t+1];
		}

		j->samples[JITTER_HISTORY_MAX-1] = diff;

		j->smoothed = 0;

		for(t = 0; t < JITTER_HISTORY_MAX; t++)
		{
			log_printf(MSG_DEBUG4,"jitter sample(%u): %lld, %.03f(us)",
				t,j->samples[t] / BILLION, (double)(j->samples[t] % BILLION) / 1000.0);

			j->smoothed += j->samples[t];
		}

		j->smoothed /= JITTER_HISTORY_MAX;
	}
}


//
//	Name:			drift_calculations()
//
//	Description:	Performs drift-related calculations on reference/system time differential.
//

void drift_calculations(jitter_calc_t *j, drift_data_t *d)
{
	int	t;

	// note that this calculation uses the jitter-smoothed differential, so we don't start calculations
	// until that averaging buffer is full

	if (j->sample_cnt < JITTER_HISTORY_MAX) return;

	if (++(d->sample_period_cnt) < d->sample_period) return;

	d->sample_period_cnt = 0;

	if ((d->sample_cnt == (d->sample_max - 1)) && d->samples_populated)
	{
		// rotate the samples

		for(t = 0; t < d->sample_max-1; t++)
		{
			d->samples[t] = d->samples[t+1];
		}
	}

	d->samples[d->sample_cnt] = j->smoothed;

	if (d->sample_cnt == (d->sample_max - 1))
	{
		// we have a full drift sample queue, perform calculations

		d->samples_populated = 1;

		d->total_drift = d->samples[d->sample_cnt] - d->samples[0];

		log_printf(MSG_DEBUG3,"smoothed movement over %u seconds=%lld, %.03f(us)",
			d->sample_period * d->sample_max,
			d->total_drift / BILLION, (double)(d->total_drift % BILLION) / 1000.0);

		for(t = 0; t <= d->sample_cnt; t++)
		{
			log_printf(MSG_DEBUG4,"frequency sample(%u)=%lld, %.03f(us)",
				t,
				d->samples[t] / BILLION,(double)(d->samples[t] % BILLION) / 1000.0);
		}
	}

	if (d->sample_cnt < (d->sample_max - 1)) d->sample_cnt++;

	
}


//
//	Name:		catch_signals()
//
//	Description:	Captures the TERM signal to initiate an orderly shutdown of the daemon.
//

void catch_signals(int sig)
{
	if (sig == SIGTERM)
	{
		run_remoteio = false;
		run_pps = false;
		run_main = false;
	}
}


//
//	Name:			enumerate_serial()
//
//	Description:	Locate the primary AVR communication port for MCR/GPS.  
//
//					Also look for NMEA messages on a secondary port, if found
//					indicates we have PCIe-GPS.
//

int enumerate_serial(void)
{
	device_avr = enumerate_serial_AVR();

	if (device_avr < 0) return(-1);

	device_gps = enumerate_serial_GPS();

	return(0);
}


//
//	Name:			enumerate_serial_AVR()
//
//	Description:	Find the primary (AVR) serial port.
//
//	Returns:		< 0 on error
//					>= 0, index to device name where found
//

int enumerate_serial_AVR(void)
{
	int 			t, t2, t3, fd, res;
	char			buf[MAX_1K_BUFFER], *p;
	char			code, *fields[MAX_REPLY_FIELDS];
	struct termios	oldtio;

	t = 0;

	while(t < device_cnt)
	{
		log_printf(MSG_DEBUG1,"enum AVR: %s",device_list[t]);

		fd = setup_com_port(device_list[t],BAUDRATE_AVR,&oldtio);

		if (fd < 0)
		{
			log_printf(MSG_DEBUG1,"enumerate_serial_AVR(), unable to open %s: %s",device_list[t],strerror(errno));

			t++;

			continue;
		}

		res = write(fd,"d",1);

		if (res <= 0)
		{
			log_printf(MSG_DEBUG1,"enum AVR: unexpected serial write error: %s",strerror(errno));
		}
		
		usleep(20000); // wait 20ms

		memset(buf,0,sizeof(buf)-1);
          
		res = read(fd,buf,sizeof(buf)-1);

		if (res < 0)
		{
			log_printf(MSG_DEBUG1,"enum AVR: unexpected serial read error: %s",strerror(errno));
		}
		else
		if (res > 0)
		{
			p = strstr(buf,"e<"); 

			if (p)
			{
				// looks like we have a status response, parse for firmware version

				t2 = parse_fields(buf,&code,fields,MAX_REPLY_FIELDS);

				if (t2 >= 18)
				{
					// expecting at least 18 fields, success!

					card_verMaj = atoi(fields[11]);
					card_verMin = atoi(fields[12]);
					card_verRev = atoi(fields[13]);

					log_printf(MSG_DEBUG1,"enum AVR: found card, maj=%u, min=%u, rev=%u",
						card_verMaj,card_verMin,card_verRev);

					for(t3 = 0; t3 < t2; t3++) free(fields[t3]);

					// attempt to turn on some NMEA messages that we want to see for GPS operation
					// this also allows us to auto-detect later that the card has a GPS receiver					
					
					char *pmsg = "p<1,9600,1,1,2,3,4,5,6,7,8>";
					write(fd,pmsg,strlen(pmsg));
					
					tcdrain(fd); // wait for data to be transmitted

					release_com_port(fd,&oldtio);
					
					return(t);	
				}			

			}	
			else
			{
				log_printf(MSG_DEBUG3,"enum AVR: got some data (%d bytes), but not recognized",res);
			}         	
          }
	
		release_com_port(fd,&oldtio);

		t++;
	}

	return(-1);
}


//
//	Name:			enumerate_serial_GPS()
//
//	Description:	Find the secondary (NMEA) serial port.
//
//	Notes:			This will not be present if installed card is PCIe-MCR
//
//	Returns:		< 0 on error or not found (normal when installed card is PCIe-MCR)
//					>= 0, index to device name where found
//

int enumerate_serial_GPS(void)
{
	int 			t, fd, res;
	char			buf[MAX_1K_BUFFER];
	struct termios	oldtio;

	t = 0;

	while(t < device_cnt)
	{
		log_printf(MSG_DEBUG1,"enum GPS: %s",device_list[t]);

		fd = setup_com_port(device_list[t],BAUDRATE_NMEA,&oldtio);

		if (fd == -1)
		{
			log_printf(MSG_DEBUG1,"enumerate_serial_GPS(), unable to open %s: %s",device_list[t],strerror(errno));
			
			t++;

			continue;
		}

		usleep(1200000); // wait 1.2 seconds

		memset(buf,0,sizeof(buf));

          res = read(fd,buf,sizeof(buf)-1);

		if (res < 0)
		{
			log_printf(MSG_DEBUG1,"enum GPS: unexpected serial read error: %s",strerror(errno));
		}
		else
		if (res > 0)
		{

			if (strstr(buf,"$GPZDA"))
			{
				card_hasGPS = 1;

				log_printf(MSG_DEBUG1,"enum GPS: found PCIe-GPS!");

				release_com_port(fd,&oldtio);

				return(t);
			}
			else
			{
				log_printf(MSG_DEBUG1,"enum GPS: got %d data bytes, but not recognized",res);
			}         	
          }

		release_com_port(fd,&oldtio);

		t++;
	}

	return(-1);
}


//
//		Name:		process_nmea()
//
//		Description:	Handle queuing and processing for NMEA message (PCIe-GPS only)
//

void process_nmea(char *nmea_buffer, unsigned int nmea_buffer_size)
{
	char		*p1, *p2;
	char		tmpbuf1[MAX_1K_BUFFER], tmpbuf2[MAX_1K_BUFFER];

	memset(tmpbuf1,0,sizeof(tmpbuf1));
	memset(tmpbuf2,0,sizeof(tmpbuf2));

	// we may have some left-over partial message data from the last serial read, so copy
	// that into the buffer now before we start a new read

	strncpy(tmpbuf1,nmea_buffer,sizeof(tmpbuf1)-1);

	if (strlen(tmpbuf1))
	{
		read(serB,tmpbuf2,sizeof(tmpbuf2)-strlen(tmpbuf1)-1);

		strncat(tmpbuf1,tmpbuf2,sizeof(tmpbuf1)-1);
	}
	else
	{
		read(serB,tmpbuf1,sizeof(tmpbuf1)-1);
	}

	// process all complete NMEA messages in the buffer

	p1 = strchr(tmpbuf1,'$');

	while(p1)
	{
		p2 = strchr(p1,'\r');

		if (p2)
		{
			*p2 = '\0';

			if (*(p2+1) == '\n') p2++;

			*p2 = '\0';

			// process here

			log_printf(MSG_DEBUG5,"NMEA: %s",p1);

			if (!strncmp(p1,"$GPGGA",6))
			{
				pthread_mutex_lock(&api_report.mutex);

				memset(api_report.lastGGA,0,sizeof(api_report.lastGGA));
				strncpy(api_report.lastGGA,p1,sizeof(api_report.lastGGA)-1);

				pthread_mutex_unlock(&api_report.mutex);
			}

			if (*(p2 + 1) == '\0')
			{
				nmea_buffer[0] = '\0';

				break;
			}

			p1 = strchr(p2+1,'$');
		}
		else
		{
			// if this happens, we have a partial message at the tail of our buffer which we will
			// preserve until the next processing iteration

			strncpy(nmea_buffer,p1,nmea_buffer_size-1);

			break;
		}
	}
}


//
//	Name:		get_serial_ports()
//
//	Description:	Build a list of serial devices on the system, matching a prefix.
//				Linux serial ports are typically ttyS0, ttyS1, etc.
//
//	Notes:		Later on we may want to cull this list by attempting to determine
//				which devices assigned to the Pericom chip.
//       

int get_serial_ports(void)
{
	int			t, fspec_size;
	char			*p;
	DIR			*ff;
	struct dirent	*ffData;

	t = 0;

	ff = opendir(device_path);

	if (ff == NULL)
	{
		return(-1);
	}

	while(((ffData = readdir(ff)) != NULL) && (t < MAX_SERIAL_DEVICES))
	{
		if (!strncmp(ffData->d_name,serial_prefix,strlen(serial_prefix)))
		{
			fspec_size = strlen(device_path) + 1 + strlen(ffData->d_name) + 1;

			p = calloc(1,fspec_size);

			if (!p) return (-2);

			// yes, it matches, add it to our list

			snprintf(p,fspec_size,"%s/%s",device_path,ffData->d_name);

			device_list[t++] = p;
		}
	}
	
	closedir(ff);

	return(t);
}


int setup_com_port(const char *port, tcflag_t baud, struct termios *oldtio)
{
	int			fd;
	struct termios	newtio;

	memset(&newtio,0,sizeof(newtio));

  	fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);

	if (fd < 0) { return(fd); }

	tcgetattr(fd,oldtio); // save current port settings

	fcntl(fd,F_SETFL,FNDELAY); // make read return if no characters available in rx buffer

     // set baud rate

	cfsetispeed(&newtio,baud);
	cfsetospeed(&newtio,baud);

	// 8 data bits, no parity, 1 stop bit

	newtio.c_cflag &= ~PARENB;
	newtio.c_cflag &= ~CSTOPB;
	newtio.c_cflag &= ~CSIZE;
	newtio.c_cflag |= CS8;

	newtio.c_cflag &= ~(CSIZE & PARENB & CSTOPB);	
	newtio.c_cflag |= CS8;

     newtio.c_cflag |= (CLOCAL | CREAD);

	// no hardware or software flow control

#ifdef CNEW_RTSCTS
	newtio.c_cflag &= ~CNEW_RTSCTS;
#endif
	newtio.c_iflag &= ~(IXON | IXOFF | IXANY);

	// non-canoncial, raw input

	newtio.c_lflag &= ~(ICANON | ECHO | ECHONL | ECHOE | ISIG | IEXTEN);	

     newtio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL | IXON);

     newtio.c_oflag = ~OPOST;

     tcsetattr(fd,TCSAFLUSH,&newtio); 

	return(fd);
}

int release_com_port(int fd, struct termios *tio)
{
	// restore previous port configuration

	tcsetattr(fd,TCSANOW,tio);

	close(fd);

	return(0);
}


//
//	Name:			parse_conf()
//
//	Description:		Read and parse configuration file.
//

int parse_conf(char *conf)
{
   	unsigned short	gotcfg, linecnt;
   	FILE			*f;
   	char			tmp[MAX_1K_BUFFER];
   	char			*p;

	memset(tmp,0,sizeof(tmp));

   	if ((f = fopen(conf,"rt")) == NULL)
   	{
     	return(-1);
   	}

   	linecnt = gotcfg = 0;

   	while(fgets(tmp,sizeof(tmp)-1,f) != NULL)
	{
		linecnt++;

		if ((tmp[strlen(tmp)-1] == '\r') || (tmp[strlen(tmp)-1] == '\n'))
			tmp[strlen(tmp)-1] = '\0';

		if ((tmp[strlen(tmp)-1] == '\r') || (tmp[strlen(tmp)-1] == '\n'))
			tmp[strlen(tmp)-1] = '\0';

        if ((tmp[0] == '#') || !strlen(tmp)) continue;

        if (!strncmp(tmp,"DevicePath",10))
	   	{
			p = strchr(tmp,'=') + 1;

           	strncpy(device_path,p,sizeof(device_path)-1);

           	gotcfg |= CFG_DEVICE_PATH;
       	}
		else
        if (!strncmp(tmp,"SerialDevices",13))
	   	{
			p = strchr(tmp,'=') + 1;

           	strncpy(serial_prefix,p,sizeof(serial_prefix)-1);

           	gotcfg |= CFG_SERIAL_PREFIX;
       	}
		else
        if (!strncmp(tmp,"LogFile",7))
	   	{
			p = strchr(tmp,'=') + 1;

           	strncpy(log_file,p,sizeof(log_file)-1);

           	gotcfg |= CFG_LOG_FILE;
       	}
		else
       	if (!strncmp(tmp,"ProcessFile",7))
	   	{
			p = strchr(tmp,'=') + 1;

           	strncpy(process_file,p,sizeof(process_file)-1);

           	gotcfg |= CFG_PROCESS_FILE;
       	}
		else
       	if (!strncmp(tmp,"SocketFile",8))
	   	{
			p = strchr(tmp,'=') + 1;

           	strncpy(socket_file,p,sizeof(socket_file)-1);

           	gotcfg |= CFG_SOCKET_FILE;
       	}
		else
		if (!strncmp(tmp,"JamThreshold",12))
		{
			p = strchr(tmp,'=') + 1;

			if (p)
			{
				jam_thresh = atoi(p);

				gotcfg |= CFG_JAM;
			}
		}
		else
		if (!strncmp(tmp,"TickThreshold",13))
		{
			p = strchr(tmp,'=') + 1;

			if (p)
			{
				tick_thresh = atoi(p);

				gotcfg |= CFG_TICK_THRESH;
			}
		}
		else
		if (!strncmp(tmp,"TickAdjustment",12))
		{
			p = strchr(tmp,'=') + 1;

			if (p)
			{
				tick_adjust = atoi(p);

				if (tick_adjust > 1000) 
				{
          			log_printf(MSG_ALWAYS,"parse_conf(): error in configuration file, field 'TickAdjustment' cannot exceed 1000.");

           			fclose(f);

           			return(-3);
				}

				gotcfg |= CFG_TICK_ADJ;
			}
		}
       	else
		if (!strncmp(tmp,"FrequencyThreshold",18))
		{
			p = strchr(tmp,'=') + 1;

			if (p)
			{
				freq_thresh = atoi(p);

				if (freq_thresh < 1000) 
				{
          			log_printf(MSG_ALWAYS,"parse_conf(): error in configuration file, field 'FrequencyThreshold' cannot be less than 1000.");

           			fclose(f);

           			return(-3);
				}

				gotcfg |= CFG_FREQ_THRESH;
			}
		}
       	else
		if (!strncmp(tmp,"FrequencyAdjustment",19))
		{
			p = strchr(tmp,'=') + 1;

			if (p)
			{
				freq_adjust = atoi(p);

				if (freq_adjust > 500) 
				{
          			log_printf(MSG_ALWAYS,"parse_conf(): error in configuration file, field 'FrequencyAdjustment' cannot exceed 500.");

           			fclose(f);

           			return(-3);
				}

				gotcfg |= CFG_FREQ_ADJ;
			}
		}
		else
		if (!strncmp(tmp,"TimeCodeReader",14))
		{
			p = strchr(tmp,'=') + 1;

			if (p)
			{
				tc_reader = atoi(p);
				
				if (!((tc_reader == TC_READER_NONE) ||
					  (tc_reader == TC_READER_IRIG) ||
					  (tc_reader == TC_READER_SMPTE)))
				{
					log_printf(MSG_ALWAYS,"parse_conf(): error in configuration file, field 'TimeCodeReader' has an undefined option.");

           			fclose(f);

           			return(-3);
           		}

				gotcfg |= CFG_TC_READER;
			}
		}
    	else
		if (!strncmp(tmp,"TimeCodeHasDate",15))
		{
			p = strchr(tmp,'=') + 1;

			if (p)
			{
				tc_has_date = atoi(p);
				
				if (tc_has_date > 3)
				{
					log_printf(MSG_ALWAYS,"parse_conf(): error in configuration file, field 'TimeCodeHasDate' has an undefined option.");

           			fclose(f);

           			return(-3);		
				}

				gotcfg |= CFG_TC_HAS_DATE;
			}
		}
       	else
       	{
       		log_printf(MSG_ALWAYS,"parse_conf(): error in configuration file at line %d: %s",
      			linecnt,tmp);

       		fclose(f);

       		return(-2);
		}
	}

    if (gotcfg != CFG_ALL)
    {
        log_printf(MSG_ALWAYS,"parse_conf() - configuration file error, an item is invalid or missing.");

        fclose(f);

        return(-3);
    }

    fclose(f);

	return(0);
}






//
//		Name:		log_printf()
//
//		Description:	Performs logging and debugging message output.
//
//					'debug_level' parameter indicates whether or not the message should be output.
//
//					When enabled, messages will be sent to our own log file.  Useful for debugging,
//					especially when we are sending more debugging messages which we don't want
//					clogging the system log.
//
//					Only messages of type 'MSG_ALWAYS' will be sent to the system log.  					
//					

void log_printf(char debug_level, char *fmt, ...)
{
	char		buf[MAX_1K_BUFFER];
	FILE		*err;
	time_t	timestamp;
	struct tm	inttime;
	va_list	arglist;

	memset(buf,0,sizeof(buf));

	va_start(arglist,fmt);
	vsnprintf(buf,sizeof(buf)-1,fmt,arglist);
	va_end(arglist);

	// if not forked, all enabled messages also go to 'stdout'

	if (do_not_fork && (debug_level <= current_debug_level))
	{
		puts(buf);
		fflush(stdout);
	}

	// if log file open, all enabled messages to log file

	if (use_log_file && (debug_level <= current_debug_level))
	{
		err = fopen(log_file,"at");

		if (err)
		{
			time(&timestamp);
			gmtime_r(&timestamp,&inttime);

			fprintf(err,"[%02u:%02u:%02u %02u/%02u/%02u] %s\n",
				inttime.tm_hour,inttime.tm_min,inttime.tm_sec,
				inttime.tm_mon+1,inttime.tm_mday,
				inttime.tm_year > 99 ? inttime.tm_year - 100: inttime.tm_year,
				buf);

			fflush(err);
			fclose(err);
		}
	}

	// if message is level 0 (critical), send also to system log

	if (debug_level == 0)
	{
		syslog(LOG_NOTICE,buf);
	}
}

