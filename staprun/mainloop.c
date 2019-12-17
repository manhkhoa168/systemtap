/* -*- linux-c -*-
 *
 * mainloop - stapio main loop
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 *
 * Copyright (C) 2005-2018 Red Hat Inc.
 */

#include "staprun.h"
#include <sys/utsname.h>
#include <sys/ptrace.h>
#include <sys/select.h>
#include <search.h>
#include <wordexp.h>


#define WORKAROUND_BZ467568 1  /* PR 6964; XXX: autoconf when able */


/* globals */
int ncpus;
static int use_old_transport = 0;
static int pending_interrupts = 0;
static int target_pid_failed_p = 0;

/* Setup by setup_main_signals, used by signal_thread to notify the
   main thread of interruptable events. */
static pthread_t main_thread;

static void set_nonblocking_std_fds(void)
{
  int fd;
  for (fd = 1; fd < 3; fd++) {
    /* NB: writing to stderr/stdout blockingly in signal handler is
     * dangerous since it may prevent the stap process from quitting
     * gracefully on receiving SIGTERM/etc signals when the stderr/stdout
     * write buffer is full. PR23891 */
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1)
      continue;

    if (flags & O_NONBLOCK)
      continue;

    (void) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

static void set_blocking_std_fds(void)
{
  int fd;
  for (fd = 1; fd < 3; fd++) {
    /* NB: writing to stderr/stdout blockingly in signal handler is
     * dangerous since it may prevent the stap process from quitting
     * gracefully on receiving SIGTERM/etc signals when the stderr/stdout
     * write buffer is full. PR23891 */
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1)
      continue;

    if (!(flags & O_NONBLOCK))
      continue;

    (void) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  }
}

static void my_exit(int rc)
{
  /* to avoid leaving any side-effects on the stdout/stderr devices */
  if (pending_interrupts > 2)
    set_blocking_std_fds();

  _exit(rc);
}

static void *signal_thread(void *arg)
{
  sigset_t *s = (sigset_t *) arg;
  int signum = 0;

  while (1) {
    if (sigwait(s, &signum) < 0) {
      _perr("sigwait");
      continue;
    }
    if (signum == SIGQUIT) {
      load_only = 1; /* flag for stp_main_loop */
      pending_interrupts ++;
    } else if (signum == SIGINT || signum == SIGHUP || signum == SIGTERM
               || signum == SIGPIPE)
    {
      pending_interrupts ++;
    }
    if (pending_interrupts > 2) {
      set_nonblocking_std_fds();
      pthread_kill (main_thread, SIGURG);
    }
    dbug(2, "sigproc %d (%s)\n", signum, strsignal(signum));
  }
  /* Notify main thread (interrupts select). */
  pthread_kill (main_thread, SIGURG);
  return NULL;
}

static void urg_proc(int signum)
{
  /* This handler is just notified from the signal_thread
     whenever an interruptable condition is detected. The
     handler itself doesn't do anything. But this will
     result select to detect an EINTR event. */
  dbug(2, "urg_proc %d (%s)\n", signum, strsignal(signum));
}

static void chld_proc(int signum)
{
  int32_t rc, btype = STP_EXIT;
  int chld_stat = 0;
  dbug(2, "chld_proc %d (%s)\n", signum, strsignal(signum));
  pid_t pid = waitpid(-1, &chld_stat, WNOHANG);
  if (pid != target_pid) {
    return;
  }

  if (chld_stat) {
    // our child exited with a non-zero status
    if (WIFSIGNALED(chld_stat)) {
      warn(_("Child process exited with signal %d (%s)\n"),
          WTERMSIG(chld_stat), strsignal(WTERMSIG(chld_stat)));
      target_pid_failed_p = 1;
    }
    if (WIFEXITED(chld_stat) && WEXITSTATUS(chld_stat)) {
      warn(_("Child process exited with status %d\n"),
          WEXITSTATUS(chld_stat));
      target_pid_failed_p = 1;
    }
  }

  rc = write(control_channel, &btype, sizeof(btype)); // send STP_EXIT
  (void) rc; /* XXX: notused */
}

#if WORKAROUND_BZ467568
/* When a SIGUSR1 signal arrives, set this variable. */
volatile sig_atomic_t usr1_interrupt = 0;

static void signal_usr1(int signum)
{
  (void) signum;
  usr1_interrupt = 1;
}
#endif	/* WORKAROUND_BZ467568 */

static void setup_main_signals(void)
{
  pthread_t tid;
  struct sigaction sa;
  sigset_t *s = malloc(sizeof(*s));
  if (!s) {
    _perr("malloc failed");
    exit(1);
  }

  /* The main thread will only handle SIGCHLD and SIGURG.
     SIGURG is send from the signal thread in case the interrupt
     flag is set. This will then interrupt any select call. */
  main_thread = pthread_self();
  sigfillset(s);
  pthread_sigmask(SIG_SETMASK, s, NULL);

  memset(&sa, 0, sizeof(sa));
  /* select will report EINTR even when SA_RESTART is set. */
  sa.sa_flags = SA_RESTART;
  sigfillset(&sa.sa_mask);

  /* Ignore all these events on the main thread. */
  sa.sa_handler = SIG_IGN;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);

  /* This is to notify when our child process (-c) ends. */
  sa.sa_handler = chld_proc;
  sigaction(SIGCHLD, &sa, NULL);

  if (monitor)
    {
      sa.sa_handler = monitor_winch;
      sigaction(SIGWINCH, &sa, NULL);
    }

  /* This signal handler is notified from the signal_thread
     whenever a interruptable event is detected. It will
     result in an EINTR event for select or sleep. */
  sa.sa_handler = urg_proc;
  sigaction(SIGURG, &sa, NULL);

  /* Everything else is handled on a special signal_thread. */
  sigemptyset(s);
  sigaddset(s, SIGINT);
  sigaddset(s, SIGTERM);
  sigaddset(s, SIGHUP);
  sigaddset(s, SIGQUIT);
  sigaddset(s, SIGPIPE);
  pthread_sigmask(SIG_SETMASK, s, NULL);
  if (pthread_create(&tid, NULL, signal_thread, s) < 0) {
    _perr(_("failed to create thread"));
    exit(1);
  }
}

/*
 * start_cmd forks the command given on the command line with the "-c"
 * option. It will wait just at the cusp of the exec until we get the
 * signal from the kernel to let it run.  We do it this way because we
 * must have the pid of the forked command so it can be set to the
 * module and made available internally as _stp_target.  PTRACE_DETACH
 * is sent from stp_main_loop() below when it receives STP_START from
 * the module.
 */
void start_cmd(void)
{
  pid_t pid;
  struct sigaction a;
#if WORKAROUND_BZ467568
  struct sigaction usr1_action, old_action;
  sigset_t blockmask, oldmask;
#endif	/* WORKAROUND_BZ467568 */

  /* if we are execing a target cmd, ignore ^C in stapio */
  /* and let the target cmd get it. */
  memset(&a, 0, sizeof(a));
  sigemptyset(&a.sa_mask);
  a.sa_flags = 0;
  a.sa_handler = SIG_IGN;
  sigaction(SIGINT, &a, NULL);

#if WORKAROUND_BZ467568
  /* Set up the mask of signals to temporarily block. */
  sigemptyset (&blockmask);
  sigaddset (&blockmask, SIGUSR1);

  /* Establish the SIGUSR1 signal handler. */
  memset(&usr1_action, 0, sizeof(usr1_action));
  sigfillset (&usr1_action.sa_mask);
  usr1_action.sa_flags = 0;
  usr1_action.sa_handler = signal_usr1;
  sigaction (SIGUSR1, &usr1_action, &old_action);

  /* Block SIGUSR1 */
  sigprocmask(SIG_BLOCK, &blockmask, &oldmask);
#endif	/* WORKAROUND_BZ467568 */

  if ((pid = fork()) < 0) {
    _perr("fork");
    exit(1);
  } else if (pid == 0) {
    /* We're in the target process.	 Let's start the execve of target_cmd, */
    int rc;
    wordexp_t words;
    char *sh_c_argv[4] = { NULL, NULL, NULL, NULL };

    a.sa_handler = SIG_DFL;
    sigaction(SIGINT, &a, NULL);

    /* Close any FDs we still hold, similarly as though this were
       a program being spawned due to an system("") tapset function. */
    closefrom(3);

    /* We could call closefrom() here, to make sure we don't leak any
     * fds to the target, but it really isn't needed here since
     * close-on-exec should catch everything. We don't have the
     * synchronizations issues here we have with system_cmd(). */

    /* Formerly, we just execl'd(sh,-c,$target_cmd).  But this does't
       work well if target_cmd is a shell builtin.  We really want to
       probe a new child process, not a mishmash of shell-interpreted
       stuff. */
    rc = wordexp (target_cmd, & words, WRDE_NOCMD|WRDE_UNDEF);
    if (rc == WRDE_BADCHAR)
      {
        /* The user must have used a shell metacharacter, thinking that
           we use system(3) to evaluate 'stap -c CMD'.  We could generate
           an error message ... but let's just do what the user meant.
           rhbz 467652. */
        sh_c_argv[0] = "sh";
        sh_c_argv[1] = "-c";
        sh_c_argv[2] = target_cmd;
        sh_c_argv[3] = NULL;

        if (read_stdin)
          {
            /* close target_cmd's stdin to prevent a data race */
            char *buf = malloc(sizeof(char) * (strlen(target_cmd) + 13));
            if (buf == NULL)
              {
                 _err (_("Failed to allocate memory.\n"));
                 _exit(1);
              }
            sprintf(buf, "%s < /dev/null", target_cmd);
            target_cmd = buf;
          }
      }
    else
      {
        switch (rc)
          {
          case 0:
            break;
          case WRDE_SYNTAX:
            _err (_("wordexp: syntax error (unmatched quotes?) in -c COMMAND\n"));
            _exit(1);
          default:
            _err (_("wordexp: parsing error (%d)\n"), rc);
            _exit (1);
          }
        if (words.we_wordc < 1) { _err ("empty -c COMMAND"); _exit (1); }
      }

/* PR 6964: when tracing all the user space process including the
   child the signal will be messed due to uprobe module or utrace
   bug. The kernel sometimes crashes.  So as an alternative
   approximation, we just wait here for a signal from the parent. */

    dbug(1, "blocking briefly\n");
    alarm(60); /* but not indefinitely */

#if WORKAROUND_BZ467568
    {
      /* Wait for the SIGUSR1 */
      while (!usr1_interrupt)
	  sigsuspend(&oldmask);

      /* Restore the old SIGUSR1 signal handler. */
      sigaction (SIGUSR1, &old_action, NULL);

      /* Restore the original signal mask */
      sigprocmask(SIG_SETMASK, &oldmask, NULL);
    }
#else  /* !WORKAROUND_BZ467568 */
    rc = ptrace (PTRACE_TRACEME, 0, 0, 0);
    if (rc < 0) perror ("ptrace me");
    raise (SIGCONT); /* Harmless; just passes control to parent. */
#endif /* !WORKAROUND_BZ467568 */

    alarm(0); /* clear alarms */
    dbug(1, "execing target_cmd %s\n", target_cmd);

    /* Note that execvp() is not a direct system call; it does a $PATH
       search in glibc.  We would like to filter out these dummy syscalls
       from the utrace events seen by scripts.

       This filtering would be done for us for free, if we used ptrace
       ...  but see PR6964.  XXX: Instead, we could open-code the
       $PATH search here; put the pause() afterward; and run a direct
       execve instead of execvp().  */

    if (execvp ((sh_c_argv[0] == NULL ? words.we_wordv[0] : sh_c_argv[0]),
                (sh_c_argv[0] == NULL ? words.we_wordv    : sh_c_argv)) < 0)
      perror(target_cmd);

      /* (There is no need to wordfree() words; they are or will be gone.) */

    _exit(1);
  } else {
    /* We're in the parent.  The child will parse target_cmd and
       execv() the result.  It will be stopped thereabouts and send us
       a SIGTRAP.  Or rather, due to PR 6964, it will stop itself and wait for
       us to release it. */
    target_pid = pid;
#if WORKAROUND_BZ467568
    /* Restore the old SIGUSR1 signal handler. */
    sigaction (SIGUSR1, &old_action, NULL);

    /* Restore the original signal mask */
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
#else  /* !WORKAROUND_BZ467568 */
    int status;
    waitpid (target_pid, &status, 0);
    dbug(1, "waited for target_cmd %s pid %d status %x\n", target_cmd, target_pid, (unsigned) status);
#endif /* !WORKAROUND_BZ467568 */
  }
}

/**
 * system_cmd() executes system commands in response
 * to an STP_SYSTEM message from the module. These
 * messages are sent by the system() systemtap function.
 */
void system_cmd(char *cmd)
{
  pid_t child_pid;

  /*
   * This needs some explanation. This function is going to fork,
   * creating a child process. That child will close fds, then fork
   * again to create a grandchild process, which execs the user's
   * command. The original child immediately exits after the 2nd fork
   * succeeds. The original parent will wait on the child to close the
   * fds and spawn the actual command.
   *
   * We're not waiting on the command to finish, we're waiting for the
   * child to close all fds (and then fork). This avoids a race if we
   * immediately get an exit after the system_cmd() and they fight
   * over who has the control channel and/or relay fds open.
   */

  dbug(2, "system %s\n", cmd);
  if ((child_pid = fork()) < 0) {	/* fork failed */
    _perr("fork");
    return;
  } else if (child_pid > 0) {		/* parent (stapio) */
     dbug(2, "waiting on %lu\n", (unsigned long)child_pid);
     (void)waitpid(child_pid, NULL, 0);
     return;
  }

  /* The child will close all fds (like the control channel and relay
   * fds), then fork/exec cmd, creating a grandchild. */
  pid_t grandchild_pid;
  closefrom(3);

  if ((grandchild_pid = fork()) < 0) {	/* fork failed */
    _perr("fork");
    _exit(1);
  } else if (grandchild_pid > 0) {	/* child  */
    dbug(2, "created %lu\n", (unsigned long)grandchild_pid);
    _exit(0);
  }

  /* The grandchild will now actually run the command. */
  if (execlp("sh", "sh", "-c", cmd, NULL) < 0)
    perr("%s", cmd);
  _exit(1);
}

/* This is only used in the old relayfs code */
static void read_buffer_info(void)
{
  char buf[PATH_MAX];
  struct statfs st;
  int fd, len, ret;

  /* NB: we don't have to worry about PR14245 on old_transport aka
     rhel4; no HAVE_OPENAT, and thus no -F fd option. */
  if (!use_old_transport)
    return;

  if (statfs("/sys/kernel/debug", &st) == 0 && (int)st.f_type == (int)DEBUGFS_MAGIC)
    return;

  if (sprintf_chk(buf, "/proc/systemtap/%s/bufsize", modname))
    return;
  fd = open(buf, O_RDONLY);
  if (fd < 0)
    return;

  len = read(fd, buf, sizeof(buf));
  if (len <= 0) {
    perr(_("Couldn't read bufsize"));
    close(fd);
    return;
  }
  ret = sscanf(buf, "%u,%u", &n_subbufs, &subbuf_size);
  if (ret != 2)
    perr(_("Couldn't read bufsize"));

  dbug(2, "n_subbufs= %u, size=%u\n", n_subbufs, subbuf_size);
  close(fd);
  return;
}

/**
 *	init_stapio - initialize the app
 *	@print_summary: boolean, print summary or not at end of run
 *
 *	Returns 0 on success, negative otherwise.
 */
int init_stapio(void)
{
  dbug(2, "init_stapio\n");

  /* create control channel */
  use_old_transport = init_ctl_channel(modname, 1);
  if (use_old_transport < 0) {
    err(_("Failed to initialize control channel.\n"));
    return -1;
  }
  read_buffer_info();

  if (attach_mod) {
    dbug(2, "Attaching\n");
    if (use_old_transport) {
      if (init_oldrelayfs() < 0) {
        close_ctl_channel();
        return -1;
      }
    } else {
      if (init_relayfs() < 0) {
        close_ctl_channel();
        return -1;
      }
    }
    return 0;
  }

  /* fork target_cmd if requested. */
  /* It will not actually exec until signalled. */
  if (target_cmd)
    start_cmd();

  if (target_namespaces_pid > 0)
    dbug(2, "target_namespaces_pid=%d\n", target_namespaces_pid);

  /* Run in background */
  if (daemon_mode) {
    pid_t pid;
    int ret;
    dbug(2, "daemonizing stapio\n");

    /* daemonize */
    ret = daemon(0, 1); /* don't close stdout at this time. */
    if (ret) {
      err(_("Failed to daemonize stapio\n"));
      return -1;
    }

    /* change error messages to syslog. */
    switch_syslog("stapio");

    /* show new pid */
    pid = getpid();
    fprintf(stdout, "%d\n", pid);
    fflush(stdout);

    /* redirect all outputs to /dev/null */
    ret = open("/dev/null", O_RDWR);
    if (ret < 0) {
      err(_("Failed to open /dev/null\n"));
      return -1;
    }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    dup2(ret, STDOUT_FILENO);
    dup2(ret, STDERR_FILENO);
    close(ret);
  }

  return 0;
}

/* cleanup_and_exit() closed channels, frees memory,
 * removes the module (if necessary) and exits. */
void cleanup_and_exit(int detach, int rc)
{
  static int exiting = 0;
  const char *staprun;
  pid_t pid;
  int rstatus;
  struct sigaction sa;

  if (monitor)
    monitor_cleanup();

  if (read_stdin)
    read_stdin_cleanup();

  if (exiting)
    return;
  exiting = 1;

  setup_main_signals();

  dbug(1, "detach=%d\n", detach);

  /* NB: We don't really need to wait for child processes.  Any that
     were started by the system() tapset function (system_cmd() above)
     can run loose. Or, a target_cmd (stap -c CMD) may have already started and
     stopped.  */

  /* OTOH, it may be still be running - but there's no need for
     us to wait for it, considering that the script must have exited
     for another reason.  So, we no longer   while(...wait()...);  here.
   */

  if (use_old_transport)
    close_oldrelayfs(detach);
  else if (pending_interrupts > 2)
    kill_relayfs();
  else
    close_relayfs();

  dbug(1, "closing control channel\n");
  close_ctl_channel();

  if (detach) {
    eprintf(_("\nDisconnecting from systemtap module.\n" "To reconnect, type \"staprun -A %s\"\n"), modname);
    my_exit(0);
  }
  else if (rename_mod)
    dbug(2, "\nRenamed module to: %s\n", modname);

  /* At this point, we're committed to calling staprun -d MODULE to
   * unload the thing and exit. */
  /* Due to PR9788, we fork and exec the setuid staprun only in a child process. */

  staprun = getenv ("SYSTEMTAP_STAPRUN") ?: BINDIR "/staprun";
  dbug(2, "removing %s\n", modname);

  // So that waitpid() below will work correctly, we need to clear
  // out our SIGCHLD handler.
  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &sa, NULL);

  pid = fork();
  if (pid < 0) {
          _perr("fork");
          my_exit(-1);
  }

  if (pid == 0) {			/* child process */
          /* Run the command. */
          char *cmd;
          int rc = asprintf(&cmd, "%s %s %s -d -C %s '%s'", staprun,
                            (verbose >= 1) ? "-v" : "",
                            (verbose >= 2) ? "-v" : "",
                            color_mode == color_always ? "always"
                              : color_mode == color_auto ? "auto" : "never",
                            modname);
          if (rc >= 1) {
                  execlp("sh", "sh", "-c", cmd, NULL);
                  /* should not return */
                  perror(staprun);
                  my_exit(-1);
          } else {
                  perror("asprintf");
                  my_exit(-1);
          }
  }

  /* parent process */
  if (waitpid(pid, &rstatus, 0) < 0) {
          _perr("waitpid");
          my_exit(-1);
  }

  if (WIFEXITED(rstatus)) {
          if(rc || target_pid_failed_p || rstatus) // if we have an error
            my_exit(1);
          else
            my_exit(0); //success
  }

  my_exit(-1);
}


/**
 *	stp_main_loop - loop forever reading data
 */

int stp_main_loop(void)
{
  ssize_t nb;
  FILE *ofp = stdout;
  struct
  {
    uint32_t type;
    union
    {
      char data[8192];
      struct _stp_msg_start start;
      struct _stp_msg_cmd cmd;
      struct _stp_msg_ns_pid nspid;
    } payload;
  } recvbuf;
  int error_detected = 0;
  int select_supported;
  int flags;
  int res;
  int rc;
  int maxfd;
  struct timeval tv;
  struct timespec ts;
  struct timespec *timeout = NULL;
  fd_set fds;


  setvbuf(ofp, (char *)NULL, _IONBF, 0);
  setup_main_signals();
  dbug(2, "in main loop\n");

  rc = send_request(STP_READY, NULL, 0);
  if (rc != 0) {
    perror ("Unable to send STP_READY");
    cleanup_and_exit(0, rc);
    /* NOTREACHED */
  }

  flags = fcntl(control_channel, F_GETFL);

  /* Make select return immediately.  We just check whether
     there is an exception available on the control_channel,
     which is how we know the module supports select. */
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  FD_ZERO(&fds);
  FD_SET(control_channel, &fds);
  res = select(control_channel + 1, NULL, NULL, &fds, &tv);
  select_supported = (res == 1 && FD_ISSET(control_channel, &fds));
  dbug(2, "select_supported: %d\n", select_supported);

  if (monitor)
      monitor_setup();

  if (read_stdin)
      read_stdin_setup();

  /* In monitor mode, we must timeout pselect to poll the monitor
     interface. In non-monitor mode, we must timeout pselect so that
     we can handle pending_interrupts. */
  ts.tv_sec = 0;
  ts.tv_nsec = 500*1000*1000;
  timeout = &ts;

  /* handle messages from control channel */
  while (1) {
    if (monitor)
      {
        monitor_input();
        monitor_render();
      }

    if (pending_interrupts) {
         int btype = STP_EXIT;
         int rc;

	 /* If 'load_only' is set, we don't want to send STP_EXIT,
	  * which would cause any 'probe end' processing to be
	  * done. Instead, we'll just detach by calling
	  * cleanup_and_exit(). This should let the module continue to
	  * run. */
	 if (load_only)
	   {
	     cleanup_and_exit(load_only /* = detach */, 0);
	     /* NOTREACHED */
	   }

         rc = write(control_channel, &btype, sizeof(btype));
         dbug(2, "signal-triggered %d exit rc %d\n", pending_interrupts, rc);
         if (monitor || (pending_interrupts > 2))
	   { /* user mashing on ^C multiple times */
	     cleanup_and_exit (load_only /* = detach */, 0);
	     /* NOTREACHED */
	   }
         else
           {} /* await STP_EXIT reply message to kill staprun */
    }

    /* If the runtime does not implement select() on the command
       filehandle, we have to poll periodically.  The polling interval can
       be relatively large, since we don't receive EAGAIN during the
       time-sensitive startup period (packets go back-to-back). */

    flags |= O_NONBLOCK;
    fcntl(control_channel, F_SETFL, flags);
    nb = read(control_channel, &recvbuf, sizeof(recvbuf));
    flags &= ~O_NONBLOCK;
    fcntl(control_channel, F_SETFL, flags);

    dbug(3, "nb=%ld\n", (long)nb);
    if (nb < (ssize_t) sizeof(recvbuf.type)) {
      if (nb >= 0 || (errno != EINTR && errno != EAGAIN)) {
        _perr(_("Unexpected EOF in read (nb=%ld)"), (long)nb);
        cleanup_and_exit(0, 1);
	/* NOTREACHED */
      }

      if (!select_supported) {
	dbug(4, "sleeping\n");
	usleep (250*1000); /* sleep 250ms between polls */
      } else {
	FD_ZERO(&fds);
	FD_SET(control_channel, &fds);
        maxfd = control_channel;
        // Immediately update screen on input
        if (monitor)
          FD_SET(STDIN_FILENO, &fds);
	res = pselect(maxfd + 1, &fds, NULL, NULL, timeout, NULL);
	if (res < 0 && errno != EINTR)
	  {
	    _perr(_("Unexpected error in select"));
	    cleanup_and_exit(0, 1);
	    /* NOTREACHED */
	  }
      }
      continue;
    }

    nb -= sizeof(recvbuf.type);
    PROBE3(staprun, recv__ctlmsg, recvbuf.type, recvbuf.payload.data, nb);

    switch (recvbuf.type) {
#if STP_TRANSPORT_VERSION == 1
    case STP_REALTIME_DATA:
      if (write_realtime_data(recvbuf.payload.data, nb)) {
        _perr(_("write error (nb=%ld)"), (long)nb);
        cleanup_and_exit(0, 1);
	/* NOTREACHED */
      }
      break;
#endif
    case STP_OOB_DATA:
      /* Note that "WARNING:" should not be translated, since it is
       * part of the module cmd protocol. */
      if (strncmp(recvbuf.payload.data, "WARNING: ", 9) == 0) {
              if (suppress_warnings) break;
              if (verbose) { /* don't eliminate duplicates */
                      if (monitor)
                              monitor_remember_output_line (recvbuf.payload.data, nb);
                      else
                              /* trim "WARNING: " */
                              warn("%.*s", (int) nb-9, recvbuf.payload.data+9);
                      break;
              } else { /* eliminate duplicates */
                      static void *seen = 0;
                      static unsigned seen_count = 0;
                      char *dupstr = strndup (recvbuf.payload.data, (int) nb);
                      char *retval;

                      if (! dupstr) {
                              /* OOM, should not happen. */
                              if (monitor)
                                      monitor_remember_output_line (recvbuf.payload.data, nb);
                              else
                                      /* trim "WARNING: " */
                                      warn("%.*s", (int) nb-9, recvbuf.payload.data+9);
                              break;
                      }

                      retval = tfind (dupstr, & seen, (int (*)(const void*, const void*))strcmp);
                      if (! retval) { /* new message */
                              if (monitor)
                                      monitor_remember_output_line (recvbuf.payload.data, nb);
                              else
                                      /* trim "WARNING: " */
                                      warn("%.*s", strlen(dupstr)-9, dupstr+9);

                              /* We set a maximum for stored warning messages,
                                 to prevent a misbehaving script/environment
                                 from emitting countless _stp_warn()s, and
                                 overflow staprun's memory. */
#define MAX_STORED_WARNINGS 1024
                              if (seen_count++ == MAX_STORED_WARNINGS) {
                                      eprintf(_("WARNING deduplication table full\n"));
                                      free (dupstr);
                              }
                              else if (seen_count > MAX_STORED_WARNINGS) {
                                      /* Be quiet in the future, but stop counting to
                                         preclude overflow. */
                                      free (dupstr);
                                      seen_count = MAX_STORED_WARNINGS+1;
                              }
                              else if (seen_count < MAX_STORED_WARNINGS) {
                                      /* NB: don't free dupstr; it's going into the tree. */
                                      retval = tsearch (dupstr, & seen,
                                                        (int (*)(const void*, const void*))strcmp);
                                      if (retval == 0) {
                                              /* OOM, should not happen */
                                              /* Next time we should get the 'full' message. */
                                              free (dupstr);
                                              seen_count = MAX_STORED_WARNINGS;
                                      }
                              }
                      } else { /* old message */
                              free (dupstr);
                      }
              } /* duplicate elimination */
      /* Note that "ERROR:" should not be translated, since it is
       * part of the module cmd protocol. */
      } else if (strncmp(recvbuf.payload.data, "ERROR: ", 7) == 0) {
              if (monitor)
                      monitor_remember_output_line (recvbuf.payload.data, nb);
              else
                      /* trim "ERROR: " */
                      err("%.*s", (int) nb-7, recvbuf.payload.data+7);
              error_detected = 1;
      } else { /* neither warning nor error */
              if (monitor)
                      monitor_remember_output_line (recvbuf.payload.data, nb);
              else
                      eprintf("%.*s", (int) nb, recvbuf.payload.data);
      }
      break;
    case STP_EXIT:
      {
        /* module asks us to unload it and exit */
        dbug(2, "got STP_EXIT\n");
        if (monitor)
                monitor_exited();
        else {
                cleanup_and_exit(0, error_detected);
		/* NOTREACHED */
	}
        /* monitor mode exit handled elsewhere, later. */
        break;
      }
    case STP_REQUEST_EXIT:
      {
        /* module asks us to start exiting, so send STP_EXIT */
        dbug(2, "got STP_REQUEST_EXIT\n");
        int32_t rc, btype = STP_EXIT;
        rc = write(control_channel, &btype, sizeof(btype));
        (void) rc; /* XXX: notused */
        break;
      }
    case STP_START:
      {
        struct _stp_msg_start *t = &recvbuf.payload.start;
        dbug(2, "systemtap_module_init() returned %d\n", t->res);
        if (t->res < 0) {
          if (target_cmd)
            kill(target_pid, SIGKILL);
          cleanup_and_exit(0, 1);
	  /* NOTREACHED */
        } else if (target_cmd) {
          dbug(1, "detaching pid %d\n", target_pid);
#if WORKAROUND_BZ467568
          /* Let's just send our pet signal to the child
             process that should be waiting for us, mid-pause(). */
          kill (target_pid, SIGUSR1);
#else
          /* Were it not for PR6964, we'd like to do it this way: */
          int rc = ptrace (PTRACE_DETACH, target_pid, 0, 0);
          if (rc < 0)
            {
              perror (_("ptrace detach"));
              if (target_cmd)
                kill(target_pid, SIGKILL);
              cleanup_and_exit(0, 1);
	      /* NOTREACHED */
            }
#endif
        }
        break;
      }
    case STP_SYSTEM:
      {
        struct _stp_msg_cmd *c = &recvbuf.payload.cmd;
        dbug(2, "STP_SYSTEM: %s\n", c->cmd);
        system_cmd(c->cmd);
        break;
      }
    case STP_NAMESPACES_PID:
      {
        struct _stp_msg_ns_pid *nspid = &recvbuf.payload.nspid;
        dbug(2, "STP_NAMESPACES_PID: %d\n", nspid->target);
        break;
      }
    case STP_TRANSPORT:
      {
        struct _stp_msg_start ts;
        struct _stp_msg_ns_pid nspid;
        if (use_old_transport) {
	  if (init_oldrelayfs() < 0) {
            cleanup_and_exit(0, 1);
	    /* NOTREACHED */
	  }
        } else {
          if (init_relayfs() < 0)
            cleanup_and_exit(0, 1);
	    /* NOTREACHED */
        }

        if (target_namespaces_pid > 0) {
          nspid.target = target_namespaces_pid;
          rc = send_request(STP_NAMESPACES_PID, &nspid, sizeof(nspid));
          if (rc != 0) {
	    perror ("Unable to send STP_NAMESPACES_PID");
	    cleanup_and_exit (1, rc);
	    /* NOTREACHED */
	  }
        }

        ts.target = target_pid;
        rc = send_request(STP_START, &ts, sizeof(ts));
	if (rc != 0) {
	  perror ("Unable to send STP_START");
	  cleanup_and_exit(0, rc);
	  /* NOTREACHED */
	}
        if (load_only) {
          cleanup_and_exit(1, 0);
	  /* NOTREACHED */
	}
        break;
      }
    default:
      warn(_("Ignored message of type %d\n"), recvbuf.type);
    }
  }
  fclose(ofp);
  return 0;
}
