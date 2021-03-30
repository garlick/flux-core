#!/bin/sh
#

test_description='Test broker overlay monitor'

# Append --logfile option if FLUX_TESTS_LOGFILE is set in environment:
test -n "$FLUX_TESTS_LOGFILE" && set -- "$@" --logfile
. `dirname $0`/sharness.sh

SIZE=4
test_under_flux $SIZE minimal

mon_rpc() {
        flux python -c "import flux; print(flux.Flux().rpc(\"overlay.monitor\",nodeid=$1).get_str())"
}

mon_rpc_streaming () {
        flux python -c "import flux; print(flux.Flux().rpc(\"overlay.monitor\",nodeid=$1,flags=flux.constants.FLUX_RPC_STREAMING).get_str())"
}

mon_rpc_streaming_cancel () {
        flux python - <<EOT
import flux
from flux.core.inner import raw
h = flux.Flux()
f = h.rpc("overlay.monitor",nodeid=$1,flags=flux.constants.FLUX_RPC_STREAMING)
matchtag = raw.flux_rpc_get_matchtag(f)
print(f.get_str())
f.reset()
h.rpc("overlay.monitor-cancel",{"matchtag":matchtag},nodeid=$1,flags=flux.constants.FLUX_RPC_NORESPONSE)
f.get()
EOT
}

request_count() {
	flux module stats overlay | jq -e '."monitor-requests"'
}

# Usage: request_count_wait_for retries value
request_count_wait_for() {
	local i
	for i in $(seq $1); do
		test $(request_count) -eq $2 && return 0
		sleep 0.5
	done
	return 1
}

test_expect_success "overlay monitor on rank 0 works" '
	mon_rpc 0 >0.out
'
test_expect_success HAVE_JQ "there are 2 children" '
	test $(jq ".children | length" 0.out) -eq 2
'
test_expect_success HAVE_JQ "children have expected ranks" '
	test $(jq ".children[0].rank" 0.out) = 1 &&
	test $(jq ".children[1].rank" 0.out) = 2
'
test_expect_success HAVE_JQ "children are connected" '
	test $(jq ".children[0].connected" 0.out) = true &&
	test $(jq ".children[1].connected" 0.out) = true
'
test_expect_success HAVE_JQ "children are not idle" '
	test $(jq ".children[0].idle" 0.out) = false &&
	test $(jq ".children[1].idle" 0.out) = false
'
test_expect_success "overlay montor fails with ENODATA on a leaf node" '
	rank=$(($SIZE-1)) &&
	test_must_fail mon_rpc $rank 2>leaf.err &&
	grep "Errno 61" leaf.err
'
test_expect_success HAVE_JQ "streaming request is cleaned up on disconnect" '
	before=$(request_count) &&
	mon_rpc_streaming 0 &&
	request_count_wait_for 5 $before
'
test_expect_success HAVE_JQ "streaming request is cleaned up on cancel" '
	before=$(request_count) &&
	! mon_rpc_streaming_cancel 0 2>cancel.err &&
	grep "Errno 61" cancel.err &&
	request_count_wait_for 5 $before
'

test_done
