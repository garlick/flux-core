#!/bin/sh

test_description='test scheduler pool-class loading and error messages'

. $(dirname $0)/sharness.sh

test_under_flux 1 job

test_expect_success 'unload sched-simple' '
	flux module remove sched-simple
'

test_expect_success 'increase broker log level to capture debug messages' '
	flux setattr log-stderr-level 7
'

test_expect_success 'loading scheduler with invalid pool-class fails' '
	test_must_fail flux module load sched-simple \
		log-level=debug pool-class=nonexistent_module
'

test_expect_success 'broker log contains concise error at LOG_ERR' '
	flux dmesg | grep sched-simple.err >err.log &&
	grep "Failed to load pool class.*nonexistent_module.*No module named" err.log
'

test_expect_success 'broker log contains full traceback at LOG_DEBUG' '
	flux dmesg | grep sched-simple.debug >debug.log &&
	grep "Pool class load traceback" debug.log &&
	grep "Traceback" debug.log &&
	grep "ModuleNotFoundError" debug.log
'

test_expect_success 'clear dmesg for next test' '
	flux dmesg -C
'

test_expect_success 'loading scheduler with module missing pool_class fails' '
	test_must_fail flux module load sched-simple \
		log-level=debug pool-class=json
'

test_expect_success 'broker log contains concise error at LOG_ERR for AttributeError' '
	flux dmesg | grep sched-simple.err >err2.log &&
	grep "Failed to load pool class.*json.*has no attribute.*pool_class" err2.log
'

test_expect_success 'broker log contains full traceback at LOG_DEBUG for AttributeError' '
	flux dmesg | grep sched-simple.debug >debug2.log &&
	grep "Pool class load traceback" debug2.log &&
	grep "Traceback" debug2.log &&
	grep "AttributeError" debug2.log
'

test_expect_success 'load scheduler without pool-class argument succeeds' '
	flux module load sched-simple
'

test_expect_success 'restore broker log level' '
	flux setattr log-stderr-level 5
'

test_done
