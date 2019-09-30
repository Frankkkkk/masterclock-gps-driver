#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>
#include <sys/timex.h>
#include <unistd.h>
#include <time.h>
#include "ourtypes.h"
#include "gmtool.h"
#include "gpsmcrlib.h"

//
//	GMTOOL
//		
//   Client tool for GPSMCR software to monitor daemon status and demonstrate
//   API.
//
//	Version history:		
//
//	0.1.0 	- Alpha version released for targeted customer testing.
//
//	0.1.1	- Slight display wording revisions.
//
//	0.2.0 	- Bug fixes on output.
//
//  0.3.0	- Enhance status output for reference sources. 
//			  Update for display of system<->reference time differential.
//

const char *lock_desc[6] = {
	"Unlocked",
	"Unlocked",
	"Unlocked",
	"Locked",
	"Locked",
	"Unknown"
};

const char *sync_desc[6] = {
	"Disabled                   ",
	"Waiting for Lock           ",
	"OK (Running)               ",
	"Clock Tick Adjustment      ",
	"Clock Frequency Adjustment ",
	"Clock Frequency Maintenance"
};

char socket_file[1024];

int main(int argc, char *argv[]) 
{
	struct sockaddr_un 	ipc_addr;
	int					ipc_sock;
	char 				*p, buf[1024];
	int 				x, res;
	int					field_cnt, field_step; 
	unsigned short		rtc_status, gps_status, tc_status, hso_status, hso_achieve, sync_mode, has_GPS;
	char				code, *fields[MAX_REPLY_FIELDS];
	int64				rt_in, rt_out, rt_interval_beg;
	struct tm			rt_out_tm, systime_tm;
	time_t				rt_out_timet;
	struct timespec		rt_interval_end, systime;
	int64				rt_interval, delta;
	char				lat_string[48], lat_dir[12]; 
	char				long_string[48], long_dir[12];
	char				altitude[12];
	int					fix_quality;
	int					satellites;
	int					setcardsys, setcardntp, setcardman, show_forever;
	int					ntp_sock;
	NTPTIME				ntp;
	char				ntp_server[256];
	int					ntp_port;
	struct sockaddr_in	ntp_addr;

	memset(socket_file,0,sizeof(socket_file));
	memset(buf,0,sizeof(buf));
	memset(lat_string,0,sizeof(lat_string));
	memset(lat_dir,0,sizeof(lat_dir));
	memset(long_string,0,sizeof(long_string));
	memset(long_dir,0,sizeof(long_dir));
	memset(altitude,0,sizeof(altitude));
	memset(ntp_server,0,sizeof(ntp_server));

	setcardsys = setcardntp = setcardman = 0;
	show_forever = 1; //While true

	printf("GMTOOL for PCIe-GPS/PCIe-TCR/PCIe-OCS (version %u.%u.%u)\n",
		GMTOOL_VER_MAJ,GMTOOL_VER_MIN,GMTOOL_VER_REV);

	//
	// read and parse configuration file
	//

	res = parse_conf(CFG_FILE);

	if (res < 0) return(-1);

	//
	// connect to the daemon's inter-process communication socket
	//

	if ((ipc_sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) 
	{
    		perror("socket error");
    		return(-1);
  	}

	memset(&ipc_addr, 0, sizeof(ipc_addr));
	ipc_addr.sun_family = AF_UNIX;
	strncpy(ipc_addr.sun_path,socket_file,sizeof(ipc_addr.sun_path)-1);

  	if (connect(ipc_sock, (struct sockaddr*)&ipc_addr, sizeof(ipc_addr)) == -1) 
	{
	    perror("connect error");

	    return(-1);
	}

	//
	// read and print out some useful information
	//

	write(ipc_sock,"A",1);

	res = read(ipc_sock,buf,sizeof(buf)-1);

	field_cnt = parse_fields(buf,&code,fields,MAX_REPLY_FIELDS);

	if ((code == 'A') && (field_cnt >= 12))
	{
		// expecting code 'A' and at least 12 fields, success!

		printf("\nCard firmware: %u.%u.%u\n\n",
			atoi(fields[3]),atoi(fields[4]),atoi(fields[5]));

		printf("Daemon version: %u.%u.%u  Start time (UTC): %02u:%02u:%02u %02u/%02u/%04u\n\n",
			atoi(fields[0]),atoi(fields[1]),atoi(fields[2]),
			atoi(fields[6]),atoi(fields[7]),atoi(fields[8]),
			atoi(fields[9]),atoi(fields[10]),atoi(fields[11]));

		if (fields[12][0] == 'Y')
			has_GPS = 1;
		else
			has_GPS = 0;
	}

	for(field_step = 0; field_step < field_cnt; field_step++) free(fields[field_step]);

	fflush(stdout);

	//
   	// parse command-line parameters
	//

	for(x = 0; x < argc; x++)
	{
		if (!strncmp(argv[x],"-setcardsys",11))
		{
			setcardsys = 1;

			continue;
		}

		if (!strncmp(argv[x],"-setcardntp",11))
		{
			// we should have at least one more parameter

			if (x >= (argc - 1))
			{
				printf("please specify an NTP server for option -setcardntp\n");

				return(1);
			}

			strncpy(ntp_server,argv[x+1],sizeof(ntp_server)-1);

			ntp_port = 123;

			p = strchr(ntp_server,':');

			if (p)
			{
				*p = '\0';

				ntp_port = atoi(p+1);
			}
			
			setcardntp = 1;

			continue;
		}

		if (!strncmp(argv[x],"-setcardman",11))
		{
			setcardman = 1;

			continue;
		}

		if (!strncmp(argv[x],"-oneshot",8))
		{
			show_forever = 0;

			continue;
		}

		if (!strncmp(argv[x], "-h", 2))
		{
			printf("gmtool command-line options:\n");
			printf("  -setcardsys         - set time on card from current system time (UTC/GMT)\n");
			printf("  -setcardntp x       - set time on card from (S)NTP inquiry,\n");
			printf("                        where 'x' is NTP server to query\n");
			printf("  -oneshot            - Only show information once (not infinitely)\n");
//			printf("  -setcardman x       - set time on card manually,\n");
//			printf("                        where 'x' is HH:MM:SS MM/DD/YYYY\n");

			return(0);
		}
	}

	//
	// if we were requested to set time on the card using any of several methods, 
	// perform that here and exit
	//

	if (setcardsys)
	{
		clock_gettime(CLOCK_REALTIME,&systime);

		gmtime_r(&systime.tv_sec,&systime_tm);		

		snprintf(buf,sizeof(buf)-1,"F<%u,%u,%u,%u,%u,%u,%lu>",
			systime_tm.tm_mon+1,
			systime_tm.tm_mday,
			systime_tm.tm_year + 1900,
			systime_tm.tm_hour,
			systime_tm.tm_min,
			systime_tm.tm_sec,
			systime.tv_nsec / 1000000);

		write(ipc_sock,buf,strlen(buf));

		printf("Set card time from system clock: %02u:%02u:%02u.%3lu %02u/%02u/%04u\n",
			systime_tm.tm_hour,
			systime_tm.tm_min,
			systime_tm.tm_sec,
			systime.tv_nsec / 1000000,
			systime_tm.tm_mon+1,
			systime_tm.tm_mday,
			systime_tm.tm_year + 1900);

		return(0);
	}
	else
	if (setcardntp)
	{
		ntp_sock = init_NTP(ntp_server,ntp_port,&ntp_addr);

		if (ntp_sock < 0)
		{
			printf("Error %d initialize NTP server communications",ntp_sock);

			return(1);
		}
		
		res = request_NTP(ntp_sock,(struct sockaddr *)&ntp_addr,&ntp,NULL,NULL);

		if (res < 0)
		{
			printf("Error %d performing NTP request\n",res);

			return(1);
		}

		systime.tv_sec = ntp.integer - C;
		systime.tv_nsec = (double)ntp.fraction / NS_TO_32BITFS;

		gmtime_r(&systime.tv_sec,&systime_tm);		

		snprintf(buf,sizeof(buf)-1,"F<%u,%u,%u,%u,%u,%u,%lu>",
			systime_tm.tm_mon+1,
			systime_tm.tm_mday,
			systime_tm.tm_year + 1900,
			systime_tm.tm_hour,
			systime_tm.tm_min,
			systime_tm.tm_sec,
			systime.tv_nsec / 1000000);

		write(ipc_sock,buf,strlen(buf));

		printf("Set card time from NTP server '%s:%u': %02u:%02u:%02u.%3lu %02u/%02u/%04u\n",
			ntp_server,ntp_port,
			systime_tm.tm_hour,
			systime_tm.tm_min,
			systime_tm.tm_sec,
			systime.tv_nsec / 1000000,
			systime_tm.tm_mon+1,
			systime_tm.tm_mday,
			systime_tm.tm_year + 1900);

		return(0);
	}

	//
	// loop here displaying synchronization time and status information
	//

	rtc_status = gps_status = tc_status = hso_status = hso_achieve = 0;
	
	do
	{
		usleep(250000);

		//
		// get reference status
		//

		write(ipc_sock,"B",1);

		res = read(ipc_sock,buf,sizeof(buf)-1);

		if ((buf[0] == 'B') && res)
		{
			field_cnt = parse_fields(buf,&code,fields,MAX_REPLY_FIELDS);

			if (field_cnt >= 5)
			{
				// expecting code 'B' and at least 5 fields, success!

				rtc_status = atoi(fields[0]);
				gps_status = atoi(fields[1]);
				tc_status = atoi(fields[2]);
				hso_status = atoi(fields[3]);
				hso_achieve = atoi(fields[4]);
			}

			for(field_step = 0; field_step < field_cnt; field_step++) free(fields[field_step]);
		}

		//
		// get reference time
		//

		write(ipc_sock,"C",1);

		res = read(ipc_sock,buf,sizeof(buf)-1);

		if ((buf[0] == 'C') && res)
		{
			field_cnt = parse_fields(buf,&code,fields,MAX_REPLY_FIELDS);

			if (field_cnt >= 3)
			{
				// expecting code 'C' and at least 3 fields, success!

				rt_in = atoll(fields[0]);
				
				rt_interval_beg = (BILLION * atoll(fields[1])) + atoll(fields[2]);
			}

			for(field_step = 0; field_step < field_cnt; field_step++) free(fields[field_step]);
		}

		//
		// get daemon time synchronization operational status
		//

		write(ipc_sock,"D",1);

		res = read(ipc_sock,buf,sizeof(buf)-1);

		if ((buf[0] == 'D') && res)
		{	
			field_cnt = parse_fields(buf,&code,fields,MAX_REPLY_FIELDS);

			if (field_cnt >= 2)
			{
				// expecting code 'D' and at least 2 fields, success!

				sync_mode = atol(fields[0]);
				delta = atoll(fields[1]);
			}

			for(field_step = 0; field_step < field_cnt; field_step++) free(fields[field_step]);
		}

		//
		// get GPS navigation information
		//

		write(ipc_sock,"E",1);

		res = read(ipc_sock,buf,sizeof(buf)-1);

		if ((buf[0] == 'E') && res)
		{
			field_cnt = parse_fields(buf,&code,fields,MAX_REPLY_FIELDS);

			if (field_cnt >= 14)
			{
				// expecting code 'E' and at least 14 fields, success!

				strncpy(lat_string,fields[2],sizeof(lat_string)-1);
				strncpy(lat_dir,fields[3],sizeof(lat_dir)-1);
				strncpy(long_string,fields[4],sizeof(long_string)-1);
				strncpy(long_dir,fields[5],sizeof(long_dir)-1);
				fix_quality = atoi(fields[6]);
				satellites = atoi(fields[7]);
				strncpy(altitude,fields[9],sizeof(altitude)-1);
			}

			for(field_step = 0; field_step < field_cnt; field_step++) free(fields[field_step]);
		}

		//
		// compute time elapsed since top of last reference second
		//

		rt_out = BILLION * rt_in;

		clock_gettime(CLOCK_MONOTONIC_RAW,&rt_interval_end);
		
		rt_interval = ((BILLION * rt_interval_end.tv_sec) + rt_interval_end.tv_nsec) - rt_interval_beg;
		
		rt_out += rt_interval;
		
		rt_out_timet = rt_out / BILLION;

		gmtime_r(&rt_out_timet,&rt_out_tm);
		
		clock_gettime(CLOCK_REALTIME,&systime);
		
//		delta = ((BILLION * systime.tv_sec) + systime.tv_nsec) - rt_out;

		gmtime_r(&systime.tv_sec,&systime_tm);

		if (rtc_status < 6)
			printf("RTC: %-15s",lock_desc[rtc_status]);
		else
			printf("RTC: %-15s","???");

		if (gps_status < 6)
			printf("GPS: %-15s",lock_desc[gps_status]);
		else
			printf("GPS: %-15s","???");
			
		if (tc_status < 6)
			printf("TC: %-15s",lock_desc[tc_status]);
		else
			printf("TC: %-15s","???");

		if (hso_status < 6)
			printf("HSO: %-15s",lock_desc[hso_status]);
		else
			printf("HSO: %-15s","???");

		printf("HSO_A: %u\n\n",hso_achieve);

		printf("Reference time:        %02u:%02u:%02u.%03lld %02u/%02u/%04u    \n",
			rt_out_tm.tm_hour,rt_out_tm.tm_min,rt_out_tm.tm_sec,(rt_out % BILLION) / 1000000,
			rt_out_tm.tm_mon+1,rt_out_tm.tm_mday,rt_out_tm.tm_year + 1900);

		printf("System time:           %02u:%02u:%02u.%03lu %02u/%02u/%04u    \n\n",
			systime_tm.tm_hour,systime_tm.tm_min,systime_tm.tm_sec,systime.tv_nsec / 1000000,
			systime_tm.tm_mon+1,systime_tm.tm_mday,systime_tm.tm_year + 1900);

		if (sync_mode < 6)
			printf("Synchronization mode:  %s     \n",sync_desc[sync_mode]);
		else
			printf("Synchronization mode:  ???                                    \n");

		if (delta < 0)
		{
			printf("Time delta:            -%lld sec, %.03f us        \n",
				llabs(delta) / BILLION, (double)(llabs(delta) % BILLION) / 1000.0);
		}
		else
		{
			printf("Time delta:            +%lld sec, %.03f us        \n",
				delta / BILLION, (double)(delta % BILLION) / 1000.0);
		}

		if (has_GPS)
		{
			printf("Latitude %s,%s  Longitude %s,%s  Altitude %s  Fix quality=%u  Satellites=%u      \n",
				lat_string,lat_dir,long_string,long_dir,altitude,fix_quality,satellites);

			if(show_forever)
				printf("\033[8A");
		}
		else
			if(show_forever)
				printf("\033[7A");

	}
	while(show_forever);

	return(0);
}


//
//		Name:			init_NTP()
//
//		Description:	Initializes socket and server address for NTP inquiry.
//

int init_NTP(char *ntpserver, unsigned short ntpport, struct sockaddr_in *srv_addr)
{
	int					cli_sock;
	struct sockaddr_in	cli_addr;
	struct hostent		*hostInfo;

	cli_addr.sin_addr.s_addr = INADDR_ANY;
	cli_addr.sin_family = AF_INET;
	cli_addr.sin_port = 0;

	cli_sock = socket(AF_INET,SOCK_DGRAM,0);

	if (cli_sock == -1) return(-1);

	// set up the primary server socket

	if (bind(cli_sock,(struct sockaddr *)&cli_addr,sizeof(cli_addr)) == -1)
		return(-2);

	if (isipaddress(ntpserver))
	{
		srv_addr->sin_addr.s_addr = inet_addr(ntpserver);		
	}
	else
	{
		hostInfo = gethostbyname(ntpserver);

		if (!hostInfo) return(-3);

		srv_addr->sin_addr.s_addr = *((uint32_t *)&hostInfo->h_addr_list[0][0]);
	}

	srv_addr->sin_family = AF_INET;
	srv_addr->sin_port = htons(ntpport);

	return(cli_sock);
}


//
//		Name:			request_NTP()
//
//		Description:	Make an active NTP request on the specified socket.
//						
//		Note:			The NTP time returned is not adjusted for either of	the calculated round-trip delays.
//

int request_NTP(int sock, struct sockaddr *addr, NTPTIME *ntp, 
	NTPTIME *roundTrip1, uint32_t *roundTrip2)
{
	int				resp;
	NTP_PACKET		txpkt, rxpkt;
	NTPTIME			timestamp;
	socklen_t			fromLen;
	struct timespec	tmpstamp, stamp1, stamp2;
	unsigned long		RTripI, RTripF;
	unsigned long		DelayServerI,DelayServerF;
	unsigned long		DelayTransitI, DelayTransitF;

	// set NTP version and client mode in control so that server will listen

	txpkt.control = htonl((0x03 << 27) | (0x03 << 24) | (0x06 << 8) | 0x04);
	txpkt.rootdel = htonl(0);
	txpkt.rootdis = htonl(0);
	txpkt.refiden = htonl(0);
	txpkt.reftimeI = htonl(0);
	txpkt.reftimeF = htonl(0);

	clock_gettime(CLOCK_REALTIME,&tmpstamp);

	timestamp.integer = tmpstamp.tv_sec + C;
	timestamp.fraction = (double)tmpstamp.tv_nsec * NS_TO_32BITFS;

	txpkt.orgtimeI = htonl(0);
	txpkt.orgtimeF = htonl(0);
	txpkt.rcvtimeI = htonl(0);
	txpkt.rcvtimeF = htonl(0);
	txpkt.xmttimeI = htonl(timestamp.integer);
	txpkt.xmttimeF = htonl(timestamp.fraction);

	clock_gettime(CLOCK_MONOTONIC_RAW,&stamp1);			

	resp = sendto(	sock,
				(void *)&txpkt,
				sizeof(txpkt),
				0,
				addr,
				sizeof(struct sockaddr));

	if (resp == -1) return(-1);

retry_recv:

	fromLen = sizeof(struct sockaddr);

	resp = recvfrom(	sock,
					(void *)&rxpkt,
					sizeof(rxpkt),
					0,
					addr,
					&fromLen);

	clock_gettime(CLOCK_MONOTONIC_RAW,&stamp2);

	clock_gettime(CLOCK_REALTIME,&tmpstamp);

	timestamp.integer = tmpstamp.tv_sec;
	timestamp.fraction = (double)tmpstamp.tv_nsec * NS_TO_32BITFS;

	if (resp != sizeof(NTP_PACKET)) return(-2);

	// get the contents of our packet back into host-byte order

	rxpkt.control = ntohl(rxpkt.control);
	rxpkt.rootdel = ntohl(rxpkt.rootdel);
	rxpkt.rootdis = ntohl(rxpkt.rootdis);
	rxpkt.orgtimeI = ntohl(rxpkt.orgtimeI);
	rxpkt.orgtimeF = ntohl(rxpkt.orgtimeF);
	rxpkt.rcvtimeI = ntohl(rxpkt.rcvtimeI);
	rxpkt.rcvtimeF = ntohl(rxpkt.rcvtimeF);
	rxpkt.reftimeI = ntohl(rxpkt.reftimeI);
	rxpkt.reftimeF = ntohl(rxpkt.reftimeF);
	rxpkt.xmttimeI = ntohl(rxpkt.xmttimeI);
	rxpkt.xmttimeF = ntohl(rxpkt.xmttimeF);
	rxpkt.refiden = ntohl(rxpkt.refiden);

	if (((rxpkt.control >> 24) & 0x07) != 0x04)
	{
		// waiting for server unicast reply only, try again

		goto retry_recv;
	}
/*
	printf("\ncontrol:   LI=%1x VN=%1x Mode=%1x Stratum=%02x Poll=%02x Prec=%02x\n",
		rxpkt.control >> 30,
		(rxpkt.control >> 27) & 0x07,
		(rxpkt.control >> 24) & 0x07,
		(rxpkt.control >> 16) & 0xff,
		(rxpkt.control >> 8) & 0xff,
		rxpkt.control & 0xff);
	printf("rootdel:   %04x.%04x\n",
		rxpkt.rootdel >> 16,
		rxpkt.rootdel & 0xffff);
	printf("refiden:   %08x\n",
		rxpkt.refiden);
	printf("reftime:   %08x.%08x\n",
		rxpkt.reftimeI,rxpkt.reftimeF);
	printf("orgtime:   %08x.%08x\n",
		rxpkt.orgtimeI,rxpkt.orgtimeF);
	printf("recvtime:  %08x.%08x\n",
		rxpkt.rcvtimeI,rxpkt.rcvtimeF);
	printf("xmttime:   %08x.%08x\n",
		rxpkt.xmttimeI,rxpkt.xmttimeF);

*/
	// check the leap indicator flag in the control field for time validity

	if ((rxpkt.control & 0xc0000000) == 0xc0000000) return(-3);

	////////////////////////////////////////////////////////////
	// calculate round-trip delay using NTP timestamps
	////////////////////////////////////////////////////////////

	// calculate packet delay while at server

	DelayServerI = rxpkt.xmttimeI - rxpkt.rcvtimeI;
	DelayServerF = rxpkt.xmttimeF - rxpkt.rcvtimeF;

	if (rxpkt.rcvtimeF > rxpkt.xmttimeF)
	{
		// fractional result underflowed

		DelayServerI--;
	}

	// calculate packet trip time (not including processing time at server) note that 
	// the granularity of this calculation is at best the granularity of the 
	// local system clock

	DelayTransitI = timestamp.integer - rxpkt.orgtimeI;
	DelayTransitF = timestamp.fraction - rxpkt.orgtimeF;

	if (rxpkt.orgtimeF > timestamp.fraction)
	{
		// fractional result underflowed

		DelayTransitI--;
	}

	// calculate total round-trip delay

	RTripI = DelayTransitI + DelayServerI;
	RTripF = DelayTransitF + DelayServerF;

	if (RTripF < largest32(DelayTransitF,DelayServerF))
	{
		// fractional result overflowed

		RTripI++;
	}

	if (roundTrip1)
	{
		roundTrip1->integer = RTripI;
		roundTrip1->fraction = RTripF;
	}

	////////////////////////////////////////////////////////////////////
	// calculate round-trip delay using local timestamps (more precise)
	////////////////////////////////////////////////////////////////////

	if (roundTrip2)
	{
		*roundTrip2 = stamp2.tv_nsec - stamp1.tv_nsec;
	}

	// set time registers and return

	ntp->integer = rxpkt.xmttimeI;
	ntp->fraction = rxpkt.xmttimeF;

	return(0);
}


//
//	Name:			parse_conf()
//
//	Description:	Read and parse configuration file.
//

int parse_conf(char *conf)
{
   	unsigned short	gotcfg, linecnt;
   	FILE			*f;
   	char			tmp[1024];
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

       	if (!strncmp(tmp,"SocketFile",8))
	   	{
			p = strchr(tmp,'=') + 1;

           	strncpy(socket_file,p,sizeof(socket_file)-1);

           	gotcfg |= CFG_SOCKET_FILE;
       	}
       	else
       	{
		// ignore lines that we don't recognize
       	}
    }

    if (gotcfg != CFG_ALL)
    {
        printf("parse_conf() - configuration file error, an item is invalid or missing.");

        fclose(f);

        return(-3);
    }

    fclose(f);

    return(0);
}


//
//		Name:		isipaddress()
//
//		Description:	Determines if a host address string is in the (Ipv4) Internet 
//					Protocol dotted address format (x.x.x.x)
//
//		Returns:		TRUE if address is in correct format
//					FALSE otherwise, assume a lookup is required
//

int isipaddress(char *addr)
{
	char			tmp[256];
	char			*p1, *p2;
	unsigned short	val;

	memset(tmp,0,sizeof(tmp));

	strncpy(tmp,addr,sizeof(tmp)-1);
	
	// scan for anything other than numbers or '.'

	for(p1 = tmp; *p1; p1++)
	{
		if (((*p1 < '0') || (*p1 > '9')) && (*p1 != '.')) return(0);
	}

	p1 = tmp;
	p2 = strchr(p1,'.');

	if (p2)
	{
		*p2 = '\0';
		val = (unsigned short)atoi(p1);

		if (val < 256)
		{
			p1 = p2 + 1;
			p2 = strchr(p1,'.');

			if (p2)
			{
				*p2 = '\0';
				val = (unsigned short)atoi(p1);

				if (val < 256)
				{
					p1 = p2 + 1;
					p2 = strchr(p1,'.');

					if (p2)
					{
						*p2 = '\0';
						val = (unsigned short)atoi(p1);

						if (val < 256)
						{
							p1 = p2 + 1;

							val = (unsigned short)atoi(p1);

							if (val < 256) return(1);
						}
					}
				}
			}
		}
	}

	return(0);
}


uint32_t largest32(uint32_t x, uint32_t y)
{
	if (x > y) return x; else return y;
}

