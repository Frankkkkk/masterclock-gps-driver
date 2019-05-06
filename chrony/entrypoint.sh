#!/bin/bash
cfile=/etc/chrony/chrony.conf

#Remove chrony pid if exists
rm /run/chronyd.pid || true

#Remove socket if it exists
rm $(cat $cfile | grep 'refclock SOCK' | awk '{print $3 }') || true

chronyd -d


