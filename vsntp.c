/* SNTP for Virtual PC */
/*
   Copyright (C) 2003-2007 imacat.
   
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Filename:	vsntp.c
   Description:	SNTP for Virtual PC
   Author:	imacat <imacat@mail.imacat.idv.tw>
   Date:	2003-12-22
   Copyright:	(c) 2003-2007 imacat */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Headers */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <sys/socket.h> 
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
extern int h_errno;

/* Configuration */
#define AUTHOR "imacat"
#define AUTHORMAIL "imacat@mail.imacat.idv.tw"
#define EXEC_USER "root"
#define PIDDIR "/var/run"
#define DEFAULT_INTERVAL 900
#define MAX_NETWORK_ERROR 1200
#define SNTP_PORT 123
#define SCHEDULER_SLEEP 1
#define SCHEDULER_ALARM 2
#define DEFAULT_SCHEDULER SCHEDULER_SLEEP

#define VERSTR "\
%s v%s, Copyright (C) 2003-2007 %s\n\
This is free software; see the source for copying conditions.  There is NO\n\
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n\
\n"

#define LONGHELP "\
Usage: %s [-i n] [-p file] [-a|-s] server\n\
Synchronize the system time against a time server periodically.  It\n\
uses RFC 1361 SNTP service UDP port 123 to synchronize the time.  You\n\
may need to be root to synchronize the system time.\n\
\n\
  -i,--interval n    Set the synchronization interval to n seconds. (%d)\n\
  -p,--pidfile file  The PID file location. (%s)\n\
  -s,--sleep         Use sleep() to schedule synchronization.  (default)\n\
  -a,--alarm         Use alarm() to schedule synchronization.\n\
  -h,--help          Display this help.\n\
  -v,--version       Display version information.\n\
  server             Time server to synchronize against.\n\
\n"

#define EXIT_OK 0
#define EXIT_ARGERR 1
#define EXIT_NETERR 2
#define EXIT_SYSERR 127

#define EPOCH_DIFF ((unsigned long) 86400 * (365 * 70 + 17))

/* Prototypes */
void set_this_file (char *argv0);
void parse_args (int argc, char *argv[]);
void check_priv (void);
void xexit (int status)
    __attribute__((noreturn));
void verror (int status, char *message, va_list ap)
    __attribute__((noreturn));
void error (int status, char *message, ...)
    __attribute__((noreturn, format(printf, 2, 3)));
void vwarn (char *message, va_list ap);
void warn (char *message, ...)
    __attribute__((format(printf, 1, 2)));
void neterror (int firstsync, time_t errstart, char *message, ...)
    __attribute__((format(printf, 3, 4)));
void sigterm_exit (int signum)
    __attribute__((noreturn));
void daemonize (void);
void makepid (void);
void setsigterm (void);
#ifdef HAVE_ALARM
void setsigalrm (void);
void alarm_wakeup (int signum);
#endif
double synctime (void);

void fork2background (void);
void closeall (void);
unsigned long fromnetnum (const char *oct);
const char *tonetnum (unsigned long num);
unsigned long usec2frac (long usec);
long frac2usec (unsigned long frac);

void *xmalloc (size_t size);
char *xstrcpy (const char *src);
char *xstrcat (int n, ...);
time_t xtime (time_t *t);
void xgettimeofday (struct timeval *tv, struct timezone *tz);
void xsettimeofday (const struct timeval *tv , const struct timezone *tz);
void xchdir (const char *path);
pid_t xfork (void);
pid_t xsetsid (void);
long xsysconf (int name, const char *confname);
FILE *xfopen (const char *path, const char *mode);
void xfclose (FILE *stream);
int xfprintf (FILE *stream, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void xsigemptyset (sigset_t *set);
void xsigaction (int signum, const struct sigaction *act, struct sigaction *oldact); 

/* Variables */
char *server_name = NULL, *this_file = NULL, *pidfile = NULL;
int interval = -1, daemonized = 0, scheduler = -1;
struct sockaddr_in server;
time_t next;

/* Main program */
int
main (int argc, char *argv[])
{
  double toff;
  
  /* Find this file's name */
  set_this_file (argv[0]);
  parse_args (argc, argv);
  
  /* Synchronize for the first time, to ensure no obvious network errors */
  toff = synctime ();
  /* Record the first synchronization */
  if (toff != 0)
    syslog (LOG_INFO, "Time adjusted %.6f seconds.", toff);
  
  /* Daemonize */
  daemonize ();
  /* Make the PID file */
  makepid ();
  /* Set the way we exit */
  setsigterm ();
  
  switch (scheduler)
   {
    /* Use sleep() to schedule synchronization */
    case SCHEDULER_SLEEP:
      syslog (LOG_INFO, "Using sleep() to schedule synchronization");
      /* Enter an endless loop of synchronization */
      while (1) {
        /* Synchronize the time */
        toff = synctime ();
        /* Record the synchronization */
        if (toff != 0)
          syslog (LOG_DEBUG, "Time adjusted %.6f seconds.", toff);
        
        /* Wait until the next time */
        sleep (interval);
      }
      break;
    
#ifdef HAVE_ALARM
    /* Use alarm() to schedule synchronization */
    case SCHEDULER_ALARM:
      /* Set the the synchronization scheduler alerm */
      setsigalrm ();
      /* syslog() must follow setaction(), or it crash after the first alarm.
         To be investigated. */
      syslog (LOG_INFO, "Using alarm() to schedule synchronization");
      /* Schedule the next alarm */
      alarm (interval);
      /* Enter an endless loop of synchronization
         pause() is interrupted by every SIGALRM signal.
         Endless loop of pause() make SIGALRM signals endlessly. */
      while (1)
        pause ();
      break;
#endif
   }
  
  return 0;
}

/* set_this_file: Get the name of this file
     return: none. */
void
set_this_file (char *argv0)
{
  char *p;
  p = rindex (argv0, '/');
  if (p == NULL)
    this_file = xstrcpy (argv0);
  else
    this_file = xstrcpy (++p);
  return;
}

/* parse_args: Parse the arguments.
     return: none. */
void
parse_args (int argc, char *argv[])
{
  static struct option longopts[] = {
    {"interval", 1, NULL, 'i'},
    {"pidfile", 1, NULL, 'p'},
    {"sleep", 0, NULL, 's'},
    {"alarm", 0, NULL, 'a'},
    {"help", 0, NULL, 'h'},
    {"version", 0, NULL, 'v'},
    {0, 0, 0, 0}
  };
  int i, r, c, c0, longindex = 0;
  struct in_addr server_addr;
  struct hostent *server_hostent;
  void *p;
  size_t len;
  
  /* Set the default value */
  interval = DEFAULT_INTERVAL;
  scheduler = DEFAULT_SCHEDULER;
  pidfile = xstrcat (4, PIDDIR, "/", this_file, ".pid");
  
  while (1)
   {
    c = getopt_long (argc, argv, "i:p:sahv", longopts, &longindex);
    if (c == -1)
      break;
    
    switch (c)
     {
      case 'i':
        r = sscanf (optarg, "%5d%c", &interval, &c0);
        if (r != 1 || interval <= 0)
            error (EXIT_ARGERR, "invalid interval: %s.", optarg);
        break;
      
      case 'p':
        if (pidfile != NULL)
          free (pidfile);
      	pidfile = xstrcpy (optarg);
      	break;
      
      case 's':
        scheduler = SCHEDULER_SLEEP;
        break;
      
      case 'a':
        scheduler = SCHEDULER_ALARM;
        break;
      
      case 'h':
        printf (LONGHELP, this_file, interval, pidfile);
        exit (EXIT_OK);
      
      case 'v':
        printf (VERSTR, this_file, VERSION, AUTHOR);
        exit (EXIT_OK);
     
      default:
        exit (EXIT_ARGERR);
     }
   }
  
  /* Process each argument */
  for (i = optind; i < argc; i++)
    switch (i - optind)
     {
      case 0:
        server_name = xstrcpy (argv[i]);
        break;
      
      default:
        error (EXIT_ARGERR, "Too many argument: %s.", argv[i]);
     }
  
  /* Process the interval */
  if (interval == -1)
    interval = DEFAULT_INTERVAL;
  
#ifndef HAVE_ALARM
  if (scheduler == SCHEDULER_ALARM)
    error (EXIT_ARGERR, "alarm() is not supported on this platform.");
#endif
  
  /* Process the PID file */
  /* Compose the default PID file path */
  if (pidfile == NULL)
    pidfile = xstrcat (4, PIDDIR, "/", this_file, ".pid");
  
  /* Process the time server */
  if (server_name == NULL)
    error (EXIT_ARGERR, "Please specify the time server.");
  r = inet_aton (server_name, &server_addr);
  if (r == 0)
   {
    /* Try DNS look up */
    server_hostent = gethostbyname (server_name);
    if (server_hostent == NULL)
      error (EXIT_ARGERR, "%s: %s.", server_name, hstrerror (h_errno));
    /* Obtain the IP number, reverse the byte order */
    server_addr.s_addr =
        ((server_hostent->h_addr_list[0][3] << 24) & 0xFF000000) |
        ((server_hostent->h_addr_list[0][2] << 16) & 0x00FF0000) |
        ((server_hostent->h_addr_list[0][1] << 8)  & 0x0000FF00) |
        (server_hostent->h_addr_list[0][0]         & 0x000000FF);
    /* Modify the server name for logging */
    len = strlen (server_name) + 20;
    p = (void *) server_name;
    server_name = (char *) xmalloc (len);
    snprintf (server_name, len, "%s (%hhu.%hhu.%hhu.%hhu)", (char *) p,
        server_hostent->h_addr_list[0][0],
        server_hostent->h_addr_list[0][1],
        server_hostent->h_addr_list[0][2],
        server_hostent->h_addr_list[0][3]);
    free (p);
   }
  /* Save the server infomation */
  server.sin_family = AF_INET;
  server.sin_addr = server_addr;
  server.sin_port = htons (SNTP_PORT);
  
  return;
}

/* xexit: Properly handle the exit.
     return: none. */
void
xexit (int status)
{
  int r;
  
  /* Proper exit in daemon mode */
  if (daemonized)
   {
    /* Remove the PID file */
    r = unlink (pidfile);
    if (r == -1)
      syslog (LOG_ERR, "%s:%d: unlink %s: %s",
        __FILE__, __LINE__, pidfile, strerror (errno));
    /* Close the syslog */
    closelog ();
   }
  
  exit (status);
}

/* verror: Issue an error with variable argument list */
void
verror (int status, char *message, va_list ap)
{
  /* Issue the error message */
  if (daemonized)
   {
    vsyslog (LOG_ERR, message, ap);
    syslog (LOG_ERR, "Exited upon unrecoverable network error.");
   }
  else
   {
    vfprintf (stderr, message, ap);
    fprintf (stderr, "\n");
   }
  
  xexit (status);
}

/* error: Issue an error */
void
error (int status, char *message, ...)
{
  va_list ap;
  
  /* Handle the error with verror */
  va_start (ap, message);
  verror (status, message, ap);
  va_end (ap);
  
  /* No return */
}

/* vwarn: Issue a warning with variable argument list */
void
vwarn (char *message, va_list ap)
{
  /* Issue the warning message */
  if (daemonized)
    vsyslog (LOG_WARNING, message, ap); 
  else
    vfprintf (stderr, message, ap);
  
  return;
}

/* warn: Issue a warning */
void
warn (char *message, ...)
{
  va_list ap;
  
  /* Handle warnings with vwarn */
  va_start (ap, message);
  vwarn (message, ap);
  va_end (ap);
  
  return;
}

/* neterror: Issue a network error */
void
neterror (int firstsync, time_t errstart, char *message, ...)
{
  va_list ap;
  time_t now;
  
  va_start (ap, message);
  
  /* Don't pass it if we can't even synchronize the first time */
  if (firstsync)
    verror (EXIT_NETERR, message, ap);
  
  /* Warn it */
  vwarn (message, ap);
  /* Errors lasted for too long */
  now = xtime (NULL);
  if (now - errstart > MAX_NETWORK_ERROR)
    error (EXIT_NETERR, "Exited upon network error exceeding %d seconds.", MAX_NETWORK_ERROR);
  
  return;
}

/* sigterm: End the program */
void
sigterm_exit (int signum)
{
  /* Log the exit */
  syslog (LOG_INFO, "Exited upon TERM signal.");
  /* Exit normally */
  xexit (EXIT_OK);
}

/* daemonize: Daemonize the process */
/* http://www.erlenstar.demon.co.uk/unix/faq_2.html#SEC16 */
void
daemonize (void)
{
  /* Fork to be a new process group leader */
  fork2background ();
  /* Become a new session group leader, to get rid of the
     controlling terminal */
  xsetsid ();
  /* Fork again to get rid of this new session */
  fork2background ();
  /* chdir to "/", to avoid staying on any mounted file system */
  xchdir ("/");
  /* Avoid inheriting umasks */
  umask (0);
  /* Close all opened file descriptors */
  closeall ();

  /* Set the flag */
  daemonized = 1;
  
  /* Start the syslog */
  openlog (this_file, LOG_PID, LOG_DAEMON);
  /* Log the start */
  syslog (LOG_INFO, "Start synchronization with %s.", server_name);
  
  return;
}

/* makepid: Make the PID file */
void
makepid (void)
{
  pid_t pid;
  FILE *fp;
  
  /* Record the PID */
  pid = getpid ();
  
  /* Save to the file */
  fp = xfopen (pidfile, "w");
  xfprintf (fp, "%d\n", pid);
  xfclose (fp);
  
  return;
}

/* setsigterm: Configure the way we exit */
void
setsigterm (void)
{
  struct sigaction action;
  /* Set the TERM signal handler */
  action.sa_handler = sigterm_exit;
  /* Block further signals */
  xsigemptyset (&action.sa_mask);
  /* Set the TERM signal handler */
  xsigaction (SIGTERM, &action, NULL);
  return;
}

#ifdef HAVE_ALARM
/* setsigalrm: Configure the synchronization scheduler alerm */
void
setsigalrm (void)
{
  struct sigaction action;
  /* Set the ALRM signal handler */
  action.sa_handler = alarm_wakeup;
  /* Block further signals */
  xsigemptyset (&action.sa_mask);
  /* Set the TERM signal handler */
  xsigaction (SIGALRM, &action, NULL);
  return;
}

/* alarm_wakeup: Wake up on alarm and synchronize the time */
void
alarm_wakeup (int signum)
{
  double toff;
  
  /* Synchronize the time */
  toff = synctime ();
  /* Record the synchronization */
  if (toff != 0)
    syslog (LOG_DEBUG, "Time adjusted %.6f seconds.", toff);
  
  /* Schedule the next alarm */
  alarm (interval);
  return;
}
#endif

/* synctime: Synchronize the time.  See RFC 1361.
     return: Time offset that is synchronized */
double
synctime (void)
{
  int i, r, udp;
  static int firstsync = 1;
  char buf[61];
  ssize_t len;
  static time_t errstart = 0;
  static struct timeval tv1, tv2, tv3, tv4, tvnew;
  struct timezone tz;
  double t1, t2, t3, t4, toff, tnew;
  time_t now;
  
  /* Create the UDP socket */
  syslog (LOG_DEBUG, "%s:%d: Connecting UDP to %s",
      __FILE__, __LINE__, server_name);
  udp = socket (PF_INET, SOCK_DGRAM, 0);
  /* Initialize the connection */
  r = connect (udp, (struct sockaddr*) &server, sizeof (server));
  if (r == -1)
   {
    now = xtime (NULL);
    /* First error */
    if (errstart == 0)
      errstart = now;
    neterror (firstsync, errstart, "%s:%d: connect %s (%d): %s",
      __FILE__, __LINE__, server_name, now - errstart, strerror (errno));
    /* Close the opened socket */
    r = close (udp);
    if (r != 0)
      warn ("%s:%d: close udp: %s",
        __FILE__, __LINE__, strerror (errno));
    return 0;
   }
  
  /* Send to the server */
  /* Pad zeroes */
  for (i = 0; i < 61; i++)
    buf[i] = 0;
  /* 00 001 011 - leap, ntp ver, client.  See RFC 1361. */
  buf[0] = (0 << 6) | (1 << 3) | 3;
  /* Get the local sent time - Originate Timestamp */
  xgettimeofday (&tv1, &tz);
  t1 = (double) tv1.tv_sec + (double) tv1.tv_usec / 1000000;
  /* Send to the server */
  memcpy (&buf[40], tonetnum ((unsigned long) tv1.tv_sec + EPOCH_DIFF), 4);
  memcpy (&buf[44], tonetnum (usec2frac (tv1.tv_usec)), 4);
  syslog (LOG_DEBUG, "%s:%d: Sending UDP buf to %s",
      __FILE__, __LINE__, server_name);
  len = send (udp, buf, 48, 0);
  if (len == -1)
   {
    now = xtime (NULL);
    /* First error */
    if (errstart == 0)
      errstart = now;
    neterror (firstsync, errstart, "%s:%d: send %s (%d): %s",
      __FILE__, __LINE__, server_name, now - errstart, strerror (errno));
    /* Close the opened socket */
    r = close (udp);
    if (r != 0)
      warn ("%s:%d: close udp: %s",
        __FILE__, __LINE__, strerror (errno));
    return 0;
   }
  
  /* Read from the server */
  syslog (LOG_DEBUG, "%s:%d: Reading UDP buf from %s",
      __FILE__, __LINE__, server_name);
  len = recv (udp, &buf, 60, 0);
  if (len == -1)
   {
    now = xtime (NULL);
    /* First error */
    if (errstart == 0)
      errstart = now;
    neterror (firstsync, errstart, "%s:%d: recv %s (%d): %s",
      __FILE__, __LINE__, server_name, now - errstart, strerror (errno));
    /* Close the opened socket */
    r = close (udp);
    if (r != 0)
      warn ("%s:%d: close udp: %s",
        __FILE__, __LINE__, strerror (errno));
    return 0;
   }
  
  /* Close the socket */
  r = close (udp);
  if (r != 0)
    warn ("%s:%d: close udp: %s",
      __FILE__, __LINE__, strerror (errno));
  
  /* Get the local received time */
  xgettimeofday (&tv4, &tz);
  t4 = (double) tv4.tv_sec + (double) tv4.tv_usec / 1000000;
  
  /* Calculate the local time offset */
  /* Get the remote Receive Timestamp */
  tv2.tv_sec = fromnetnum (&buf[32]) - EPOCH_DIFF;
  tv2.tv_usec = frac2usec (fromnetnum (&buf[36]));
  t2 = (double) tv2.tv_sec + (double) tv2.tv_usec / 1000000;
  /* Get the remote Transmit Timestamp */
  tv3.tv_sec = fromnetnum (&buf[40]) - EPOCH_DIFF;
  tv3.tv_usec = frac2usec (fromnetnum (&buf[44]));
  t3 = (double) tv3.tv_sec + (double) tv3.tv_usec / 1000000;
  /* The time offset */
  toff = (t2 + t3 - t1 - t4) / 2;
  syslog (LOG_DEBUG, "%s:%d: Local time offset: t1: %.6f, t2: %.6f, t3: %.6f, t4: %.6f, toff: %.6f",
      __FILE__, __LINE__, t1, t2, t3, t4, toff);
  /* Calculate the new time */
  tnew = t4 + toff;
  tvnew.tv_usec = (long long) (tnew * 1000000) % 1000000;
  tvnew.tv_sec = ((long long) (tnew * 1000000) - tvnew.tv_usec) / 1000000;
  
  /* Set the time */
  xsettimeofday (&tvnew, &tz);
  
  /* Re-initialize the error timer */
  errstart = 0;
  /* Remove the first time flag */
  firstsync = 0;
  
  return toff;
}

/* fork2background: Fork to the background.
     return: none. */
void
fork2background (void)
{
  pid_t pid;
  /* Fork */
  pid = xfork ();
  /* Exit the parent process */
  if (pid != 0)
    exit (0);
  return;
}

/* closeall: Close all the opened file descriptors,
             especially: stdin, stdout and stderr.
     return: none. */
void
closeall (void)
{
  int i;
  long openmax;
  openmax = xsysconf (_SC_OPEN_MAX, "_SC_OPEN_MAX");
  for (i = 0; i < openmax; i++)
    close (i);
  return;
}

/* fromnetnum: Convert from a network number to a C number.
     return: the number in unsigned long. */
unsigned long
fromnetnum (const char *oct)
{
  return ((unsigned char) oct[0] << 24 | (unsigned char) oct[1] << 16 | (unsigned char) oct[2] << 8 | (unsigned char) oct[3]);
}

/* tonetnum: Convert from a C number to a network number.
     return: the number in network octet.  */
const char *tonetnum (unsigned long num)
{
  static char oct[5] = "0000";
  oct[0] = (num >> 24) & 255;
  oct[1] = (num >> 16) & 255;
  oct[2] = (num >> 8) & 255;
  oct[3] = num & 255;
  return oct;
}

/* usec2frac: Convert from microsecond to fraction of a second.
     return: Fraction of a second. */
unsigned long
usec2frac (long usec)
{
  return (unsigned long) (((long long) usec << 32) / 1000000);
}

/* usec2frac: Convert from fraction of a second to microsecond
     return: microsecond. */
long
frac2usec (unsigned long frac)
{
  return (long) (((long long) frac * 1000000) >> 32);
}

/* xstrcpy: allocate enough memory, make a copy of the string, and handle its error.
     return: pointer to the destination string. */
char *
xstrcpy (const char *src)
{
  char *dest;
  dest = (char *) xmalloc (strlen (src) + 1);
  strcpy (dest, src);
  return dest;
}

/* xstrcat: allocate enough memory, concatenate the strings, and handle its error.
     return: pointer to the destination string. */
char *
xstrcat (int n, ...)
{
  int i;
  size_t len;
  va_list ap;
  char *s;
  
  /* Calculate the result size */
  va_start (ap, n);
  for (i = 0, len = 0; i < n; i++)
    len += strlen (va_arg (ap, char *));
  va_end (ap);
  
  /* Allocate the memory */
  s = (char *) xmalloc (len + 1);
  
  /* Concatenate the strings */
  va_start (ap, n);
  strcpy (s, va_arg (ap, char *));
  for (i = 1; i < n; i++)
    strcat (s, va_arg (ap, char *));
  va_end (ap);
  
  return s;
}

/* xmalloc: malloc() and handle its error.
     return: pointer to the allocated memory block. */
void *
xmalloc (size_t size)
{
  void *ptr;
  ptr = malloc (size);
  if (ptr == NULL)
    error (EXIT_SYSERR, "%s:%d: malloc: %s",
      __FILE__, __LINE__, strerror (errno));
  return ptr;
}

/* xtime: time() and handle its error.
     return: current time. */
time_t
xtime (time_t *t)
{
  time_t r;
  r = time (t);
  if (r == -1)
    error (EXIT_SYSERR, "%s:%d: time: %s",
      __FILE__, __LINE__, strerror (errno));
  return r;
}

/* xgettimeofday: gettimeofday() and handle its error.
     return: none. */
void
xgettimeofday (struct timeval *tv, struct timezone *tz)
{
  int r;
  r = gettimeofday (tv, tz);
  if (r == -1)
    error (EXIT_SYSERR, "%s:%d: gettimeofday: %s",
      __FILE__, __LINE__, strerror (errno));
  return;
}

/* xsettimeofday: settimeofday() and handle its error.
     return: none. */
void
xsettimeofday (const struct timeval *tv , const struct timezone *tz)
{
  int r;
  r = settimeofday (tv, tz);
  if (r == -1)
    error (EXIT_SYSERR, "%s:%d: settimeofday: %s",
      __FILE__, __LINE__, strerror (errno));
  return;
}

/* xchdir: chdir() and handle its error.
     return: none. */
void
xchdir (const char *path)
{
  int r;
  r = chdir (path);
  if (r == -1)
    error (EXIT_SYSERR, "%s:%d: chdir %s: %s",
      __FILE__, __LINE__, path, strerror (errno));
  return;
}

/* xfork: fork() and handle its error.
     return: none. */
pid_t
xfork (void)
{
  pid_t pid;
  pid = fork ();
  if (pid == -1)
    error (EXIT_SYSERR, "%s:%d: fork: %s",
      __FILE__, __LINE__, strerror (errno));
  return pid;
}

/* xsetsid: setsid() and handle its error.
     return: none. */
pid_t
xsetsid (void)
{
  pid_t pid;
  pid = setsid ();
  if (pid == -1)
    error (EXIT_SYSERR, "%s:%d: setsid: %s",
      __FILE__, __LINE__, strerror (errno));
  return pid;
}

/* xsysconf: sysconf() and handle its error.
     return: none. */
long
xsysconf (int name, const char *confname)
{
  long r;
  r = sysconf (name);
  if (r == -1)
    error (EXIT_SYSERR, "%s:%d: sysconf %s: %s",
      __FILE__, __LINE__, confname, strerror (errno));
  return r;
}

/* xfopen: fopen() and handle its error.
     return: file handler pointer. */
FILE *
xfopen (const char *path, const char *mode)
{
  FILE *fp;
  fp = fopen (path, mode);
  if (fp == NULL)
    error (EXIT_SYSERR, "%s:%d: fopen %s: %s",
      __FILE__, __LINE__, path, strerror (errno));
  return fp;
}

/* xfclose: fclose() and handle its error.
     return: none. */
void
xfclose (FILE *stream)
{
  int r;
  r = fclose (stream);
  if (r == EOF)
    error (EXIT_SYSERR, "%s:%d: fclose: %s",
      __FILE__, __LINE__, strerror (errno));
  return;
}

/* xfprintf: fprintf() and handle its error.
     return: length print so far. */
int
xfprintf (FILE *stream, const char *format, ...)
{
  int len;
  va_list ap;
  va_start (ap, format);
  len = vfprintf (stream, format, ap);
  va_end (ap);
  if (len < 0)
    error (EXIT_SYSERR, "%s:%d: vfprintf: %s",
      __FILE__, __LINE__, strerror (errno));
  return len;
}

/* xsigemptyset: sigemptyset() and handle its error.
     return: none. */
void
xsigemptyset (sigset_t *set)
{
  int r;
  r = sigemptyset (set);
  if (r == -1)
    error (EXIT_SYSERR, "%s:%d: sigemptyset: %s",
      __FILE__, __LINE__, strerror (errno));
  return;
}

/* xsigaction: sigaction() and handle its error.
     return: none. */
void
xsigaction (int signum, const struct sigaction *act, struct sigaction *oldact)
{
  int r;
  r = sigaction (signum, act, oldact);
  if (r == -1)
    error (EXIT_SYSERR, "%s:%d: sigaction: %s",
      __FILE__, __LINE__, strerror (errno));
  return;
}
