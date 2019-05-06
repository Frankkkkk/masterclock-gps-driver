#ifndef _GMTOOL_H
#define _GMTOOL_H

#include <stdint.h>

#define GMTOOL_VER_MAJ		0
#define GMTOOL_VER_MIN		3
#define GMTOOL_VER_REV		0

#define MAX_REPLY_FIELDS	64

#define CFG_SOCKET_FILE		0x0001
#define CFG_ALL				0x0001

#define NTP_PORT			123				// default NTP port
#define C					2208988800L 	// number of seconds from 01/01/1900 till 01/01/1970
#define NS_TO_32BITFS		4.294967295		// conversion factor from nanoseconds to timestamp.fraction

#pragma pack(push,1)

typedef struct {
	uint32_t	control;
	uint32_t	rootdel;
	uint32_t	rootdis;
	uint32_t	refiden;
	uint32_t	reftimeI;
	uint32_t	reftimeF;
	uint32_t	orgtimeI;
	uint32_t	orgtimeF;
	uint32_t	rcvtimeI;
	uint32_t	rcvtimeF;
	uint32_t	xmttimeI;
	uint32_t	xmttimeF;

} NTP_PACKET;

#pragma pack(pop)

typedef struct {
	uint32_t integer;
	uint32_t fraction;

} NTPTIME;

int parse_conf(char *conf);
uint32_t largest32(uint32_t x, uint32_t y);
int init_NTP(char *ntpserver, unsigned short ntpport, struct sockaddr_in *srv_addr);
int request_NTP(int sock, struct sockaddr *addr, NTPTIME *ntp, 
	NTPTIME *roundTrip1, uint32_t *roundTrip2);
int isipaddress(char *addr);

#endif // _GMTOOL_H
