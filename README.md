# Masterclock GPS driver (gpsmcr)
This repo contains the masterclock GPS driver with some modifications
 - It has been dockerized so you can launch it on your bare-metal k8s cluster 
   (or similar)
 - It has been patched so that the gpsmcr util no longer modifies the time 
   itself (the adjustment was not monotonic), but delegates the work to chrony

# How to run
Simply `docker-compose up -d`


# How to check everything is well
## GPS PCI-e card
```
$ docker exec -ti gpsmcr ./gmtool
GMTOOL for PCIe-GPS/PCIe-TCR/PCIe-OCS (version 0.3.0)

Card firmware: 1.0.3

Daemon version: 0.4.0  Start time (UTC): 11:06:28 09/30/2019

RTC: Locked         GPS: Locked         TC: Unlocked       HSO: Unknown        HSO_A: 0

Reference time:        09:44:28.883 03/03/2020
System time:           09:44:28.883 03/03/2020

Synchronization mode:  Waiting for Lock
Time delta:            +0 sec, 49.095 us
Latitude 46XXXXXXXX,N  Longitude 006XXXXXXXX,E  Altitude 00521  Fix quality=1  
Satellites=8
```

## Chrony and GPS PPS usage
```
$ docker exec -ti chrony chronyc sources -v
210 Number of sources = 6

  .-- Source mode  '^' = server, '=' = peer, '#' = local clock.
 / .- Source state '*' = current synced, '+' = combined , '-' = not combined,
| /   '?' = unreachable, 'x' = time may be in error, '~' = time too variable.
||                                                 .- xxxx [ yyyy ] +/- zzzz
||      Reachability register (octal) -.           |  xxxx = adjusted offset,
||      Log2(Polling interval) --.      |          |  yyyy = measured offset,
||                                \     |          |  zzzz = estimated error.
||                                 |    |           \
MS Name/IP address         Stratum Poll Reach LastRx Last sample
===============================================================================
#* GPS                           0   4   377    19    +13us[  +14us] +/-   13us
^? aton.infomaniak.ch            3  10   377  130d  -5152ms[ -251us] +/-   67ms
^- falseticker.ntp.infomani>     2   9   377   87m   -587us[ +106us] +/-   32ms
^- tick.ntp.infomaniak.ch        1   8    17   353  +3401ns[ +790ns] +/-  394us
^- tick.ntp.infomaniak.ch        1   6   377   125  -2198ns[-2343ns] +/-   60us
^- metasntp13.admin.ch           1  10   377   225   -242us[ -246us] +/- 4572us
```
If you see the `#* GPS` line, then everything is well
