/*
 *	Copyright (c) 2011 Vlad Seryakov
 *	Copyright (c) 2002-2011 Bernhard Baehr
 *
 *	Synchronize display idle time between multiple computers
 *      -------------------------------------------------------
 *
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <libgen.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>

#include <mach/mach_port.h>
#include <mach/mach_interface.h>
#include <mach/mach_init.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOMessage.h>

#define PORT			3030
#define MAX_CLIENTS		8

static bool fg = 0;
static char *server = NULL;
static int loglevel = LOG_NOTICE;
static int timeout = 180;
static CFRunLoopTimerRef timer = NULL;
static in_addr clients[MAX_CLIENTS];

static void usage()
{

}

void log(int priority, const char *msg, ...)
{
	va_list ap;

	if (priority <= loglevel) {
		va_start (ap, msg);
		if (!fg) {
			openlog("idlesync", LOG_PID, LOG_DAEMON);
			vsyslog(priority, msg, ap);
			closelog();
		} else {
			vprintf(msg, ap);
		}
		va_end(ap);
	}
}

static long int getIdleTime (void)
{
	return CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
}

static void wakeUp()
{
	log(LOG_NOTICE, "waking up display, idle %ld\n", getIdleTime());

	CGEventRef ev = CGEventCreate(NULL);
	CGPoint pos = CGEventGetLocation(ev);
	CFRelease(ev);
	ev = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(0, 0) ,kCGMouseButtonLeft);
	CGEventPost(kCGHIDEventTap, ev);
	CFRelease(ev);
	ev = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, pos ,kCGMouseButtonLeft);
	CGEventPost(kCGHIDEventTap, ev);
	CFRelease(ev);
}

int sockListen()
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        log(LOG_ERR, "socket: %s", strerror(errno));
        return -1;
    }
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    int n = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &n, sizeof(n));
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        log(LOG_ERR, "bind: %s", strerror(errno));
        return -1;
    }
    n = 1;
    ioctl(fd, FIONBIO, &n);
    return fd;
}

u_long sockAddr(const char *str)
{
   char buf[6], *ptr;
   u_long ipaddr = 0;
   int i, count, octet;

   for (i = 0;i < 4;i++) {
       ptr = buf;
       count = 0;
       *ptr = 0;
       while (*str != '.' && *str && count < 4) {
             if (!isdigit(*str)) {
                 // Last octet may be terminated with non-digit
                 if (i == 3) {
                     break;
                 }
                 return 0;
             }
             *ptr++ = *str++;
             count++;
       }
      if (count >= 4 || count == 0) {
          return 0;
      }
      *ptr = 0;
      octet = atoi(buf);
      if (octet < 0 || octet > 255) {
          return 0;
      }
      str++;
      ipaddr = ipaddr << 8 | octet;
   }
   return htonl(ipaddr);
}

void sockSend(char *server, long int idle)
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		log(LOG_ERR, "socket: %s", strerror(errno));
		return;
	}
	struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = sockAddr(server);
	sendto(sock, &idle, sizeof(idle), 0, (sockaddr*)&sa, (socklen_t)sizeof(sa));
	close(sock);

	log(LOG_INFO, "sending to %s idle %ld\n", server, idle);
}

// mode: 0 - read, 1 - write
int sockSelect(int fd, int mode, int timeout)
{
    int n;
    struct pollfd pfd;

    if (timeout < 0) {
        return 0;
    }
    timeout *= 1000;
    pfd.fd = fd;
    switch (mode) {
    case 0:
        pfd.events = POLLIN;
        break;
    case 1:
        pfd.events = POLLOUT;
        break;
    case 2:
        pfd.events = POLLPRI;
        break;
    default:
        return -1;
        break;
    }
    pfd.revents = 0;
    do {
        n = poll(&pfd, 1, timeout);
    } while (n < 0 && errno == EINTR);
    if (n > 0) {
        return 1;
    }
    return 0;
}

static void sockCallback(CFSocketRef s, CFSocketCallBackType type, CFDataRef address, const void *arg, void *context)
{
	long int data;
	struct sockaddr_storage addr;
	sockaddr_in *in = (sockaddr_in*)&addr;

	int sock = CFSocketGetNative(s);
	socklen_t addrlen = sizeof(addr);
	ssize_t bytesRead = recvfrom(sock, &data, sizeof(data), 0, (struct sockaddr *) &addr, &addrlen);
	if (bytesRead <= 0) {
		return;
	}
	long int idle = getIdleTime();

	log(LOG_INFO, "received %ld from %s idle %ld\n", data, inet_ntoa(in->sin_addr), idle);

	if (server) {
		if (idle > timeout && data < idle) {
			wakeUp();
		}
	} else {
		sockSend(inet_ntoa(in->sin_addr), idle);

		// Add client to the list
		int i, empty = -1;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].s_addr == in->sin_addr.s_addr) {
				break;
			}
			if (clients[i].s_addr == 0) {
				empty = i;
			}
		}
		if (i == MAX_CLIENTS && empty != -1) {
			clients[empty] = in->sin_addr;
			log(LOG_NOTICE, "added new client %d from %s\n", empty, inet_ntoa(in->sin_addr));
		}
	}
}

void sockBroadcast(long int idle)
{
	for (int i = 0;i < MAX_CLIENTS; i++) {
		if (clients[i].s_addr != 0) {
			sockSend(inet_ntoa(clients[i]), idle);
		}
	}
}

void sockPing(long int idle)
{
	sockSend(server, idle);
}

void sockSetup()
{
	int fd = sockListen();
	if (fd < 0) {
		exit(1);
	}
	CFSocketRef sock = CFSocketCreateWithNative(kCFAllocatorDefault, fd, kCFSocketReadCallBack, sockCallback, NULL);
	CFRunLoopSourceRef src = CFSocketCreateRunLoopSource(kCFAllocatorDefault, sock, 0);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
	CFRelease(src);
	log(LOG_NOTICE, "listening on port %d socket %d server %s\n", PORT, fd, server);
}


static void idleCallback (CFRunLoopTimerRef timer, void *info)
{
	sockPing(getIdleTime());
}

static void timerSetup(void)
{
	// Only client sends pings
	if (!server) {
		return;
	}
	sockPing(getIdleTime());
	timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + timeout, timeout, 0, 0, idleCallback, NULL);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
	log(LOG_NOTICE, "timer interval %d\n", timeout);
}

void displayCallback (void *context, io_service_t y, natural_t msgType, void *msgArgument)
{
	long int idle = getIdleTime();

	switch (msgType) {
	case kIOMessageDeviceWillPowerOff:
	case kIOMessageDeviceHasPoweredOn:
		log(LOG_NOTICE, "display %s idle %ld\n", msgType == kIOMessageDeviceWillPowerOff ? "power off" : "power on", idle);
		if (server) {
			sockPing(idle);
		} else {
			sockBroadcast(msgType == kIOMessageDeviceWillPowerOff ? idle : 0);
		}
		break;
	}
}

static void displaySetup(void)
{
	io_service_t displayWrangler;
	IONotificationPortRef notificationPort;
	io_object_t notifier;

	displayWrangler = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceNameMatching("IODisplayWrangler"));
	if (!displayWrangler) {
		log(LOG_ERR, "IOServiceGetMatchingService failed\n");
		exit (1);
	}
	notificationPort = IONotificationPortCreate(kIOMasterPortDefault);
	if (!notificationPort) {
		log(LOG_ERR, "IONotificationPortCreate failed\n");
		exit (1);
	}
	if (IOServiceAddInterestNotification(notificationPort, displayWrangler, kIOGeneralInterest,
		displayCallback, NULL, &notifier) != kIOReturnSuccess) {
		log(LOG_ERR, "IOServiceAddInterestNotification failed\n");
		exit (1);
	}
	CFRunLoopAddSource (CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(notificationPort), kCFRunLoopDefaultMode);
	IOObjectRelease(displayWrangler);
}

int main (int argc, char * const *argv)
{
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-h")) {
			usage();
			exit(0);
		} else

		if (!strcmp(argv[i], "-f")) {
			fg = true;
		} else

		if (!strcmp(argv[i], "-v")) {
			loglevel = LOG_DEBUG;
		} else

		if (!strcmp(argv[i], "-g")) {
			printf("%ld\n", getIdleTime());
			exit(0);
		} else

		if (!strcmp(argv[i], "-t")) {
			if (++i < argc) {
				timeout = atoi(argv[i]);
 			}
		} else

		if (!strcmp(argv[i], "-s")) {
			if (++i < argc) {
				server = argv[i];
			}
		}
	}

	if (!fg && daemon(0, 0)) {
		log(LOG_ERR, "daemonizing failed: %d\n", errno);
		return (1);
	}

	sockSetup();
	timerSetup();
	displaySetup();

	CFRunLoopRun ();
	return (0);
}
