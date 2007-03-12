/*
 * libstp - staprun 'library'
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) IBM Corporation, 2005
 * Copyright (C) Red Hat Inc, 2005, 2006
 *
 */
#include "librelay.h"
#include <signal.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/fd.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <linux/types.h>
#include <linux/limits.h>
#include <sys/wait.h>
#include <sys/statfs.h>


/* stp_check script */
#ifdef PKGLIBDIR
char *stp_check=PKGLIBDIR "/stp_check";
#else
char *stp_check="stp_check";
#endif

/* maximum number of CPUs we can handle - change if more */
#define NR_CPUS 256

/* relayfs parameters */
static struct params
{
	unsigned subbuf_size;
	unsigned n_subbufs;
	int merge;
	char relay_filebase[256];
} params;

/* temporary per-cpu output written here for relayfs, filebase0...N */
static char *percpu_tmpfilebase = "stpd_cpu";

/* procfs files */
static char proc_filebase[128];
static int proc_file[NR_CPUS];

/* probe output written here, if non-NULL */
/* if no output file name is specified, use this */
#define DEFAULT_RELAYFS_OUTFILE_NAME "probe.out"
extern char *outfile_name;

/* internal variables */
static int transport_mode;
static int ncpus;
static int print_totals;
static int exiting;

/* per-cpu data */
static int relay_file[NR_CPUS];
static FILE *percpu_tmpfile[NR_CPUS];
static char *relay_buffer[NR_CPUS];
static pthread_t reader[NR_CPUS];

/* control channel */
int control_channel;

/* flags */
extern int print_only, quiet, verbose;
extern unsigned int buffer_size;
extern char *modname;
extern char *modpath;
extern char *modoptions[];
extern int target_pid;
extern int driver_pid;
extern char *target_cmd;

/* uid/gid to use when execing external programs */
extern uid_t cmd_uid;
extern gid_t cmd_gid;

/* per-cpu buffer info */
static struct buf_status
{
	struct _stp_buf_info info;
	unsigned max_backlog; /* max # sub-buffers ready at one time */
} status[NR_CPUS];



/**
 *	streaming - is the current transport mode streaming or not?
 *
 *	Returns 1 if in streaming mode, 0 otherwise.
 */
static int streaming(void)
{
	if (transport_mode == STP_TRANSPORT_PROC)
		return 1;

	return 0;
}

/**
 *	send_request - send request to kernel over control channel
 *	@type: the relay-app command id
 *	@data: pointer to the data to be sent
 *	@len: length of the data to be sent
 *
 *	Returns 0 on success, negative otherwise.
 */
int send_request(int type, void *data, int len)
{
	char buf[1024];
	memcpy(buf, &type, 4);
	memcpy(&buf[4],data,len);
	return write(control_channel, buf, len+4);
}


/**
 *	summarize - print a summary if applicable
 */
static void summarize(void)
{
	int i;

	if (transport_mode != STP_TRANSPORT_RELAYFS)
		return;

	printf("summary:\n");
	for (i = 0; i < ncpus; i++) {
		printf("cpu %u:\n", i);
		printf("    %u sub-buffers processed\n",
		       status[i].info.consumed);
		printf("    %u max backlog\n", status[i].max_backlog);
	}
}

static void close_proc_files()
{
	int i;
	for (i = 0; i < ncpus; i++)
	  close(proc_file[i]);
}

/**
 *	close_relayfs_files - close and munmap buffer and open output file
 */
static void close_relayfs_files(int cpu)
{
	size_t total_bufsize = params.subbuf_size * params.n_subbufs;
	
	munmap(relay_buffer[cpu], total_bufsize);
	close(relay_file[cpu]);
	relay_file[cpu] = 0;
	fclose(percpu_tmpfile[cpu]);
}

/**
 *	close_all_relayfs_files - close and munmap buffers and output files
 */
static void close_all_relayfs_files(void)
{
	int i;

	if (!streaming()) {
		for (i = 0; i < ncpus; i++)
			close_relayfs_files(i);
	}
}

/**
 *	open_relayfs_files - open and mmap buffer and open output file
 */
static int open_relayfs_files(int cpu, const char *relay_filebase)
{
	size_t total_bufsize;
	char tmp[PATH_MAX];

	memset(&status[cpu], 0, sizeof(struct buf_status));
	status[cpu].info.cpu = cpu;

	sprintf(tmp, "%s%d", relay_filebase, cpu);
	relay_file[cpu] = open(tmp, O_RDONLY | O_NONBLOCK);
	if (relay_file[cpu] < 0) {
		fprintf(stderr, "ERROR: couldn't open relayfs file %s: errcode = %s\n", tmp, strerror(errno));
		return -1;
	}

	sprintf(tmp, "%s/%d", proc_filebase, cpu);
	dbug("Opening %s.\n", tmp); 
	proc_file[cpu] = open(tmp, O_RDWR | O_NONBLOCK);
	if (proc_file[cpu] < 0) {
		fprintf(stderr, "ERROR: couldn't open proc file %s: errcode = %s\n", tmp, strerror(errno));
		return -1;
	}

	sprintf(tmp, "%s%d", percpu_tmpfilebase, cpu);	
	if((percpu_tmpfile[cpu] = fopen(tmp, "w+")) == NULL) {
		fprintf(stderr, "ERROR: Couldn't open output file %s: errcode = %s\n", tmp, strerror(errno));
		close(relay_file[cpu]);
		return -1;
	}

	total_bufsize = params.subbuf_size * params.n_subbufs;
	relay_buffer[cpu] = mmap(NULL, total_bufsize, PROT_READ,
				 MAP_PRIVATE | MAP_POPULATE, relay_file[cpu],
				 0);
	if(relay_buffer[cpu] == MAP_FAILED)
	{
		fprintf(stderr, "ERROR: couldn't mmap relay file, total_bufsize (%d) = subbuf_size (%d) * n_subbufs(%d), error = %s \n", (int)total_bufsize, (int)params.subbuf_size, (int)params.n_subbufs, strerror(errno));
		close(relay_file[cpu]);
		fclose(percpu_tmpfile[cpu]);
		return -1;
	}

	return 0;
}

/**
 *	delete_percpu_files - delete temporary per-cpu output files
 *
 *	Returns 0 if successful, -1 otherwise.
 */
static int delete_percpu_files(void)
{
	int i;
	char tmp[PATH_MAX];

	for (i = 0; i < ncpus; i++) {
		sprintf(tmp, "%s%d", percpu_tmpfilebase, i);
		if (unlink(tmp) < 0) {
			fprintf(stderr, "ERROR: couldn't unlink percpu file %s: errcode = %s\n", tmp, strerror(errno));
			return -1;
		}
	}
	return 0;
}

/**
 *	kill_percpu_threads - kill per-cpu threads 0->n-1
 *	@n: number of threads to kill
 *
 *	Returns number of threads killed.
 */
static int kill_percpu_threads(int n)
{
	int i, killed = 0;

	for (i = 0; i < n; i++) {
		if (pthread_cancel(reader[i]) == 0)
			killed++;
	}

	return killed;
}

/**
 *     wait_for_percpu_threads - wait for all threads to exit
 *     @n: number of threads to wait for
 */
static void wait_for_percpu_threads(int n)
{
	int i;

	for (i = 0; i < n; i++)
		pthread_join(reader[i], NULL);
}

/**
 *	process_subbufs - write ready subbufs to disk
 */
static int process_subbufs(struct _stp_buf_info *info)
{
	unsigned subbufs_ready, start_subbuf, end_subbuf, subbuf_idx, i;
	int len, cpu = info->cpu;
	char *subbuf_ptr;
	int subbufs_consumed = 0;
	unsigned padding;

	subbufs_ready = info->produced - info->consumed;
	start_subbuf = info->consumed % params.n_subbufs;
	end_subbuf = start_subbuf + subbufs_ready;

	for (i = start_subbuf; i < end_subbuf; i++) {
		subbuf_idx = i % params.n_subbufs;
		subbuf_ptr = relay_buffer[cpu] + subbuf_idx * params.subbuf_size;
		padding = *((unsigned *)subbuf_ptr);
		subbuf_ptr += sizeof(padding);
		len = (params.subbuf_size - sizeof(padding)) - padding;
		if (len) {
			if (fwrite_unlocked (subbuf_ptr, len, 1, percpu_tmpfile[cpu]) != 1) {
				fprintf(stderr, "ERROR: couldn't write to output file for cpu %d, exiting: errcode = %d: %s\n", cpu, errno, strerror(errno));
				exit(1);
			}
		}
		subbufs_consumed++;
	}

	return subbufs_consumed;
}

/**
 *	reader_thread - per-cpu channel buffer reader
 */
static void *reader_thread(void *data)
{
	int rc;
	int cpu = (long)data;
	struct pollfd pollfd;
	struct _stp_consumed_info consumed_info;
	unsigned subbufs_consumed;

	pollfd.fd = relay_file[cpu];
	pollfd.events = POLLIN;

	do {
		rc = poll(&pollfd, 1, -1);
		if (rc < 0) {
			if (errno != EINTR) {
				fprintf(stderr, "ERROR: poll error: %s\n",
					strerror(errno));
				exit(1);
			}
			fprintf(stderr, "WARNING: poll warning: %s\n",
				strerror(errno));
			rc = 0;
		}

		rc = read(proc_file[cpu], &status[cpu].info,
			  sizeof(struct _stp_buf_info));
		subbufs_consumed = process_subbufs(&status[cpu].info);
		if (subbufs_consumed) {
			if (subbufs_consumed > status[cpu].max_backlog)
				status[cpu].max_backlog = subbufs_consumed;
			status[cpu].info.consumed += subbufs_consumed;
			consumed_info.cpu = cpu;
			consumed_info.consumed = subbufs_consumed;
			if (write (proc_file[cpu], &consumed_info, sizeof(struct _stp_consumed_info)) < 0)
				fprintf(stderr,"WARNING: writing consumed info failed.\n");
		}
		if (status[cpu].info.flushing)
			pthread_exit(NULL);
	} while (1);
}

#define RELAYFS_MAGIC			0xF0B4A981
#define DEBUGFS_MAGIC			0x64626720
/**
 *	init_relayfs - create files and threads for relayfs processing
 *
 *	Returns 0 if successful, negative otherwise
 */
int init_relayfs(void)
{
	int i, j, wstat;
	pid_t pid;
	struct statfs st;

	dbug("initializing relayfs\n");

	/* first run the _stp_check script */
	if ((pid = fork()) < 0) {
		perror ("fork of stp_check failed.");
		exit(-1);
	} else if (pid == 0) {
		if (execlp(stp_check, stp_check, NULL) < 0)
			_exit (-1);
	}
	if (waitpid(pid, &wstat, 0) < 0) {
		perror("waitpid");
		exit(-1);
	}
	if (WIFEXITED(wstat) && WEXITSTATUS(wstat)) {
		perror (stp_check);
		fprintf(stderr, "Could not execute %s\n", stp_check);
		return -1;
	}

	if (statfs("/mnt/relay", &st) == 0 && (int) st.f_type == (int) RELAYFS_MAGIC)
 		sprintf(params.relay_filebase, "/mnt/relay/systemtap/%d/cpu", getpid());
 	else if (statfs("/sys/kernel/debug", &st) == 0 && (int) st.f_type == (int) DEBUGFS_MAGIC)
 		sprintf(params.relay_filebase, "/sys/kernel/debug/systemtap/%d/cpu", getpid());
 	else
 		sprintf(params.relay_filebase, "/debug/systemtap/%d/cpu", getpid());
	
	for (i = 0; i < ncpus; i++) {
		if (open_relayfs_files(i, params.relay_filebase) < 0) {
			fprintf(stderr, "ERROR: couldn't open relayfs files, cpu = %d\n", i);
			goto err;
		}
		/* create a thread for each per-cpu buffer */
		if (pthread_create(&reader[i], NULL, reader_thread, (void *)(long)i) < 0) {
			close_relayfs_files(i);
			fprintf(stderr, "ERROR: Couldn't create reader thread, cpu = %d\n", i);
			goto err;
		}
	}

        if (print_totals && verbose)
          printf("Using channel with %u sub-buffers of size %u.\n",
                 params.n_subbufs, params.subbuf_size);

	return 0;
err:
	for (j = 0; j < i; j++)
		close_relayfs_files(j);
	kill_percpu_threads(i);

	return -1;
}

void start_cmd(void)
{
	pid_t pid;
	sigset_t usrset;
		
	sigemptyset(&usrset);
	sigaddset(&usrset, SIGUSR1);
	sigprocmask(SIG_BLOCK, &usrset, NULL);

	dbug ("execing target_cmd %s\n", target_cmd);
	if ((pid = fork()) < 0) {
		perror ("fork");
		exit(-1);
	} else if (pid == 0) {
		int signum;

		if (setregid(cmd_gid, cmd_gid) < 0) {
			perror("setregid");
		}
		if (setreuid(cmd_uid, cmd_uid) < 0) {
			perror("setreuid");
		}
		/* wait here until signaled */
		sigwait(&usrset, &signum);

		if (execl("/bin/sh", "sh", "-c", target_cmd, NULL) < 0)
			perror(target_cmd);
		_exit(-1);
	}
	target_pid = pid;
}

void system_cmd(char *cmd)
{
	pid_t pid;

	dbug ("system %s\n", cmd);
	if ((pid = fork()) < 0) {
		perror ("fork");
	} else if (pid == 0) {
		if (setregid(cmd_gid, cmd_gid) < 0) {
			perror("setregid");
		}
		if (setreuid(cmd_uid, cmd_uid) < 0) {
			perror("setreuid");
		}
		if (execl("/bin/sh", "sh", "-c", cmd, NULL) < 0)
			perror(cmd);
		_exit(-1);
	}
}

/**
 *	init_stp - initialize the app
 *	@print_summary: boolean, print summary or not at end of run
 *
 *	Returns 0 on success, negative otherwise.
 */
int init_stp(int print_summary)
{
	char buf[1024];
	struct _stp_transport_info ti;
	pid_t pid;
	int rstatus;

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	print_totals = print_summary;

	/* insert module */
	sprintf(buf, "_stp_pid=%d", (int)getpid());
        modoptions[0] = "insmod";
        modoptions[1] = modpath;
        modoptions[2] = buf;
        /* modoptions[3...N] set by command line parser. */

	if ((pid = fork()) < 0) {
		perror ("fork");
		exit(-1);
	} else if (pid == 0) {
		if (execvp("/sbin/insmod",  modoptions) < 0)
			_exit(-1);
	}
	if (waitpid(pid, &rstatus, 0) < 0) {
		perror("waitpid");
		exit(-1);
	}
	if (WIFEXITED(rstatus) && WEXITSTATUS(rstatus)) {
		fprintf(stderr, "ERROR, couldn't insmod probe module %s\n", modpath);
		return -1;
	}

        /* We no longer use /proc/systemtap/, just /proc.  */
	sprintf (proc_filebase, "/proc/%s", modname);
	char *ptr = index(proc_filebase,'.');
	if (ptr)
		*ptr = 0;

	sprintf(buf, "%s/cmd", proc_filebase);
	dbug("Opening %s\n", buf); 
	control_channel = open(buf, O_RDWR);
	if (control_channel < 0) {
		fprintf(stderr, "ERROR: couldn't open control channel %s: errcode = %s\n", buf, strerror(errno));
		goto do_rmmod;
	}

	/* start target_cmd if necessary */
	if (target_cmd)
		start_cmd();

	/* now send TRANSPORT_INFO */
	ti.buf_size = buffer_size;
	ti.subbuf_size = 0;
	ti.n_subbufs = 0;
	ti.target = target_pid;
	if (send_request(STP_TRANSPORT_INFO, &ti, sizeof(ti)) < 0) {
		fprintf(stderr, "staprun failed because TRANSPORT_INFO returned an error.\n");
		if (target_cmd)
			kill (target_pid, SIGKILL);
		close(control_channel);
		goto do_rmmod;
	}
	return 0;

 do_rmmod:
	snprintf(buf, sizeof(buf), "/sbin/rmmod -w %s", modname);
	if (system(buf))
		fprintf(stderr, "ERROR: couldn't rmmod probe module %s.\n", modname);
	return -1;
}


/* length of timestamp in output field 0 */
#define TIMESTAMP_SIZE (sizeof(uint32_t))

/**
 *	merge_output - merge per-cpu output
 *
 */
#define MERGE_BUF_SIZE 16*1024
static int merge_output(void)
{
	char *buf[ncpus], tmp[PATH_MAX];
	int i, j, dropped=0;
	uint32_t count=0, min, num[ncpus];
	FILE *ofp, *fp[ncpus];
	uint32_t length[ncpus];

	for (i = 0; i < ncpus; i++) {
		sprintf (tmp, "%s%d", percpu_tmpfilebase, i);
		fp[i] = fopen (tmp, "r");
		if (!fp[i]) {
			fprintf (stderr, "error opening file %s.\n", tmp);
			return -1;
		}
		num[i] = 0;
		buf[i] = malloc(MERGE_BUF_SIZE);
		if (!buf[i]) {
			fprintf(stderr,"Out of memory in merge_output(). Aborting merge.\n");
			printf("Out of memory in merge_output(). Aborting merge.\n");
			return -1;
		}

		if (fread_unlocked (&length[i], sizeof(uint32_t), 1, fp[i])) {
			if (fread_unlocked (buf[i], length[i]+TIMESTAMP_SIZE, 1, fp[i]))
				num[i] = *((uint32_t *)buf[i]);
		}
	}

	if (!outfile_name)
		outfile_name = DEFAULT_RELAYFS_OUTFILE_NAME;

	ofp = fopen (outfile_name, "w");
	if (!ofp) {
		fprintf (stderr, "ERROR: couldn't open output file %s: errcode = %s\n",
			 outfile_name, strerror(errno));
		return -1;
	}

	do {
		min = num[0];
		j = 0;
		for (i = 1; i < ncpus; i++) {
			if (min == 0 || (num[i] && num[i] < min)) {
				min = num[i];
				j = i;
			}
		}

		if (min && !quiet)
			fwrite_unlocked (buf[j]+TIMESTAMP_SIZE, length[j], 1, stdout);
		if (min && !print_only)
			fwrite_unlocked (buf[j]+TIMESTAMP_SIZE, length[j], 1, ofp);
		
		if (min && ++count != min) {
			count = min;
			dropped++ ;
		}

		num[j] = 0;
		if (fread_unlocked (&length[j], sizeof(uint32_t), 1, fp[j])) {
			if (fread_unlocked (buf[j], length[j]+TIMESTAMP_SIZE, 1, fp[j]))
				num[j] = *((uint32_t *)buf[j]);
		}
	} while (min);

	if (!print_only)
		fwrite_unlocked ("\n", 1, 1, ofp);

	for (i = 0; i < ncpus; i++)
		fclose (fp[i]);
	fclose (ofp);

	if (dropped)
		fprintf (stderr, "Sequence had %d drops.\n", dropped);

	return 0;
}

void cleanup_and_exit (int closed)
{
	char tmpbuf[128];
	pid_t err;

	if (exiting)
		return;
	exiting = 1;

	dbug("CLEANUP AND EXIT  closed=%d mode=%d\n", closed, transport_mode);

	/* what about child processes? we will wait for them here. */
	err = waitpid(-1, NULL, WNOHANG);
	if (err >= 0)
		fprintf(stderr,"\nWaititing for processes to exit\n");
	while(wait(NULL) > 0);

	if (transport_mode == STP_TRANSPORT_RELAYFS && relay_file[0] > 0)
		wait_for_percpu_threads(ncpus);

	close_proc_files();

	if (print_totals && verbose)
		summarize();

	if (transport_mode == STP_TRANSPORT_RELAYFS && relay_file[0] > 0) {
		close_all_relayfs_files();
		if (params.merge) {
			merge_output();
			delete_percpu_files();
		}
	}

	dbug("closing control channel\n");
	close(control_channel);

	if (!closed) {
		snprintf(tmpbuf, sizeof(tmpbuf), "/sbin/rmmod -w %s", modname);
		if (system(tmpbuf)) {
			fprintf(stderr, "ERROR: couldn't rmmod probe module %s.  No output will be written.\n",
				modname);
			exit(1);
		}
	}
	exit(0);
}

static void sigproc(int signum)
{
	if (signum == SIGCHLD) {
		pid_t pid = waitpid(-1, NULL, WNOHANG);
		if (pid != target_pid)
			return;
	}
	send_request(STP_EXIT, NULL, 0);
}

static void driver_poll (int signum __attribute__((unused)))
{
	/* See if the driver process is still alive.  If not, time to exit.  */
	if (kill (driver_pid, 0) < 0) {
		send_request(STP_EXIT, NULL, 0);
		return;
	} else  {
		/* Check again later. Use any reasonable poll interval */
		signal (SIGALRM, driver_poll);
		alarm (10); 
	}
}


/**
 *	stp_main_loop - loop forever reading data
 */
static char recvbuf[8192];

int stp_main_loop(void)
{
	int nb, rc;
	void *data;
	int type;
	FILE *ofp = stdout;

	setvbuf(ofp, (char *)NULL, _IOLBF, 0);

	signal(SIGINT, sigproc);
	signal(SIGTERM, sigproc);
	signal(SIGCHLD, sigproc);
	signal(SIGHUP, sigproc);

        if (driver_pid)
          driver_poll(0); // And by the way, I'm also the signal handler.

	dbug("in main loop\n");

	while (1) { /* handle messages from control channel */
		nb = read(control_channel, recvbuf, sizeof(recvbuf));
		if (nb <= 0) {
			perror("recv");
			fprintf(stderr, "WARNING: unexpected EOF. nb=%d\n", nb);
			continue;
		}

		type = *(int *)recvbuf;
		data = (void *)(recvbuf + sizeof(int));

		if (!transport_mode && type != STP_TRANSPORT_INFO && type != STP_EXIT) {
			fprintf(stderr, "WARNING: invalid stp command: no transport\n");
			continue;
		}

		switch (type) {
		case STP_REALTIME_DATA:
			fwrite_unlocked(data, nb - sizeof(int), 1, ofp);
			break;
		case STP_OOB_DATA:
			fputs ((char *)data, stderr);
			break;
		case STP_EXIT: 
		{
			/* module asks us to unload it and exit */
			int *closed = (int *)data;
			cleanup_and_exit(*closed);
			break;
		}
		case STP_START: 
		{
			struct _stp_transport_start *t = (struct _stp_transport_start *)data;
			dbug("probe_start() returned %d\n", t->pid);
			if (t->pid < 0) {
				if (target_cmd)
					kill (target_pid, SIGKILL);
				cleanup_and_exit(0);
			} else if (target_cmd)
				kill (target_pid, SIGUSR1);
			break;
		}
		case STP_SYSTEM:
		{
			struct _stp_cmd_info *c = (struct _stp_cmd_info *)data;
			system_cmd(c->cmd);
			break;
		}
		case STP_TRANSPORT_INFO:
		{
			struct _stp_transport_info *info = (struct _stp_transport_info *)data;
			struct _stp_transport_start ts;
			transport_mode = info->transport_mode;
			params.subbuf_size = info->subbuf_size;
			params.n_subbufs = info->n_subbufs;
			params.merge = info->merge;
#ifdef DEBUG
			if (transport_mode == STP_TRANSPORT_RELAYFS) {
				fprintf(stderr,"TRANSPORT_INFO recvd: RELAYFS %d bufs of %d bytes.\n", 
					params.n_subbufs, 
					params.subbuf_size);
				if (params.merge)
					fprintf(stderr,"Merge output\n");
			} else
				fprintf(stderr,"TRANSPORT_INFO recvd: PROC with %d Mbyte buffers.\n", 
					 info->buf_size); 
#endif
			if (!streaming()) {
				rc = init_relayfs();
				if (rc < 0) {
					fprintf(stderr, "ERROR: couldn't init relayfs, exiting\n");
					cleanup_and_exit(0);
				}
			} else if (outfile_name) {
				ofp = fopen (outfile_name, "w");
				if (!ofp) {
					fprintf (stderr, "ERROR: couldn't open output file %s: errcode = %s\n",
						 outfile_name, strerror(errno));
					cleanup_and_exit(0);
				}
			}
			ts.pid = getpid();
			send_request(STP_START, &ts, sizeof(ts));
			break;
		}
		case STP_MODULE:
		{
			struct _stp_transport_start ts;
			if (do_module(data)) {
			  ts.pid = getpid();
			  send_request(STP_START, &ts, sizeof(ts));
			}
			break;
		}		
		case STP_SYMBOLS:
		{
			struct _stp_symbol_req *req = (struct _stp_symbol_req *)data;
			struct _stp_transport_start ts;
			dbug("STP_SYMBOLS request received\n");
			if (req->endian != 0x1234) {
			  fprintf(stderr,"ERROR: staprun is compiled with different endianess than the kernel!\n");
			  cleanup_and_exit(0);
			}
			if (req->ptr_size != sizeof(char *)) {
			  fprintf(stderr,"ERROR: staprun is compiled with %d-bit pointers and the kernel uses %d-bit.\n",
				  8*sizeof(char *), 8*req->ptr_size);
			  cleanup_and_exit(0);
			}
			do_kernel_symbols();
			ts.pid = getpid();
			send_request(STP_START, &ts, sizeof(ts));
			break;
		}
		default:
			fprintf(stderr, "WARNING: ignored message of type %d\n", (type));
		}
	}
	fclose(ofp);
	return 0;
}
