#!/bin/sh
# Starts the rerinku camera program and restarts it if it exits.
#
# Called from /oem/usr/bin/RkLunch.sh in place of rkipc, after the camera
# drivers are loaded (scripts/deploy.sh makes that change).
#
#   rerinku-launch.sh          start (in the background)
#   rerinku-launch.sh stop     stop the program and the restart loop
#
# Logs go to syslog (logread / /var/log/messages), tag "rerinku".

BIN=/oem/usr/bin/rerinku-luckfox
CONF=/userdata/rerinku/rerinku.conf
PIDFILE=/var/run/rerinku-launch.pid

stop() {
	[ -f "$PIDFILE" ] || return 0
	pid=$(cat "$PIDFILE")
	rm -f "$PIDFILE"
	# TERM to the loop makes it pass TERM on and exit.
	kill "$pid" 2>/dev/null
	i=0
	while kill -0 "$pid" 2>/dev/null && [ $i -lt 50 ]; do
		sleep 0.1
		i=$((i + 1))
	done
}

loop() {
	child=
	trap '[ -n "$child" ] && kill "$child" 2>/dev/null; wait; exit 0' TERM INT
	while :; do
		"$BIN" -c "$CONF" --syslog &
		child=$!
		wait "$child"
		status=$?
		child=
		logger -t rerinku "rerinku-luckfox exited ($status), restarting in 3 s"
		sleep 3
	done
}

case "$1" in
stop)
	stop
	;;
run)
	# Internal: the restart loop itself, started below.
	loop
	;;
*)
	stop
	# The loop runs as a separate shell rather than as `loop ... &`: for a
	# redirected function busybox ash keeps copies of the caller's stdout and
	# stderr open, and started from an SSH session those hold the session open
	# until the loop ends.
	/bin/sh "$0" run </dev/null >/dev/null 2>&1 &
	echo $! >"$PIDFILE"
	;;
esac
