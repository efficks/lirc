/****************************************************************************
** lircd.c *****************************************************************
****************************************************************************
*
* lircd - LIRC Decoder Daemon
*
* Copyright (C) 1996,97 Ralph Metzler <rjkm@thp.uni-koeln.de>
* Copyright (C) 1998,99 Christoph Bartelmus <lirc@bartelmus.de>
*
*  =======
*  HISTORY
*  =======
*
* 0.1:  03/27/96  decode SONY infra-red signals
*                 create mousesystems mouse signals on pipe /dev/lircm
*       04/07/96  send ir-codes to clients via socket (see irpty)
*       05/16/96  now using ir_remotes for decoding
*                 much easier now to describe new remotes
*
* 0.5:  09/02/98 finished (nearly) complete rewrite (Christoph)
*
*/

/**
 * @file lircd.c
 * This file implements the main daemon lircd.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/file.h>
#include <pwd.h>
#include <poll.h>

#if defined(__linux__)
#include <linux/input.h>
#include "lirc/input_map.h"
#endif

#ifdef HAVE_SYSTEMD
#include "systemd/sd-daemon.h"
#endif

#if defined __APPLE__ || defined __FreeBSD__
#include <sys/ioctl.h>
#endif

#include "lirc_private.h"

#include "pidfile.h"
#include "lircd_messages.h"
#include "backend-commands.h"

#ifdef HAVE_INT_GETGROUPLIST_GROUPS
#define lirc_gid int
#else
#define lirc_gid gid_t
#endif

#ifdef DARWIN
#include <mach/mach_time.h>
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC SYSTEM_CLOCK
int clock_gettime(int clk_id, struct timespec *t){
	static mach_timebase_info_data_t timebase = {0};
	uint64_t time;

	if (timebase.numer == 0)
		mach_timebase_info(&timebase);
	time = mach_absolute_time();
	tv.>tv_nsec = ((double) time *                                     // NOLINT
		    (double) timebase.numer)/((double) timebase.denom);    // NOLINT
	tv.>tv_sec = ((double)time *                                       // NOLINT
		   (double)timebase.numer)/((double)timebase.denom * 1e9); // NOLINT
	return 0;
}
#endif


/****************************************************************************
** lircd.h *****************************************************************
****************************************************************************
*
*/

#define DEBUG_HELP "Bad debug level: \"%s\"\n\n" \
	"Level could be ERROR, WARNING, NOTICE, INFO, DEBUG, TRACE, TRACE1,\n" \
	" TRACE2 or a number in the range 3..10.\n"

static const char* const DEFAULT_PIDFILE_PATH = "backend-std.pid";

#define WHITE_SPACE " \t"

static const logchannel_t logchannel = LOG_APP;

static const char* const help =
	"Usage: lircd [options] <config-file>\n"
	"\t -h --help\t\t\tDisplay this message\n"
	"\t -v --version\t\t\tDisplay version\n"
	"\t -O --options-file\t\tOptions file\n"
        "\t -i --immediate-init\t\tInitialize the device immediately at start\n"
	"\t -n --nodaemon\t\t\tDon't fork to background\n"
	"\t -H --driver=driver\t\tUse given driver (-H help lists drivers)\n"
	"\t -d --device=device\t\tRead from given device\n"
	"\t -U --plugindir=dir\t\tDir where drivers are loaded from\n"
	"\t -o --output=socket\t\tOutput socket filename\n"
	"\t -P --pidfile=file\t\tDaemon pid file\n"
	"\t -L --logfile=file\t\tLog file path (default: use syslog)'\n"
	"\t -D[level] --loglevel[=level]\t'info', 'warning', 'notice', etc., or 3..10.\n"
	"\t -r --release[=suffix]\t\tAuto-generate release events\n"
	"\t -Y --dynamic-codes\t\tEnable dynamic code generation\n"
	"\t -A --driver-options=key:value[|key:value...]\n"
	"\t\t\t\t\tSet driver options\n"
	"\t -e --effective-user=uid\t\tRun as uid after init as root\n"
	"\t -R --repeat-max=limit\t\tallow at most this many repeats\n";


static const struct option lircd_options[] = {
	{ "help",	    no_argument,       NULL, 'h' },
	{ "version",	    no_argument,       NULL, 'v' },
	{ "nodaemon",	    no_argument,       NULL, 'n' },
	{ "immediate-init", no_argument,       NULL, 'i' },
	{ "options-file",   required_argument, NULL, 'O' },
	{ "driver",	    required_argument, NULL, 'H' },
	{ "device",	    required_argument, NULL, 'd' },
	{ "output",	    required_argument, NULL, 'o' },
	{ "pidfile",	    required_argument, NULL, 'P' },
	{ "plugindir",	    required_argument, NULL, 'U' },
	{ "logfile",	    required_argument, NULL, 'L' },
	{ "loglevel",	    optional_argument, NULL, 'D' },
	{ "release",	    optional_argument, NULL, 'r' },
	{ "dynamic-codes",  no_argument,       NULL, 'Y' },
	{ "driver-options", required_argument, NULL, 'A' },
	{ "effective-user", required_argument, NULL, 'e' },
	{ "repeat-max",	    required_argument, NULL, 'R' },
	{ 0,		    0,		       0,    0	 }
};


#ifndef timersub
#define timersub(a, b, result)                                            \
	do {                                                                    \
		(result)->tv_sec = (a)->tv_sec - (b)->tv_sec;                         \
		(result)->tv_usec = (a)->tv_usec - (b)->tv_usec;                      \
		if ((result)->tv_usec < 0) {                                          \
			--(result)->tv_sec;                                                 \
			(result)->tv_usec += 1000000;                                       \
		}                                                                     \
	} while (0)
#endif


static struct ir_remote* free_remotes = NULL;

static int repeat_fd = -1;
static char* repeat_message = NULL;
static __u32 repeat_max = REPEAT_MAX_DEFAULT;

static const char* configfile = NULL;
static const char* pidfile_path = PIDFILE;
static const char* lircdfile = LIRCD;
static int sockfd;

static int do_shutdown;

static int nodaemon = 0;
static loglevel_t loglevel_opt = LIRC_NOLOG;

#define CT_LOCAL  1
#define CT_REMOTE 2

static int daemonized = 0;
static int userelease = 0;
static bool is_open = true; /*< Are there clients expecting input? */

static sig_atomic_t term = 0, hup = 0, alrm = 0;
static int termsig;

static __u32 setup_min_freq = 0, setup_max_freq = 0;
static lirc_t setup_max_gap = 0;
static lirc_t setup_min_pulse = 0, setup_min_space = 0;
static lirc_t setup_max_pulse = 0, setup_max_space = 0;

/* Use already opened hardware? */
int use_hw(void)
{
	return is_open || repeat_remote != NULL;
}


static int setup_frequency(void)
{
	__u32 freq;

	if (!(curr_driver->features & LIRC_CAN_SET_REC_CARRIER))
		return 1;
	if (setup_min_freq == 0 || setup_max_freq == 0) {
		setup_min_freq = DEFAULT_FREQ;
		setup_max_freq = DEFAULT_FREQ;
	}
	if (curr_driver->features & LIRC_CAN_SET_REC_CARRIER_RANGE && setup_min_freq != setup_max_freq) {
		if (curr_driver->drvctl_func(LIRC_SET_REC_CARRIER_RANGE, &setup_min_freq) == -1) {
			log_error("could not set receive carrier");
			log_perror_err(__func__);
			return 0;
		}
		freq = setup_max_freq;
	} else {
		freq = (setup_min_freq + setup_max_freq) / 2;
	}
	if (curr_driver->drvctl_func(LIRC_SET_REC_CARRIER, &freq) == -1) {
		log_error("could not set receive carrier");
		log_perror_err(__func__);
		return 0;
	}
	return 1;
}


static int setup_timeout(void)
{
	lirc_t val, min_timeout, max_timeout;
	__u32 enable = 1;

	if (!(curr_driver->features & LIRC_CAN_SET_REC_TIMEOUT))
		return 1;

	if (setup_max_space == 0)
		return 1;
	if (curr_driver->drvctl_func(LIRC_GET_MIN_TIMEOUT, &min_timeout) == -1
	    || curr_driver->drvctl_func(LIRC_GET_MAX_TIMEOUT, &max_timeout) == -1)
		return 0;
	if (setup_max_gap >= min_timeout && setup_max_gap <= max_timeout) {
		/* may help to detect end of signal faster */
		val = setup_max_gap;
	} else {
		/* keep timeout to a minimum */
		val = setup_max_space + 1;
		if (val < min_timeout)
			val = min_timeout;
		else if (val > max_timeout)
			/* maximum timeout smaller than maximum possible
			 * space, hmm */
			val = max_timeout;
	}

	if (curr_driver->drvctl_func(LIRC_SET_REC_TIMEOUT, &val) == -1) {
		log_error("could not set timeout");
		log_perror_err(__func__);
		return 0;
	}
	curr_driver->drvctl_func(LIRC_SET_REC_TIMEOUT_REPORTS, &enable);
	return 1;
}


static int setup_filter(void)
{
	int ret1, ret2;
	lirc_t min_pulse_supported = 0, max_pulse_supported = 0;
	lirc_t min_space_supported = 0, max_space_supported = 0;

	if (!(curr_driver->features & LIRC_CAN_SET_REC_FILTER))
		return 1;
	if (curr_driver->drvctl_func(LIRC_GET_MIN_FILTER_PULSE,
				     &min_pulse_supported) == -1 ||
	    curr_driver->drvctl_func(LIRC_GET_MAX_FILTER_PULSE, &max_pulse_supported) == -1
	    || curr_driver->drvctl_func(LIRC_GET_MIN_FILTER_SPACE, &min_space_supported) == -1
	    || curr_driver->drvctl_func(LIRC_GET_MAX_FILTER_SPACE, &max_space_supported) == -1) {
		log_error("could not get filter range");
		log_perror_err(__func__);
	}

	if (setup_min_pulse > max_pulse_supported)
		setup_min_pulse = max_pulse_supported;
	else if (setup_min_pulse < min_pulse_supported)
		setup_min_pulse = 0;    /* disable filtering */

	if (setup_min_space > max_space_supported)
		setup_min_space = max_space_supported;
	else if (setup_min_space < min_space_supported)
		setup_min_space = 0;    /* disable filtering */

	ret1 = curr_driver->drvctl_func(LIRC_SET_REC_FILTER_PULSE, &setup_min_pulse);
	ret2 = curr_driver->drvctl_func(LIRC_SET_REC_FILTER_SPACE, &setup_min_space);
	if (ret1 == -1 || ret2 == -1) {
		if (curr_driver->
		    drvctl_func(LIRC_SET_REC_FILTER,
				setup_min_pulse < setup_min_space ? &setup_min_pulse : &setup_min_space) == -1) {
			log_error("could not set filter");
			log_perror_err(__func__);
			return 0;
		}
	}
	return 1;
}


static int setup_hardware(void)
{
	int ret = 1;

	if (curr_driver->fd != -1 && curr_driver->drvctl_func) {
		if ((curr_driver->features & LIRC_CAN_SET_REC_CARRIER)
		    || (curr_driver->features & LIRC_CAN_SET_REC_TIMEOUT)
		    || (curr_driver->features & LIRC_CAN_SET_REC_FILTER)) {
			(void)curr_driver->drvctl_func(LIRC_SETUP_START, NULL);
			ret = setup_frequency() && setup_timeout()
			      && setup_filter();
			(void)curr_driver->drvctl_func(LIRC_SETUP_END, NULL);
		}
	}
	return ret;
}


void config(void)
{
	FILE* fd;
	struct ir_remote* config_remotes;
	const char* filename = configfile;

	if (filename == NULL)
		filename = LIRCDCFGFILE;

	if (free_remotes != NULL) {
		log_error("cannot read config file");
		log_error("old config is still in use");
		return;
	}
	fd = fopen(filename, "r");
	if (fd == NULL && errno == ENOENT && configfile == NULL) {
		/* try old lircd.conf location */
		int save_errno = errno;

		fd = fopen(LIRCDOLDCFGFILE, "r");
		if (fd != NULL)
			filename = LIRCDOLDCFGFILE;
		else
			errno = save_errno;
	}
	if (fd == NULL) {
		log_perror_err("could not open config file '%s'", filename);
		return;
	}
	configfile = filename;
	config_remotes = read_config(fd, configfile);
	fclose(fd);
	if (config_remotes == (void*)-1) {                     // NOLINT
		log_error("reading of config file failed");
	} else {
		log_trace("config file read");
		if (config_remotes == NULL) {
			log_warn("config file %s contains no valid remote control definition",
				  filename);
		}
		/* I cannot free the data structure
		 * as they could still be in use */
		free_remotes = get_remotes();
		set_remotes(config_remotes);

		get_frequency_range(get_remotes(), &setup_min_freq, &setup_max_freq);
		get_filter_parameters(get_remotes(), &setup_max_gap, &setup_min_pulse, &setup_min_space, &setup_max_pulse,
				      &setup_max_space);

		setup_hardware();
	}
}


void sigterm(int sig)
{
	/* all signals are blocked now */
	if (term)
		return;
	term = 1;
	termsig = sig;
}

void dosigterm(int sig)
{
	signal(SIGALRM, SIG_IGN);
	log_notice("caught signal");

	if (free_remotes != NULL)
		free_config(free_remotes);
	free_config(get_remotes());
	repeat_remote = NULL;
	if (do_shutdown)
		shutdown(sockfd, 2);
	close(sockfd);

	Pidfile::instance()->close();
	if (use_hw() && curr_driver->deinit_func)
		curr_driver->deinit_func();
	if (curr_driver->close_func)
		curr_driver->close_func();
	lirc_log_close();
	signal(sig, SIG_DFL);
	if (sig == SIGUSR1)
		exit(0);
	raise(sig);
}


void sighup(int sig)
{
	hup = 1;
}


void dosighup(int sig)
{
	/* reopen logfile first */
	if (lirc_log_reopen() != 0) {
		/* can't print any error messagees */
		dosigterm(SIGTERM);
	}
	config();
}


void nolinger(int sock)
{
	static struct linger linger = { 0, 0 };
	int lsize = sizeof(struct linger);

	setsockopt(sock, SOL_SOCKET, SO_LINGER,
		   reinterpret_cast<void*>(&linger), lsize);
}


void drop_privileges(void)
{
	const char* user;
	struct passwd* pw;
	lirc_gid groups[32];
	int group_cnt = sizeof(groups)/sizeof(gid_t);
	char groupnames[256] = {0};
	char buff[12];
	int r;
	int i;

	if (getuid() != 0)
		return;
	user = options_getstring("lircd:effective-user");
	if (user == NULL || strlen(user) == 0) {
		log_warn("Running as root");
		return;
	}
	pw = getpwnam(user);
	if (pw == NULL) {
		log_perror_warn("Illegal effective uid: %s", user);
		return;
	}
	r = getgrouplist(user, pw->pw_gid, groups, &group_cnt);
	if (r == -1) {
		log_perror_warn("Cannot get supplementary groups");
		return;
	}
	r = setgroups(group_cnt, (const gid_t*) groups);
	if (r == -1) {
		log_perror_warn("Cannot set supplementary groups");
		return;
	}
	r = setgid(pw->pw_gid);
	if (r == -1) {
		log_perror_warn("Cannot set GID");
		return;
	}
	r = setuid(pw->pw_uid);
	if (r == -1) {
		log_perror_warn("Cannot change UID");
		return;
	}
	log_notice("Running as user %s", user);
	for (i = 0; i < group_cnt; i += 1) {
		snprintf(buff, 5, " %d", groups[i]);
		strcat(groupnames, buff);
	}
	log_debug("Groups: [%d]:%s", pw->pw_gid, groupnames);
}


/** Creates global pidfile and obtain the lock on it. Exits on errors */
void create_pidfile()
{
	Pidfile::lock_result result;
	Pidfile* pidfile = Pidfile::instance();

	/* create pid lockfile in /var/run */
        result = pidfile->lock(pidfile_path);
        switch (result) {
	case Pidfile::OK:
		break;
	case Pidfile::CANT_CREATE:
		perrorf("Can't open or create %s", pidfile_path);
		exit(EXIT_FAILURE);
	case Pidfile::LOCKED_BY_OTHER:
		fprintf(stderr,
			"lircd: There seems to already be a lircd process with pid %d\n",
			pidfile->other_pid);
		fprintf(stderr,
			"lircd: Otherwise delete stale lockfile %s\n",
			pidfile_path);
		exit(EXIT_FAILURE);
	case Pidfile::CANT_PARSE:
		fprintf(stderr, "lircd: Invalid pidfile %s encountered\n",
			pidfile_path);
		exit(EXIT_FAILURE);
	}
}


void start_server(int nodaemon, loglevel_t loglevel)
{
	struct sockaddr_un serv_addr;
	int r;

	lirc_log_open("lircd", nodaemon, loglevel);
	create_pidfile();

	ir_remote_init(options_getboolean("lircd:dynamic-codes"));

	/* open lircd backend socket */
	do_shutdown = 0;
	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd == -1) {
		perror("Could not create socket");
		Pidfile::instance()->close();
		exit(EXIT_FAILURE);
	}
	do_shutdown = 1;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sun_family = AF_UNIX;
	snprintf(serv_addr.sun_path, sizeof(serv_addr.sun_path), lircdfile);
	r = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	if (r != 0) {
		perror("Cannot connect to lircd");
		Pidfile::instance()->close();
		exit(EXIT_FAILURE);
	}
	nolinger(sockfd);

	drop_privileges();
	log_debug("Connected to server socket");
	return;
}


void daemonize(void)
{
	if (daemon(0, 0) == -1) {
		log_perror_err("daemon() failed");
		dosigterm(SIGTERM);
	}
	Pidfile::instance()->update(getpid());
	daemonized = 1;
}


void sigalrm(int sig)
{
	alrm = 1;
}


static void schedule_repeat_timer(struct timespec* last)
{
	unsigned long secs;			// NOLINT
	lirc_t usecs, gap, diff;
	struct timespec current;
	struct itimerval repeat_timer;
	gap = send_buffer_sum() + repeat_remote->min_remaining_gap;
	clock_gettime(CLOCK_MONOTONIC, &current);
	secs = current.tv_sec - last->tv_sec;
	diff = 1000000 * secs + (current.tv_nsec - last->tv_nsec) / 1000;
	usecs = (diff < gap ? gap - diff : 0);
	if (usecs < 10)
		usecs = 10;
	log_trace("alarm in %lu usecs", (unsigned long)usecs);              // NOLINT
	repeat_timer.it_value.tv_sec = 0;
	repeat_timer.it_value.tv_usec = usecs;
	repeat_timer.it_interval.tv_sec = 0;
	repeat_timer.it_interval.tv_usec = 0;

	setitimer(ITIMER_REAL, &repeat_timer, NULL);
}

void dosigalrm(int sig)
{
	if (repeat_remote->last_code != repeat_code) {
		/* we received a different code from the original
		 * remote control we could repeat the wrong code so
		 * better stop repeating */
		if (repeat_fd != -1)
			send_error(repeat_fd, repeat_message, "repeating interrupted\n");

		repeat_remote = NULL;
		repeat_code = NULL;
		repeat_fd = -1;
		if (repeat_message != NULL) {
			free(repeat_message);
			repeat_message = NULL;
		}
		if (!use_hw() && curr_driver->deinit_func)
			curr_driver->deinit_func();
		return;
	}
	if (repeat_code->next == NULL
	    || (repeat_code->transmit_state != NULL && repeat_code->transmit_state->next == NULL))
		repeat_remote->repeat_countdown--;
	struct timespec before_send;
	clock_gettime(CLOCK_MONOTONIC, &before_send);
	if (send_ir_ncode(repeat_remote, repeat_code, 1) && repeat_remote->repeat_countdown > 0) {
		schedule_repeat_timer(&before_send);
		return;
	}
	repeat_remote = NULL;
	repeat_code = NULL;
	if (repeat_fd != -1) {
		send_success(repeat_fd, repeat_message);
		free(repeat_message);
		repeat_message = NULL;
		repeat_fd = -1;
	}
	if (!use_hw() && curr_driver->deinit_func)
		curr_driver->deinit_func();
}


void broadcast_message(const char* message)
{
	int len;

	len = strlen(message);
	if (get_events_fd() >= 0)
		write_socket(get_events_fd(), message, len);
	else
		log_notice("No fifo, dropping decoded event.");
}


void input_message(const char* message, const char* remote_name, const char* button_name, int reps, int release)
{
	const char* release_message;
	const char* release_remote_name;
	const char* release_button_name;

	release_message = check_release_event(&release_remote_name, &release_button_name);
	if (release_message)
		input_message(release_message, release_remote_name, release_button_name, 0, 1);

	if (!release || userelease)
		broadcast_message(message);
}


void free_old_remotes(void)
{
	struct ir_remote* scan_remotes;
	struct ir_remote* found;
	struct ir_ncode* code;
	const char* release_event;
	const char* release_remote_name;
	const char* release_button_name;

	if (get_decoding() == free_remotes)
		return;

	release_event = release_map_remotes(free_remotes, get_remotes(), &release_remote_name, &release_button_name);
	if (release_event != NULL)
		input_message(release_event, release_remote_name, release_button_name, 0, 1);
	if (last_remote != NULL) {
		if (is_in_remotes(free_remotes, last_remote)) {
			log_info("last_remote found");
			found = get_ir_remote(get_remotes(), last_remote->name);
			if (found != NULL) {
				code = get_code_by_name(found, last_remote->last_code->name);
				if (code != NULL) {
					found->reps = last_remote->reps;
					found->toggle_bit_mask_state = last_remote->toggle_bit_mask_state;
					found->min_remaining_gap = last_remote->min_remaining_gap;
					found->max_remaining_gap = last_remote->max_remaining_gap;
					found->last_send = last_remote->last_send;
					last_remote = found;
					last_remote->last_code = code;
					log_info("mapped last_remote");
				}
			}
		} else {
			last_remote = NULL;
		}
	}
	/* check if last config is still needed */
	found = NULL;
	if (repeat_remote != NULL) {
		scan_remotes = free_remotes;
		while (scan_remotes != NULL) {
			if (repeat_remote == scan_remotes) {
				found = repeat_remote;
				break;
			}
			scan_remotes = scan_remotes->next;
		}
		if (found != NULL) {
			found = get_ir_remote(get_remotes(), repeat_remote->name);
			if (found != NULL) {
				code = get_code_by_name(found, repeat_code->name);
				if (code != NULL) {
					struct itimerval repeat_timer;

					repeat_timer.it_value.tv_sec = 0;
					repeat_timer.it_value.tv_usec = 0;
					repeat_timer.it_interval.tv_sec = 0;
					repeat_timer.it_interval.tv_usec = 0;

					found->last_code = code;
					found->last_send = repeat_remote->last_send;
					found->toggle_bit_mask_state = repeat_remote->toggle_bit_mask_state;
					found->min_remaining_gap = repeat_remote->min_remaining_gap;
					found->max_remaining_gap = repeat_remote->max_remaining_gap;

					setitimer(ITIMER_REAL, &repeat_timer, &repeat_timer);
					/* "atomic" (shouldn't be necessary any more) */
					repeat_remote = found;
					repeat_code = code;
					/* end "atomic" */
					setitimer(ITIMER_REAL, &repeat_timer, NULL);
					found = NULL;
				}
			} else {
				found = repeat_remote;
			}
		}
	}
	if (found == NULL && get_decoding() != free_remotes) {
		free_config(free_remotes);
		free_remotes = NULL;
	} else {
		log_trace("free_remotes still in use");
	}
}

struct pollfd_byname {
	struct pollfd sockfd;
	struct pollfd curr_driver;
};

#define POLLFDS_SIZE (sizeof(struct pollfd_byname)/sizeof(pollfd))

static union {
	struct  pollfd_byname byname;
	struct  pollfd byindex[POLLFDS_SIZE];
} poll_fds;


static int mywaitfordata(unsigned long maxusec)                    // NOLINT
{
	int i;
	int ret, reconnect;
	struct timeval tv = {0};
	struct timeval start = {0};
	struct timeval now = {0};
	struct timeval timeout = {0};
	struct timeval release_time = {0};
	loglevel_t oldlevel;
	while (1) {
		do {
			/* handle signals */
			if (term)
				dosigterm(termsig);
				/* Not reached */
			if (hup) {
				dosighup(SIGHUP);
				hup = 0;
			}
			if (alrm) {
				dosigalrm(SIGALRM);
				alrm = 0;
			}
			memset(&poll_fds, 0, sizeof(poll_fds));
			for (i = 0; i < static_cast<int>(POLLFDS_SIZE); i += 1)
				poll_fds.byindex[i].fd = -1;

			poll_fds.byname.sockfd.fd = sockfd;
			poll_fds.byname.sockfd.events = POLLIN;

			if (use_hw() && curr_driver->rec_mode != 0 && curr_driver->fd != -1) {
				poll_fds.byname.curr_driver.fd = curr_driver->fd;
				poll_fds.byname.curr_driver.events = POLLIN;
			}

			reconnect = 0;
			if (timerisset(&tv)) {
				gettimeofday(&now, NULL);
				if (timercmp(&now, &tv, >)) {
					timerclear(&tv);
				} else {
					timersub(&tv, &now, &start);
					tv = start;
				}
				reconnect = 1;
			}
			gettimeofday(&start, NULL);
			if (maxusec > 0) {
				tv.tv_sec = maxusec / 1000000;
				tv.tv_usec = maxusec % 1000000;
			}
			if (curr_driver->fd == -1 && use_hw()) {
				/* try to reconnect */
				timerclear(&timeout);
				timeout.tv_sec = 1;

				if (timercmp(&tv, &timeout, >)
				    || (!reconnect && !timerisset(&tv)))
					tv = timeout;
			}
			get_release_time(&release_time);
			if (timerisset(&release_time)) {
				gettimeofday(&now, NULL);
				if (timercmp(&now, &release_time, >)) {
					timerclear(&tv);
				} else {
					struct timeval gap;

					timersub(&release_time, &now, &gap);
					if (!(timerisset(&tv)
					      || reconnect)
					      || timercmp(&tv, &gap, >)) {
						tv = gap;
					}
				}
			}
			if (timerisset(&tv) || timerisset(&release_time) || reconnect)
				ret = poll((struct pollfd *) &poll_fds.byindex,
					    POLLFDS_SIZE,
					    tv.tv_sec * 1000 + tv.tv_usec / 1000);
			else
				ret = poll((struct pollfd*)&poll_fds.byindex, POLLFDS_SIZE, -1);

			if (ret == -1 && errno != EINTR) {
				log_perror_err("poll()() failed");
				raise(SIGTERM);
				continue;
			}
			gettimeofday(&now, NULL);
			if (timerisset(&release_time) && timercmp(&now, &release_time, >)) {
				const char* release_message;
				const char* release_remote_name;
				const char* release_button_name;

				release_message =
					trigger_release_event(&release_remote_name,
							      &release_button_name);
				if (release_message) {
					input_message(release_message,
						      release_remote_name,
						      release_button_name,
						      0, 1);
				}
			}
			if (free_remotes != NULL)
				free_old_remotes();
			if (maxusec > 0) {
				if (ret == 0)
					return 0;
				if (time_elapsed(&start, &now) >= maxusec)
					return 0;
				maxusec -= time_elapsed(&start, &now);
			}
		} while (ret == -1 && errno == EINTR);
		if (curr_driver->fd == -1 && use_hw() && curr_driver->init_func) {
			oldlevel = loglevel;
			lirc_log_setlevel(LIRC_ERROR);
			curr_driver->init_func();
			setup_hardware();
			lirc_log_setlevel(oldlevel);
		}
		if (poll_fds.byname.sockfd.revents & POLLIN) {
			get_command(poll_fds.byname.sockfd.fd);
		}
		if (use_hw() && curr_driver->rec_mode != 0
		    && curr_driver->fd != -1
		    && poll_fds.byname.curr_driver.revents & POLLIN) {
			register_input();
			/* we will read later */
			return 1;
		}
	}
}

void loop(void)
{
	char* message;

	log_notice("lircd(%s) ready, using %s", curr_driver->name, lircdfile);
	if (curr_driver->init_func) {
	 	// FIXME: Fix to handle default driver dynamic caps.
		if (!curr_driver->init_func()) {
			log_warn("Failed to initialize hardware");
		}
		if (curr_driver->deinit_func) {
			int status = curr_driver->deinit_func();
			if (!status)
				log_error("Failed to de-initialize hardware");
		}
	}

	while (1) {
		(void)mywaitfordata(0);
		if (!curr_driver->rec_func)
			continue;
		message = curr_driver->rec_func(get_remotes());

		if (message != NULL) {
			const char* remote_name;
			const char* button_name;
			int reps;

			if (curr_driver->drvctl_func && (curr_driver->features & LIRC_CAN_NOTIFY_DECODE))
				curr_driver->drvctl_func(LIRC_NOTIFY_DECODE, NULL);

			get_release_data(&remote_name, &button_name, &reps);

			input_message(message, remote_name, button_name, reps, 0);
		}
	}
}


static void lircd_add_defaults(void)
{
	char level[4];

	snprintf(level, sizeof(level), "%d", lirc_log_defaultlevel());

	const char* const defaults[] = {
		"lircd:nodaemon",	"False",
		"lircd:driver",		"devinput",
		"lircd:device",		NULL,
		"lircd:output",		LIRCD,
		"lircd:pidfile",	DEFAULT_PIDFILE_PATH,
		"lircd:logfile",	"syslog",
		"lircd:debug",		level,
		"lircd:release",	NULL,
		"lircd:dynamic-codes",	"False",
		"lircd:plugindir",	PLUGINDIR,
		"lircd:repeat-max",	DEFAULT_REPEAT_MAX,
		"lircd:configfile",	LIRCDCFGFILE,
		"lircd:driver-options",	"",
		"lircd:effective-user",	"",

		(const char*)NULL,	(const char*)NULL
	};
	options_add_defaults(defaults);
}


static void lircd_parse_options(int argc, char** const argv)
{
	int c;
	const char* optstring = "A:e:O:hvnpi:H:d:o:U:P:l::L:c:r::aR:D::Y"
#       if defined(__linux__)
				"u"
#       endif
	;			// NOLINT

	strncpy(progname, "lircd", sizeof(progname));
	optind = 1;
	lircd_add_defaults();
	while ((c = getopt_long(argc, argv, optstring, lircd_options, NULL))
	       != -1) {
		switch (c) {
		case 'h':
			fputs(help, stdout);
			exit(EXIT_SUCCESS);
		case 'v':
			printf("lircd %s\n", VERSION);
			exit(EXIT_SUCCESS);
		case 'e':
			if (getuid() != 0) {
				log_warn("Trying to set user while"
					 " not being root");
			}
			options_set_opt("lircd:effective-user", optarg);
			break;
		case 'O':
			break;
		case 'n':
			options_set_opt("lircd:nodaemon", "True");
			break;
                case 'i':
			options_set_opt("lircd:immediate-init", "True");
			break;
		case 'H':
			options_set_opt("lircd:driver", optarg);
			break;
		case 'd':
			options_set_opt("lircd:device", optarg);
			break;
		case 'P':
			options_set_opt("lircd:pidfile", optarg);
			break;
		case 'L':
			options_set_opt("lircd:logfile", optarg);
			break;
		case 'o':
			options_set_opt("lircd:output", optarg);
			break;
		case 'D':
			loglevel_opt = (loglevel_t) options_set_loglevel(
				optarg ? optarg : "debug");
			if (loglevel_opt == LIRC_BADLEVEL) {
				fprintf(stderr, DEBUG_HELP, optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'r':
			options_set_opt("lircd:release", "True");
			options_set_opt("lircd:release_suffix",
					optarg ? optarg : LIRC_RELEASE_SUFFIX);
			break;
		case 'U':
			options_set_opt("lircd:plugindir", optarg);
			break;
		case 'R':
			options_set_opt("lircd:repeat-max", optarg);
			break;
		case 'Y':
			options_set_opt("lircd:dynamic-codes", "True");
			break;
		case 'A':
			options_set_opt("lircd:driver-options", optarg);
			break;
		default:
			printf("Usage: %s [options] [config-file]\n", progname);
			exit(EXIT_FAILURE);
		}
	}
	if (optind == argc - 1) {
		options_set_opt("lircd:configfile", argv[optind]);
	} else if (optind != argc) {
		fprintf(stderr, "%s: invalid argument count\n", progname);
		exit(EXIT_FAILURE);
	}
}


int main(int argc, char** argv)
{
	struct sigaction act;
	const char* device = NULL;
	const char* opt;
	int immediate_init = 0;

	hw_choose_driver(NULL);
	options_load(argc, argv, NULL, lircd_parse_options);
	opt = options_getstring("lircd:debug");
	if (options_set_loglevel(opt) == LIRC_BADLEVEL) {
		fprintf(stderr, "Bad configuration loglevel:%s\n", opt);
		fprintf(stderr, DEBUG_HELP, optarg);
		fprintf(stderr, "Falling back to 'info'\n");
	}
	opt = options_getstring("lircd:logfile");
	if (opt != NULL)
		lirc_log_set_file(opt);
	lirc_log_open("lircd", 0, LIRC_INFO);

	immediate_init = options_getboolean("lircd:immediate-init");
	nodaemon = options_getboolean("lircd:nodaemon");
	device = options_getstring("lircd:device");
	opt = options_getstring("lircd:driver");
	if (strcmp(opt, "help") == 0 || strcmp(opt, "?") == 0) {
		hw_print_drivers(stdout);
		return EXIT_SUCCESS;
	}
	if (hw_choose_driver(opt) != 0) {
		fprintf(stderr, "Driver `%s' not found or not loadable", opt);
		fprintf(stderr, " (wrong or missing -U/--plugindir?).\n");
		fputs("Use lirc-lsplugins(1) to list available drivers.\n",
                      stderr);
		hw_print_drivers(stderr);
		return EXIT_FAILURE;
	}
	curr_driver->open_func(device);
	opt = options_getstring("lircd:driver-options");
	if (opt != NULL)
		drv_handle_options(opt);
	pidfile_path = options_getstring("lircd:pidfile");
	lircdfile = options_getstring("lircd:output");
	opt = options_getstring("lircd:logfile");
	if (opt != NULL)
		lirc_log_set_file(opt);
	loglevel_opt = (loglevel_t) options_getint("lircd:debug");
	userelease = options_getboolean("lircd:release");
	set_release_suffix(options_getstring("lircd:release_suffix"));
	repeat_max = options_getint("lircd:repeat-max");
	configfile = options_getstring("lircd:configfile");
	curr_driver->open_func(device);
	if (strcmp(curr_driver->name, "null") == 0) {
		fprintf(stderr, "%s: there's no hardware I can use and no peers are specified\n", progname);
		return EXIT_FAILURE;
	}
	if (curr_driver->device != NULL && strcmp(curr_driver->device, lircdfile) == 0) {
		fprintf(stderr, "%s: refusing to connect to myself\n", progname);
		fprintf(stderr, "%s: device and output must not be the same file: %s\n", progname, lircdfile);
		return EXIT_FAILURE;
	}

	signal(SIGPIPE, SIG_IGN);

	start_server(nodaemon, loglevel_opt);

	act.sa_handler = sigterm;
	sigfillset(&act.sa_mask);
	act.sa_flags = SA_RESTART;      /* don't fiddle with EINTR */
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);

	act.sa_handler = sigalrm;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;      /* don't fiddle with EINTR */
	sigaction(SIGALRM, &act, NULL);

	act.sa_handler = dosigterm;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &act, NULL);

	config();               /* read config file */

	act.sa_handler = sighup;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;      /* don't fiddle with EINTR */
	sigaction(SIGHUP, &act, NULL);

	if (immediate_init && curr_driver->init_func) {
		log_info("Doing immediate init, as requested");
		int status = curr_driver->init_func();
		if (status) {
			setup_hardware();
		} else {
			log_error("Failed to initialize hardware");
			return(EXIT_FAILURE);
		}
		if (curr_driver->deinit_func) {
			int status = curr_driver->deinit_func();
			if (!status)
				log_error("Failed to de-initialize hardware");
		}
	}

	/* ready to accept connections */
	if (!nodaemon)
		daemonize();

	loop();

	/* never reached */
	return EXIT_SUCCESS;
}