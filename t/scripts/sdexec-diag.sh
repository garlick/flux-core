#!/bin/sh
# DEBUG ONLY - drop before merge.
#
# Dump systemd and sdexec state into the sharness .output log when a test
# fails, and from a watchdog shortly before run_timeout.py SIGKILLs a hung
# test script (which would otherwise leave no clue where it was stuck).
# Source this after test_under_flux.

: ${SDEXEC_DIAG_WATCHDOG:=240}

sdexec_diag_dir=$(pwd)
sdexec_diag_pid=$$

# Debug-level logging from the shared user manager, so that if it starts
# busy-looping the journal shows which event it keeps dispatching.
# (systemd 239 lacks "systemctl log-level"; systemd-analyze has it.)
timeout 5 systemd-analyze --user set-log-level debug >/dev/null 2>&1 \
    || timeout 5 systemctl --user log-level debug >/dev/null 2>&1

sdexec_diag() {
	(
	exec >&4 2>&4
	echo "=== sdexec-diag: $* ($(date +%T))"
	echo "--- systemctl --user is-system-running"
	timeout 10 systemctl --user is-system-running; echo "(rc=$?)"
	echo "--- systemctl --user list-jobs"
	timeout 10 systemctl --user list-jobs --no-pager; echo "(rc=$?)"
	echo "--- systemctl --user list-units --all (non-flux noise filtered)"
	timeout 10 systemctl --user list-units --all --type=service \
	    --no-pager --plain --no-legend \
	    | grep -v -e "dbus\.service" -e "^systemd-"
	echo "--- unit properties of non-dead services"
	for u in $(timeout 10 systemctl --user list-units --all \
	    --type=service --no-pager --plain --no-legend \
	    | awk '$4 != "dead" {print $1}'); do
		timeout 5 systemctl --user show -p Id,Type,LoadState \
		    -p ActiveState,SubState,Result,MainPID,ControlPID \
		    -p ExecMainCode,ExecMainStatus,StateChangeTimestamp "$u" \
		    | tr '\n' ' '
		echo
	done
	for pid in $(pgrep -u $(id -u) -x systemd); do
		echo "--- systemd --user pid $pid"
		grep -E "^(State|Threads|voluntary|nonvoluntary)" \
		    /proc/$pid/status
		echo "wchan: $(cat /proc/$pid/wchan 2>&1)"
		echo "syscall: $(cat /proc/$pid/syscall 2>&1)"
		sudo -n cat /proc/$pid/stack 2>&1
		echo "open fds: $(ls /proc/$pid/fd 2>/dev/null | wc -l)"
		echo "--- fds of systemd --user (what it may be blocked on)"
		sudo -n ls -l /proc/$pid/fd 2>&1 | sed 's/.* \([0-9]* -> \)/\1/'
		sudo -n cat /proc/$pid/net/unix 2>/dev/null | wc -l \
		    | sed 's/^/unix sockets in netns: /'
		# Only one test's dump may ptrace the shared manager: concurrent
		# watchdogs fire together, and a second attach fails with EPERM
		# while the first holds the process in tracing stop.
		if sudo -n true 2>/dev/null \
		    && mkdir /tmp/sdexec-diag-ptrace.lock 2>/dev/null; then
			for tool in gdb strace; do
				command -v $tool >/dev/null && continue
				timeout 180 sudo -n dnf -q -y install $tool \
				    >/dev/null 2>&1
			done
			echo "--- strace of systemd --user for 2s (first 300 lines)"
			timeout 2 sudo -n strace -p $pid -tt -y 2>&1 | head -n 300
			echo "--- strace -c summary of systemd --user over 2s"
			timeout -s INT 2 sudo -n strace -c -p $pid 2>&1
			echo "--- userspace stack of systemd --user"
			if command -v gdb >/dev/null; then
				timeout 60 sudo -n gdb -p $pid -batch -nx \
				    -ex "bt" 2>&1 \
				    | grep -v -e "^\[New LWP" -e "^warning:"
			else
				echo "(gdb not available)"
			fi
		else
			echo "--- (ptrace skipped: another test's dump holds the lock)"
		fi
	done
	for pid in $(pgrep -u $(id -u) -x systemd); do
		echo "--- systemd --user pid $pid"
		grep -E "^(State|Threads|voluntary|nonvoluntary)" \
		    /proc/$pid/status
		echo "wchan: $(cat /proc/$pid/wchan 2>&1)"
		echo "syscall: $(cat /proc/$pid/syscall 2>&1)"
		sudo -n cat /proc/$pid/stack 2>&1
		echo "open fds: $(ls /proc/$pid/fd 2>/dev/null | wc -l)"
		echo "--- fds of systemd --user (what it may be blocked on)"
		sudo -n ls -l /proc/$pid/fd 2>&1 | sed 's/.* \([0-9]* -> \)/\1/'
		sudo -n cat /proc/$pid/net/unix 2>/dev/null | wc -l \
		    | sed 's/^/unix sockets in netns: /'
		if sudo -n true 2>/dev/null; then
			if ! command -v gdb >/dev/null; then
				timeout 120 sudo -n dnf -q -y install gdb \
				    >/dev/null 2>&1 \
				    || timeout 120 sudo -n yum -q -y install gdb \
				    >/dev/null 2>&1
			fi
			echo "--- userspace stack of systemd --user"
			if command -v gdb >/dev/null; then
				timeout 60 sudo -n gdb -p $pid -batch -nx \
				    -ex "bt" -ex "info registers rip" 2>&1 \
				    | grep -v -e "^\[New LWP" -e "^warning:"
			elif command -v eu-stack >/dev/null; then
				timeout 60 sudo -n eu-stack -p $pid 2>&1
			else
				echo "(no gdb or eu-stack available)"
			fi
		fi
	done
	for pid in $(pgrep -u $(id -u) -x systemd); do
		echo "--- epoll/timerfd fdinfo of systemd --user pid $pid"
		for fd in $(sudo -n ls /proc/$pid/fd 2>/dev/null); do
			l=$(sudo -n readlink /proc/$pid/fd/$fd 2>/dev/null)
			case "$l" in
			*eventpoll*|*timerfd*|*inotify*|*signalfd*)
				echo "## fd $fd $l"
				sudo -n cat /proc/$pid/fdinfo/$fd 2>&1
				;;
			esac
		done
		jl=$TMPDIR/sdexec-diag-journal.$$
		timeout 30 sudo -n journalctl --no-pager -o short-precise \
		    _PID=$pid >$jl 2>&1
		first=$(grep -n -m1 "Looping too fast" $jl | cut -d: -f1)
		echo "--- user manager journal: $(wc -l <$jl) lines," \
		    "$(grep -c 'Looping too fast' $jl) 'Looping too fast'," \
		    "first at line ${first:-none}"
		if test -n "$first"; then
			echo "--- user manager journal, 200 lines before first loop"
			head -n $first $jl | tail -n 200
		fi
		echo "--- user manager journal, last 200 non-loop lines"
		grep -v "Looping too fast" $jl | tail -n 200
		rm -f $jl
	done
	echo "--- system journal, tail (sudo noise filtered)"
	timeout 20 sudo -n journalctl --no-pager -n 200 2>&1 \
	    | grep -v -e "sudo\[" -e "Session c[0-9]* of user root" \
	    -e "session-c[0-9]*\.scope" | tail -n 40
	echo "--- journald"
	timeout 10 systemctl is-active systemd-journald.service 2>&1
	df -h /run /run/log/journal 2>&1
	echo "--- user@$(id -u).service output settings"
	timeout 10 systemctl show user@$(id -u).service \
	    -p StandardOutput -p StandardError -p LogTarget 2>&1
	echo "--- processes"
	ps -u $(id -u) -o pid,ppid,stat,etime,args | cut -c1-160
	for d in "$sdexec_diag_dir"/records "$sdexec_diag_dir"/*/records; do
		test -d "$d" || continue
		echo "--- records in $d"
		for f in "$d"/* "$d"/.[!.]*; do
			test -e "$f" || continue
			echo "## $f"
			cat "$f"
		done
	done
	echo "--- flux dmesg"
	timeout 10 flux dmesg -H
	echo "=== sdexec-diag end"
	) </dev/null
}

# Re-define sharness' failure hook (copied from t/sharness.sh) to dump
# diagnostics before reporting the failure.
test_failure_() {
	sdexec_diag "test $test_count failed: $1"
	test_failure=$(($test_failure + 1))
	say_color error "not ok $test_count - $1"
	test -n "$logfile" && say >&3 "not ok $test_count - $1"
	shift
	echo "$@" | sed -e 's/^/#	/'
	test "$immediate" = "" || { EXIT_OK=t; exit 1; }
}

# Watchdog: if the test script is still running after SDEXEC_DIAG_WATCHDOG
# seconds, dump once.  Exits early when the test script does.
(
	i=0
	while test $i -lt $SDEXEC_DIAG_WATCHDOG; do
		kill -0 $sdexec_diag_pid 2>/dev/null || exit 0
		sleep 1
		i=$(($i + 1))
	done
	sdexec_diag "watchdog: still running after ${SDEXEC_DIAG_WATCHDOG}s"
) </dev/null >/dev/null 2>&1 &
