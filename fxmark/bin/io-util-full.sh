
if [ $# != 2 ] ; then
echo "usage: io-util <output.file> <device name>;"
echo "e.g io-util out.out sda"
exit 1;
fi

trap 'my_exit; exit' 2 9

OUT_FILE=$1
DEVICE=$2
work_path=$(dirname $(readlink -f $0))

my_exit()
{
echo "you hit Ctrl-C/Ctrl-\, now exiting.."
echo "io_util" > $OUT_FILE
cat $OUT_FILE.tmp | grep $DEVICE | awk -f $work_path/cat.awk >> $OUT_FILE
rm -f $OUT_FILE.tmp
}

while :
do
iostat -d -x $2 2 >> $1.tmp
done
