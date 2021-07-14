#!/bin/bash
#
# There must be a symlink from D -> $TMPFS/exim
# the make script has implicit deps. on this script
#

MAXCPU=48
CPUS=1
N=1

D=/export/home/sbw/exim
DR=/export/home/sbw/exim.real
Q=$D/spool/input
CO=/export/home/sbw/exim.rates
SMPTP=./sbw-smtpbm

TMPFS=/tmp

if [ `whoami` != root ]
then
  echo eximbm.sh must be run as root.
  exit 1
fi

if mount | grep dev.shm | grep nosuid
then
  echo /dev/shm must be mounted without nosuid.
  exit 1
fi

pkill -x exim-4.7 
pkill -x exim
pkill -x sbw-smtpbm

sync
sleep 1

if [ ! -d $TMPFS/exim ]
then
  mkdir $TMPFS/exim
fi

rm -rf $D/*
rm $D
ln -s $TMPFS/exim $D
(cd $DR && tar cf - . | (cd $D && tar xpf -))
#cp -r $DR/* $D
sync

# enable/disable CPUs
I=0
while [ $I -lt $MAXCPU ]; do
    if [ $I -lt $CPUS ]; then
	psradm -F -n $I
    else
	psradm -F -f $I
    fi
    I=`expr $I + 1`
done

$D/bin/exim -bd -oX 2525 &
sleep 1

echo Running...
I=0
while [ $I -lt $N ]
do
  $SMPTP 127.0.0.1 2525 user$I@optimus.gtisc.gatech.edu > $CO/$I &
  I=`expr $I + 1`
  sleep 0.05
done

I=0
while [ $I -lt $N ]
do
  wait
  I=`expr $I + 1`
done

I=0
T=0
while [ $I -lt $N ]
do
  R=`sed 's/\..*//' $CO/$I`
  T=`expr $T + $R`
  I=`expr $I + 1`
done

expr $T

# enable all CPUs
I=0
while [ $I -lt $MAXCPU ]; do
    psradm -F -n $I    
    I=`expr $I + 1`
done

if egrep '^split.*true' $D/etc/configure > /dev/null
then
  echo WARNING split_spool_directory = true
fi

if [ -s $D/spool/log/paniclog -o -s $D/spool/log/rejectlog ]
then
  echo WARNING maybe errors in log files
fi 

if [ ! -s $D/spool/log/mainlog ]
then
  echo WARNING mainlog is empty
fi 
