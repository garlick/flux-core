#!/bin/sh

test_description='Test flux job execution service in simulated mode'

. $(dirname $0)/sharness.sh

test_under_flux 1 job

flux setattr log-stderr-level 1

skip_all_unless_have jq

RPC=${FLUX_BUILD_DIR}/t/request/rpc

job_kvsdir()    { flux job id --to=kvs $1; }
exec_eventlog() { flux kvs get -r $(job_kvsdir $1).guest.exec.eventlog; }

test_expect_success 'job-exec: generate jobspec for simple test job' '
	flux mini run \
	    --setattr=system.exec.test.run_duration=0.0001s \
	    --dry-run hostname > basic.json
'
test_expect_success 'job-exec: basic job runs in simulated mode' '
	jobid=$(flux job submit basic.json) &&
	flux job wait-event -t 1 ${jobid} start &&
	flux job wait-event -t 1 ${jobid} finish &&
	flux job wait-event -t 1 ${jobid} release &&
	flux job wait-event -t 1 ${jobid} clean
'
test_expect_success 'job-exec: guestns linked into primary' '
	#  guest key is not a link
	test_must_fail \
	  flux kvs readlink $(job_kvsdir ${jobid}).guest 2>readlink.err &&
	grep "Invalid argument" readlink.err &&
	#  gues key is a directory
	test_must_fail \
	  flux kvs get $(job_kvsdir ${jobid}).guest 2>kvsdir.err &&
	grep "Is a directory" kvsdir.err
'
test_expect_success 'job-exec: exec.eventlog exists with expected states' '
	exec_eventlog ${jobid} > eventlog.1.out &&
	head -1 eventlog.1.out | grep "init" &&
	tail -1 eventlog.1.out | grep "done"
'
test_expect_success 'job-exec: canceling job during execution works' '
	jobid=$(flux mini submit \
                --setattr=system.exec.test.run_duration=10s hostname) &&
	flux job wait-event -vt 2.5 ${jobid} start &&
	flux job cancel ${jobid} &&
	flux job wait-event -t 2.5 ${jobid} exception &&
	flux job wait-event -t 2.5 ${jobid} finish | grep status=15 &&
	flux job wait-event -t 2.5 ${jobid} release &&
	flux job wait-event -t 2.5 ${jobid} clean &&
	exec_eventlog $jobid | grep "complete" | grep "\"status\":15"
'
test_expect_success 'job-exec: mock exception during initialization' '
	jobid=$(flux mini submit \
	         --setattr=system.exec.test.mock_exception=init true) &&
	flux job wait-event -t 2.5 ${jobid} exception > exception.1.out &&
	test_debug "flux job eventlog ${jobid}" &&
	grep "type=\"exec\"" exception.1.out &&
	grep "mock initialization exception generated" exception.1.out &&
	flux job wait-event -qt 2.5 ${jobid} clean &&
	flux job eventlog ${jobid} > eventlog.${jobid}.out &&
	test_must_fail grep "finish" eventlog.${jobid}.out
'
test_expect_success 'job-exec: mock exception during run' '
	jobid=$(flux mini submit \
	         --setattr=system.exec.test.mock_exception=run true) &&
	flux job wait-event -t 2.5 ${jobid} exception > exception.2.out &&
	grep "type=\"exec\"" exception.2.out &&
	grep "mock run exception generated" exception.2.out &&
	flux job wait-event -qt 2.5 ${jobid} clean &&
	flux job eventlog ${jobid} > eventlog.${jobid}.out &&
	grep "finish status=15" eventlog.${jobid}.out
'
test_expect_success 'job-exec: R with invalid expiration raises exception' '
	flux module unload job-exec &&
	jobid=$(flux job submit basic.json) &&
	key=$(flux job id --to=kvs $jobid).R &&
	R=$(flux kvs get --wait $key | jq -c ".execution.expiration = -1.") &&
	flux kvs put ${key}=${R} &&
	flux module load job-exec &&
	flux job wait-event -v $jobid exception
'
test_expect_success 'start request with empty payload fails with EPROTO(71)' '
	${RPC} job-exec.start 71 </dev/null
'
test_expect_success 'job-exec: invalid testexec conf generates exception' '
	jobid=$(flux mini submit \
	    --setattr=system.exec.test.run_duration=0.01 hostname) &&
	flux job wait-event -t 5 ${jobid} exception > except.invalid.out &&
	grep "type=\"exec\"" except.invalid.out &&
	flux job wait-event -qt 5 ${jobid} clean &&
	flux job eventlog ${jobid}
'
test_expect_success 'job-exec: test exec start override works' '
	jobid=$(flux mini submit \
	    --setattr=system.exec.test.override=1 \
	    --setattr=system.exec.test.run_duration=0.001s \
	    true) &&
	flux job wait-event -t 5 ${jobid} alloc &&
	test_must_fail flux job wait-event -t 0.1 ${jobid} start &&
	flux job-exec-override start ${jobid} &&
	flux job wait-event -t 5 ${jobid} start &&
	flux job wait-event -t 5 -v ${jobid} clean
'
test_expect_success 'job-exec: override only works on jobs with flag set' '
	jobid=$(flux mini submit \
		--setattr=system.exec.test.run_duration=0. /bin/true) &&
	flux job wait-event -t 5 ${jobid} alloc &&
	test_must_fail flux job-exec-override start ${jobid} &&
	flux job cancel ${jobid} &&
	flux job wait-event -t 5 -v ${jobid} clean
'
test_expect_success 'job-exec: test exec start/finish override works' '
	jobid=$(flux mini submit \
	    --setattr=system.exec.test.override=1 \
	    true) &&
	flux job wait-event -t 5 ${jobid} alloc &&
	test_must_fail flux job wait-event -t 0.1 ${jobid} start &&
	test_must_fail flux job-exec-override finish ${jobid} 0 &&
	flux job-exec-override start ${jobid} &&
	flux job wait-event -t 5 ${jobid} start &&
	test_must_fail flux job-exec-override start ${jobid} &&
	test_must_fail flux job wait-event -t 0.1 ${jobid} finish &&
	flux job-exec-override finish ${jobid} 0 &&
	flux job wait-event -t 5 -v ${jobid} clean
'
test_expect_success 'job-exec: flux job-exec-override fails on invalid id' '
	test_must_fail flux job-exec-override start 1234  &&
	test_must_fail flux job-exec-override finish 1234  0
'
test_expect_success 'job-exec: job-exec.testoverride invalid request' '
	cat <<-EOF >override.py &&
	import json
	import sys
	import flux
	h = flux.Flux()
	try:
	    print(h.rpc("job-exec.override", json.load(sys.stdin)).get())
	except OSError as exc:
	    print(str(exc), file=sys.stderr)
	    sys.exit(1)
	EOF
	echo {} | \
	  test_must_fail flux python override.py &&
	jobid=$(flux mini submit \
	    --setattr=system.exec.test.override=1 \
	    true) &&
	cat <<-EOF >badevent.json &&
	{"jobid":"$(flux job id --to=dec ${jobid})", "event":"foo"}
	EOF
	test_must_fail flux python override.py < badevent.json &&
	flux job cancel $jobid &&
	flux job wait-event $jobid clean
'
test_expect_success 'job-exec: flux job-exec-override fails for invalid userid' '
	jobid=$(flux mini submit \
	    --setattr=system.exec.test.override=1 \
	    true) &&
	newid=$(($(id -u)+1)) &&
	( export FLUX_HANDLE_ROLEMASK=0x2 &&
	  export FLUX_HANDLE_USERID=$newid &&
	    test_must_fail flux job-exec-override start ${jobid}
	) &&
	flux job cancel ${jobid} &&
	flux job wait-event -t 5 -v ${jobid} clean
'
test_done
