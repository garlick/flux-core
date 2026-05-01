#!/bin/sh

test_description='sched-simple pool-class: NUMA-affinity GPU+core allocation using AffinityPool'

# Append --logfile option if FLUX_TESTS_LOGFILE is set in environment:
test -n "$FLUX_TESTS_LOGFILE" && set -- "$@" --logfile
. $(dirname $0)/sharness.sh

# 16-node instance modelling a Dell PowerEdge XE9680 (2-socket Xeon 8580,
# 8 GPUs/node) with SNC4 enabled: 4 NUMA nodes per socket, currently
# scheduled at socket granularity.
#   socket 0 (cores 0-59, gpus 0-3):
#     numa 0: cores  0-14,  gpu 0  |  numa 1: cores 15-29, gpu 1
#     numa 2: cores 30-44,  gpu 2  |  numa 3: cores 45-59, gpu 3
#   socket 1 (cores 60-119, gpus 4-7):
#     numa 0: cores  60-74, gpu 4  |  numa 1: cores  75-89, gpu 5
#     numa 2: cores 90-104, gpu 6  |  numa 3: cores 105-119, gpu 7
test_under_flux 16 job

# Build R with NUMA topology injected into scheduling key.
# All 16 nodes share the same layout, expressed as a single IDset range key.
build_affinity_R() {
	flux R encode -r0-15 -c0-119 -g0-7 | \
	    jq '.scheduling = {
	        "writer": "AffinityPool",
	        "topology": {
	            "0-15": {"socket": [
	                {"numa": [
	                    {"cores": "0-14",   "gpus": "0"},
	                    {"cores": "15-29",  "gpus": "1"},
	                    {"cores": "30-44",  "gpus": "2"},
	                    {"cores": "45-59",  "gpus": "3"}
	                ]},
	                {"numa": [
	                    {"cores": "60-74",  "gpus": "4"},
	                    {"cores": "75-89",  "gpus": "5"},
	                    {"cores": "90-104", "gpus": "6"},
	                    {"cores": "105-119","gpus": "7"}
	                ]}
	            ]}
	        }
	    }'
}

test_expect_success 'load sched-simple with NUMA affinity topology R' '
	flux module unload sched-simple &&
	build_affinity_R >R.affinity &&
	flux resource reload R.affinity &&
	flux module load sched-simple
'

test_expect_success 'AffinityPool raises ValueError when scheduling key is absent' '
	cat >test_no_sched.py <<-EOF &&
	from flux.resource.AffinityPool import AffinityPool
	import json, subprocess
	R = json.loads(subprocess.check_output(
	    ["flux", "R", "encode", "-r0-15", "-c0-127", "-g0-7"]))
	AffinityPool(R, log=lambda lvl, msg: None)
	EOF
	test_must_fail flux python test_no_sched.py
'

test_expect_success 'AffinityPool raises ValueError when topology key is absent' '
	cat >test_no_topo.py <<-EOF &&
	from flux.resource.AffinityPool import AffinityPool
	import json, subprocess
	R = json.loads(subprocess.check_output(
	    ["flux", "R", "encode", "-r0-15", "-c0-127", "-g0-7"]))
	R["scheduling"] = {"writer": "AffinityPool"}
	AffinityPool(R, log=lambda lvl, msg: None)
	EOF
	test_must_fail flux python test_no_topo.py
'

# Sub-instance test: batch job receives trimmed topology with both nodes;
# inner GPU job receives topology trimmed to its single allocated node.
test_expect_success 'sub-instance on GPU allocation inherits trimmed topology' '
	cat >batch-subinst.sh <<-EOF &&
	#!/bin/sh
	jobid=\$(flux submit -N1 -n1 -c1 -g1 hostname) &&
	flux job wait-event --timeout=10 \${jobid} alloc &&
	flux job info \${jobid} R | jq -e ".scheduling.topology | keys | length == 1" &&
	flux job wait-event --timeout=10 \${jobid} clean &&
	flux run -N1 -n1 -c1 hostname
	EOF
	chmod +x batch-subinst.sh &&
	batchjob=$(flux batch -N2 --exclusive batch-subinst.sh) &&
	flux job wait-event --timeout=30 ${batchjob} alloc &&
	flux job info ${batchjob} R | jq -e ".scheduling.writer == \"AffinityPool\"" &&
	flux job info ${batchjob} R | jq -e ".scheduling.topology | keys | length == 2" &&
	flux job attach ${batchjob}
'



test_expect_success 'GPU+core job allocates successfully' '
	jobid=$(flux submit -N1 -n1 -c1 -g1 sleep inf) &&
	flux job wait-event --timeout=10 ${jobid} alloc
'

test_expect_success 'allocated R carries scheduling.topology with writer key' '
	flux job info ${jobid} R | jq -e ".scheduling.topology" &&
	flux job info ${jobid} R | jq -e ".scheduling.writer == \"AffinityPool\""
'

test_expect_success 'allocated topology trimmed to the one assigned node' '
	flux job info ${jobid} R | jq -e ".scheduling.topology | keys | length == 1"
'

# In the test topology gpu G maps to cores [G*15, G*15+14] (15 cores per NUMA node).
test_expect_success 'allocated core is NUMA-local to its GPU' '
	flux job info ${jobid} R | jq -e ".execution.R_lite[0] | (.children.gpu|tonumber) as \$g | (.children.core|tonumber) as \$c | \$c >= \$g*15 and \$c <= \$g*15+14"
'

test_expect_success 'cleanup GPU+core job' '
	flux cancel ${jobid} &&
	flux job wait-event --timeout=10 ${jobid} clean
'

test_expect_success 'CPU-only job allocates without NUMA constraint' '
	jobid=$(flux submit -N1 -n1 -c120 sleep inf) &&
	flux job wait-event --timeout=10 ${jobid} alloc &&
	flux cancel ${jobid} &&
	flux job wait-event --timeout=10 ${jobid} clean
'

test_expect_success 'GPU+cross-socket slot succeeds via best-effort fallback' '
	jobid=$(flux submit -N1 -n1 -c61 -g1 sleep inf) &&
	flux job wait-event --timeout=10 ${jobid} alloc &&
	flux cancel ${jobid} &&
	flux job wait-event --timeout=10 ${jobid} clean
'

test_expect_success 'GPU slot exceeding node GPU count is permanently denied' '
	jobid=$(flux submit -N1 -n1 -c1 -g9 sleep inf) &&
	flux job wait-event --timeout=10 ${jobid} exception
'

test_expect_success 'single-GPU+core slots allocate on two separate nodes' '
	job0=$(flux submit -N1 -n1 -c1 -g1 sleep inf) &&
	job1=$(flux submit -N1 -n1 -c1 -g1 sleep inf) &&
	flux job wait-event --timeout=10 ${job0} alloc &&
	flux job wait-event --timeout=10 ${job1} alloc &&
	flux cancel ${job0} ${job1} &&
	flux job wait-event --timeout=10 ${job1} clean
'

# Fill all GPUs across all 16 nodes (16 nodes × 8 GPUs = 128 slots), then
# verify that a CPU-only job still allocates despite no free GPUs anywhere.
test_expect_success 'CPU-only job runs when all GPUs in pool are exhausted' '
	gpujob=$(flux submit -N16 -n128 -c1 -g1 sleep inf) &&
	flux job wait-event --timeout=30 ${gpujob} alloc &&
	cpujob=$(flux submit -N1 -n1 -c1 sleep inf) &&
	flux job wait-event --timeout=10 ${cpujob} alloc &&
	flux cancel ${gpujob} ${cpujob} &&
	flux job wait-event --timeout=30 ${gpujob} clean
'

test_done
