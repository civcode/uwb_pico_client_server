#!/bin/bash
# Manual smoke test of uwbctl against host simulators (§29 Phase 4 acceptance).
set -u
cd /home/chris/workspace/uwb_pico_client_server/uwb-system
CLI=./build/host-debug/apps/cli/uwb_cli
SIM=./build/host-debug/simulator/uwb_simulator
T="--target 127.0.0.1:13401 --target 127.0.0.1:13402 --target 127.0.0.1:13403"

pkill -x uwb_simulator 2>/dev/null
sleep 0.3
for i in 1 2 3; do
  $SIM --device-name sim$i --board-id $i,2,3,4,5,6,7,8 --udp-port 1340$i --tcp-port 1340$i \
       --period-ms 60 --range-mm 1200 --noise-mm 100 > /tmp/sim$i.log 2>&1 &
done
$SIM --device-name guarded --board-id 9,2,3,4,5,6,7,8 --udp-port 13404 --tcp-port 13404 \
     --period-ms 60 --security --security-key 0xdeadbeef > /tmp/sim4.log 2>&1 &
sleep 1

run() {
  echo "===== $* ====="
  timeout -s KILL 30 "$CLI" "$@"
  echo "exit=$?"
}

run version
run discover $T --timeout 1200
run list $T --timeout 1000
run info sim2 $T --timeout 1000
run connect sim1 sim2 sim3 $T --diagnostics
run monitor sim1 sim2 sim3 $T --duration 2000 --diagnostics
run did get sim1 name $T
run did get sim1 capabilities $T
run did get sim1 0xF00B $T
run config get sim1 $T
run config set sim1 --rate 850 --channel 9 --tag-capacity 6 $T
run config get sim1 $T
run session sim1 extended $T
run at sim1 "AT+ID" $T
run routine sim1 save-config $T
run stream range sim1 --duration 1200 --diagnostics $T
run stream range sim2 --mode recording --duration 1200 $T
# Slow consumer: a 4-slot notification queue must drop notifications but keep
# every connection healthy (§56, §60).
run monitor sim1 sim2 sim3 $T --duration 2000 --capacity 4 --diagnostics
run disconnect sim2 $T
run security guarded deadbeef --target 127.0.0.1:13404
run security guarded 00112233 --target 127.0.0.1:13404
run reset guarded soft --target 127.0.0.1:13404 --key 0xdeadbeef
run connect 127.0.0.1:13403 $T

pkill -x uwb_simulator 2>/dev/null
echo "sim3 log tail:"; tail -4 /tmp/sim3.log
exit 0
