#!/bin/sh
#

test_description='Test broker overlay monitor'

# Append --logfile option if FLUX_TESTS_LOGFILE is set in environment:
test -n "$FLUX_TESTS_LOGFILE" && set -- "$@" --logfile
. `dirname $0`/sharness.sh

SIZE=4
test_under_flux $SIZE minimal

# Usage: overlay_monitor_once nodeid
overlay_monitor_once() {
        flux python -c "import flux; print(flux.Flux().rpc(\"overlay.monitor\",nodeid=$1).get_str())"
}

# Usage: overlay_monitor nodeid
overlay_monitor() {
        flux python -c "import flux; print(flux.Flux().rpc(\"overlay.monitor\",nodeid=$1,flags=flux.constants.FLUX_RPC_STREAMING).get_str())"
}

# Usage: overlay_pause nodeid
overlay_pause() {
        flux python -c "import flux; flux.Flux().rpc(\"overlay.pause\",nodeid=$1).get()"
}

# Usage: get_request_count nodeid
get_request_count() {
	flux exec -r $1 flux module stats overlay | jq -e '."monitor-requests"'
}

# Usage: wait_request_count nodeid retries value
wait_request_count() {
	local try
	for try in $(seq $2); do
		test $(get_request_count $1) -eq $3 && return 0
		sleep 0.5
	done
	return 1
}

test_expect_success "overlay.monitor works on rank 0" '
	overlay_monitor_once 0 >root.out
'
test_expect_success HAVE_JQ "there are 2 children" '
	test $(jq ".children | length" root.out) -eq 2
'
test_expect_success HAVE_JQ "children have expected ranks" '
	test $(jq ".children[0].rank" root.out) = 1 &&
	test $(jq ".children[1].rank" root.out) = 2
'
test_expect_success HAVE_JQ "children are connected" '
	test $(jq ".children[0].connected" root.out) = true &&
	test $(jq ".children[1].connected" root.out) = true
'
test_expect_success HAVE_JQ "children are not idle" '
	test $(jq ".children[0].idle" root.out) = false &&
	test $(jq ".children[1].idle" root.out) = false
'
test_expect_success "overlay.monitor fails with ENODATA on a leaf node" '
	rank=$(($SIZE-1)) &&
	test_must_fail overlay_monitor_once $rank 2>leaf.err &&
	grep "Errno 61" leaf.err
'

test_expect_success HAVE_JQ "overlay.monitor is cleaned up on disconnect" '
	before=$(get_request_count 0) &&
	overlay_monitor 0 &&
	wait_request_count 0 5 ${before}
'

test_expect_success "pause rank 1" '
	overlay_pause 1
'

test_expect_success HAVE_JQ "rank 0 shows rank 1 is idle" '
	overlay_monitor_once 0 &&
	overlay_monitor_once 0 &&
	overlay_monitor_once 0 &&
	overlay_monitor_once 0 &&
	overlay_monitor_once 0 >pause.out &&
	test $(jq ".children[0].idle" pause.out) = true
'

test_expect_success "unpause rank 1" '
	overlay_pause 1
'

test_expect_success HAVE_JQ "rank 0 shows rank 1 is not idle" '
	overlay_monitor_once 0 >unpause.out &&
	test $(jq ".children[0].idle" unpause.out) = false
'


'

'

test_done
