/*
 * andromedad: open the Camera app when the camera slider of the Xiaomi
 * Mi Mix 3 5G (andromeda) is pushed open, as the phone's Android does.
 *
 *   andromedad            daemon (launchd job org.andromeda.andromedad)
 *   andromedad --status   print the slider value once
 *   andromedad --open     open the Camera app once (tests the launch path)
 *
 * Kernel side: patch 0097 (xnu/andromeda_slider.c), booted with
 * andrslider=1 andrslidercam=1 (or 2 for the other polarity). Without them
 * the sysctl answers ENOENT and the daemon exits 0, so launchd (KeepAlive
 * SuccessfulExit=false) leaves it stopped until the next boot.
 *
 * No polling: the main thread sleeps in the sysctl andromeda.slider_wait
 * until the kernel publishes a new reading, or 60 s pass. The kernel counts
 * closed -> open transitions ("opens", andromeda_slider.h); a new opening
 * while the slider is still open launches the camera, at most once per
 * MIN_GAP_S. Log lines go to /dev/console, so they reach the persistent log
 * and the USB live log ("andromedad: ...").
 *
 * Opening an app: FrontBoardServices calls, options and entitlements as in
 * ios-flame's phone/tools/openapp.m and openapp-ents.plist (Nick Wylynko,
 * MIT, commit 745c4341; licenses/ios-flame-MIT.txt, CREDITS.md), with
 * LSApplicationWorkspace as a fallback added here.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "andromeda_slider.h"

#define CAMERA_ID   "com.apple.camera"
#define MIN_GAP_S   3       /* one launch per 3 s at most */
#define ANSWER_S    15      /* wait for FrontBoard's answer */
#define POLL_US     500000  /* only if slider_wait is unusable */

#define FBS_PATH "/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices"
#define MCS_PATH "/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices"

/* Private classes, looked up at run time (no link-time dependency). */
@interface FBSOpenApplicationOptions : NSObject
+ (instancetype)optionsWithDictionary:(NSDictionary *)dictionary;
@end

@interface FBSOpenApplicationService : NSObject
+ (instancetype)serviceWithDefaultShellEndpoint;
- (void)openApplication:(NSString *)bundleID withOptions:(FBSOpenApplicationOptions *)options
             completion:(void (^)(id process, NSError *error))completion;
@end

@interface FBSSystemService : NSObject
+ (instancetype)sharedService;
- (void)openApplication:(NSString *)bundleID options:(NSDictionary *)options
             withResult:(void (^)(NSError *error))result;
@end

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)openApplicationWithBundleID:(NSString *)bundleID;
@end

static int console_fd = -1;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* One write per line: to the kernel console, and to a terminal if any. */
static void
say(const char *fmt, ...)
{
	char line[512];
	int n = snprintf(line, sizeof(line), "andromedad: ");
	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (m < 0) {
		m = 0;
	}
	size_t len = (size_t)n + (size_t)m;
	if (len > sizeof(line) - 2) {
		len = sizeof(line) - 2;
	}
	line[len++] = '\n';
	if (console_fd >= 0) {
		(void)write(console_fd, line, len);
	}
	if (isatty(STDERR_FILENO)) {
		(void)write(STDERR_FILENO, line, len);
	}
}

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int
slider_read(uint64_t *v)
{
	size_t len = sizeof(*v);
	if (sysctlbyname("andromeda.slider", v, &len, NULL, 0) != 0) {
		return errno;
	}
	return len == sizeof(*v) ? 0 : EIO;
}

/* Sleeps in the kernel until the generation differs from seen's (60 s max). */
static int
slider_wait(uint64_t seen, uint64_t *v)
{
	size_t len = sizeof(*v);
	if (sysctlbyname("andromeda.slider_wait", v, &len, &seen, sizeof(seen)) != 0) {
		return errno;
	}
	return len == sizeof(*v) ? 0 : EIO;
}

static void
describe(const char *what, uint64_t v)
{
	say("%s: slider %s (raw %#x, generation %u, opens %u)", what,
	    andromeda_slider_name(andromeda_slider_state(v)), andromeda_slider_raw(v),
	    andromeda_slider_generation(v), andromeda_slider_opens(v));
}

/* FrontBoard first (ios-flame's openapp), LaunchServices if that fails. */
static bool
open_camera(void)
{
	@autoreleasepool {
		static bool loaded;
		if (!loaded) {
			loaded = true;
			if (!dlopen(FBS_PATH, RTLD_NOW)) {
				say("dlopen FrontBoardServices: %s", dlerror());
			}
		}
		NSString *bid = @CAMERA_ID;
		NSDictionary *opts = @{ @"__UnlockDevice": @YES, @"__ActivateSuspended": @NO };
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		__block bool ok = false;
		bool asked = true;

		Class svc = NSClassFromString(@"FBSOpenApplicationService");
		Class optc = NSClassFromString(@"FBSOpenApplicationOptions");
		Class sys = NSClassFromString(@"FBSSystemService");
		if (svc && optc && [svc respondsToSelector:@selector(serviceWithDefaultShellEndpoint)]) {
			say("opening " CAMERA_ID " (FBSOpenApplicationService)");
			FBSOpenApplicationService *service = [svc serviceWithDefaultShellEndpoint];
			FBSOpenApplicationOptions *options = [optc optionsWithDictionary:opts];
			[service openApplication:bid withOptions:options completion:^(id process, NSError *error) {
				if (error) {
					say("open " CAMERA_ID ": %s", error.description.UTF8String);
				} else {
					say("opened " CAMERA_ID ": %s", [(NSObject *)process description].UTF8String);
				}
				ok = error == nil;
				dispatch_semaphore_signal(done);
			}];
		} else if (sys && [sys respondsToSelector:@selector(sharedService)]) {
			say("opening " CAMERA_ID " (FBSSystemService)");
			FBSSystemService *shared = [sys sharedService];
			[shared openApplication:bid options:opts withResult:^(NSError *error) {
				say("open " CAMERA_ID " (FBSSystemService): %s",
				    error ? error.description.UTF8String : "ok");
				ok = error == nil;
				dispatch_semaphore_signal(done);
			}];
		} else {
			say("FrontBoardServices classes not found");
			asked = false;
		}
		if (asked && dispatch_semaphore_wait(done,
		    dispatch_time(DISPATCH_TIME_NOW, (int64_t)ANSWER_S * (int64_t)NSEC_PER_SEC)) != 0) {
			say("open " CAMERA_ID ": no answer from FrontBoard in %d s", ANSWER_S);
		}
		if (ok) {
			return true;
		}

		Class ws = NSClassFromString(@"LSApplicationWorkspace");
		if (!ws && dlopen(MCS_PATH, RTLD_NOW)) {
			ws = NSClassFromString(@"LSApplicationWorkspace");
		}
		if (!ws || ![ws respondsToSelector:@selector(defaultWorkspace)]) {
			say("LSApplicationWorkspace not found");
			return false;
		}
		LSApplicationWorkspace *workspace = [ws defaultWorkspace];
		BOOL r = [workspace openApplicationWithBundleID:bid];
		say("open " CAMERA_ID " (LSApplicationWorkspace): %s", r ? "ok" : "failed");
		return r;
	}
}

static int
run(void)
{
	uint64_t v;
	int err = slider_read(&v);
	if (err == ENOENT) {
		say("andromeda.slider missing or off (needs kernel patch 0097 and andrslidercam=1|2): exiting");
		return 0;
	}
	if (err) {
		say("reading andromeda.slider: %s", strerror(err));
		return 1;
	}
	describe("started", v);
	if (andromeda_slider_state(v) == ANDROMEDA_SLIDER_NONE) {
		say("no slider reading yet (is andrslider=1 set?)");
	}
	unsigned seen_opens = andromeda_slider_opens(v);
	unsigned shown = andromeda_slider_state(v);    /* last state logged */
	double last_launch = -1e9;
	bool can_wait = true;
	for (;;) {
		uint64_t nv;
		if (can_wait) {
			err = slider_wait(v, &nv);
			if (err == EINTR) {
				continue;
			}
			if (err && err != ENOENT) {
				say("andromeda.slider_wait: %s; reading every %d ms instead", strerror(err),
				    POLL_US / 1000);
				can_wait = false;
				continue;
			}
		} else {
			usleep(POLL_US);
			err = slider_read(&nv);
		}
		if (err == ENOENT) {
			say("andromeda.slider went away: exiting");
			return 0;
		}
		if (err) {
			say("reading andromeda.slider: %s", strerror(err));
			return 1;
		}
		if (andromeda_slider_generation(nv) == andromeda_slider_generation(v)) {
			continue;       /* 60 s without news */
		}
		unsigned state = andromeda_slider_state(nv);
		if (state != shown && state != ANDROMEDA_SLIDER_MOVING) {
			describe("change", nv);
			shown = state;
		}
		if (andromeda_slider_opens(nv) != seen_opens) {
			seen_opens = andromeda_slider_opens(nv);
			if (state != ANDROMEDA_SLIDER_OPEN) {
				say("slider opened and closed again before the daemon woke: ignored");
			} else if (now_s() - last_launch < MIN_GAP_S) {
				say("slider opened again within %d s: ignored", MIN_GAP_S);
			} else {
				last_launch = now_s();
				(void)open_camera();
			}
		}
		v = nv;
	}
}

int
main(int argc, char **argv)
{
	console_fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (argc == 2 && strcmp(argv[1], "--status") == 0) {
		uint64_t v;
		int err = slider_read(&v);
		if (err) {
			printf("andromeda.slider: %s\n", strerror(err));
			return 1;
		}
		printf("andromeda.slider = %#llx: %s, raw %#x, generation %u, opens %u\n",
		    (unsigned long long)v, andromeda_slider_name(andromeda_slider_state(v)),
		    andromeda_slider_raw(v), andromeda_slider_generation(v), andromeda_slider_opens(v));
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--open") == 0) {
		return open_camera() ? 0 : 1;
	}
	if (argc != 1) {
		fprintf(stderr, "usage: andromedad [--status | --open]\n");
		return 2;
	}
	return run();
}
