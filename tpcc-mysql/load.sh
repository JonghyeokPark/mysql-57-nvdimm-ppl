# Honor caller-supplied LD_LIBRARY_PATH; fallback only if empty
: "${LD_LIBRARY_PATH:=/usr/local/mysql/lib/mysql/}"
export LD_LIBRARY_PATH

DBNAME=$1
WH=$2
HOST=127.0.0.1
# STEP: stripe size. Default = max(1, WH/5) so any WH gets ~5 stripes
# (15 parallel tpcc_load + 1 items job). Override with $3.
STEP=${3:-$(( WH / 5 ))}
[ $STEP -lt 1 ] && STEP=1

./tpcc_load -h $HOST -d $DBNAME -u root -p "" -w $WH -l 1 -m 1 -n $WH >> 1.out &

x=1

while [ $x -le $WH ]
do
 end=$(( x + STEP - 1 ))
 if [ $end -gt $WH ]; then end=$WH; fi
 echo "stripe $x..$end"
./tpcc_load -h $HOST -d $DBNAME -u root -p "" -w $WH -l 2 -m $x -n $end  >> 2_$x.out &
./tpcc_load -h $HOST -d $DBNAME -u root -p "" -w $WH -l 3 -m $x -n $end  >> 3_$x.out &
./tpcc_load -h $HOST -d $DBNAME -u root -p "" -w $WH -l 4 -m $x -n $end  >> 4_$x.out &
 x=$(( $x + $STEP ))
done

# Wait for all background tpcc_load jobs to finish
wait
echo "[+] All tpcc_load jobs finished"

