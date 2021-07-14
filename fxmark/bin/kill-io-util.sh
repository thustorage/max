
if [ $# != 2 ] ; then
echo "usage: kill-io-util <output.file> <device name>;"
echo "e.g kill-io-util out.out sda"
exit 1;
fi

OUT_FILE=$1
DEVICE=$2
work_path=$(dirname $(readlink -f $0))

(kill -9 $(ps -ef | grep "iostat" | grep -v grep | awk '{print $2}') >> /dev/null) &\
echo $(cat $OUT_FILE.tmp | grep $DEVICE | awk -f $work_path/cat.awk)
