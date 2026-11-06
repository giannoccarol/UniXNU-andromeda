/*
 * andromeda-apps: make sure LaunchServices knows the apps that
 * phone/andromeda-apps/install-root.sh copied into /Applications (staged on
 * the PC by scripts/add-ipa.py), so SpringBoard shows them, and say so in the
 * log.
 *
 *   andromeda-apps                   boot job org.andromeda.andromeda-apps, once per boot
 *   andromeda-apps --check           our apps against LaunchServices, no changes
 *   andromeda-apps --register        as andrapps=2 below, now
 *   andromeda-apps --list            every app LaunchServices knows: bundle id, type, path
 *   andromeda-apps --open BUNDLE_ID  open an app through FrontBoard
 *
 * The boot job does nothing unless the kernel was booted with andrapps=N:
 *   andrapps=1  wait until LaunchServices is ready, log the state of each
 *               app; registerApplication: for the missing ones
 *   andrapps=2  as 1, then, if some are still missing, one rebuild of the
 *               LaunchServices databases (all apps and extensions: minutes)
 *   andrappslist=1      also log every app LaunchServices knows
 *   andrappsopen=ID     then open app ID (needs andrapps>=1); no touch needed
 * On a freshly built root lsd registers /Applications by itself while it
 * builds its first database (seen by ios-apollo and ios-flame); andrapps=2
 * is for apps added to a root that has already booted.
 *
 * Our apps are listed in MANIFEST (written by install-apps.py). Log lines go
 * to /dev/console ("andromeda-apps: ..."), so they reach the persistent log
 * and the USB live log, like andromedad's.
 *
 * Reused from ios-flame (Nick Wylynko, MIT, commit 745c4341; licenses/
 * ios-flame-MIT.txt, CREDITS.md): the LaunchServices calls and the rebuild
 * of phone/tools/lsreg.m (registered YouTube as a System app on the same
 * iOS build) and its entitlement names; reading kern.bootargs and raising
 * the job's jetsam limit as phone/wifi/wland.c does. Opening an app: as in
 * phone/andromedad/andromedad.m (from ios-flame's phone/tools/openapp.m).
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define MANIFEST     "/private/var/local/andromeda-apps/apps.plist"
#define READY_ID     @"com.apple.Preferences"   /* LaunchServices is up once Settings is known */
#define READY_MAX_S  900                        /* first boot: lsd builds its database */
#define STABLE_S     30                         /* app count unchanged this long */
#define LATE_S       120                        /* our apps may come later in the same pass */
#define POLL_S       10
#define ANSWER_S     15                         /* FrontBoard's answer to an open */
#define MEMLIMIT_MB  128

#define FBS_PATH "/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices"
#define MCS_PATH "/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices"
#define CS_PATH  "/System/Library/Frameworks/CoreServices.framework/CoreServices"

/* Private classes, looked up at run time (no link-time dependency). */
@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)_LSPrivateRebuildApplicationDatabasesForSystemApps:(BOOL)system internal:(BOOL)internal user:(BOOL)user;
- (NSArray *)allInstalledApplications;
- (BOOL)openApplicationWithBundleID:(NSString *)bundleID;
@end

@interface LSApplicationProxy : NSObject
+ (instancetype)applicationProxyForIdentifier:(NSString *)bundleID;
- (NSString *)applicationIdentifier;
- (NSString *)applicationType;
- (NSURL *)bundleURL;
- (BOOL)isInstalled;
- (BOOL)isPlaceholder;
@end

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

/* libsystem_kernel SPI (MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES), found at run time. */
struct memlimit_properties {
	int32_t active;
	uint32_t active_attr;
	int32_t inactive;
	uint32_t inactive_attr;
};
#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES 7

enum app_state { APP_MISSING, APP_PLACEHOLDER, APP_ELSEWHERE, APP_OK };

static int console_fd = -1;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* One write per line: to the kernel console, and to a terminal if any. */
static void
say(const char *fmt, ...)
{
	char line[768];
	int n = snprintf(line, sizeof(line), "andromeda-apps: ");
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

static const char *
str(NSString *s)
{
	return s ? s.UTF8String : "(none)";
}

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* NAME=VALUE (or bare NAME, which counts as "1") from the kernel boot-args. */
static bool
bootarg(const char *name, char *value, size_t len)
{
	char args[4096];
	size_t n = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) != 0) {
		return false;
	}
	args[n < sizeof(args) ? n : sizeof(args) - 1] = '\0';
	size_t nl = strlen(name);
	char *save = NULL;
	for (char *tok = strtok_r(args, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		if (strncmp(tok, name, nl) == 0 && (tok[nl] == '\0' || tok[nl] == '=')) {
			snprintf(value, len, "%s", tok[nl] == '=' ? tok + nl + 1 : "1");
			return true;
		}
	}
	return false;
}

/* launchd gives jobs a small jetsam limit; enumerating LaunchServices needs more. */
static void
raise_memory_limit(void)
{
	int (*control)(uint32_t, int32_t, uint32_t, void *, size_t) =
	    (int (*)(uint32_t, int32_t, uint32_t, void *, size_t))dlsym(RTLD_DEFAULT, "memorystatus_control");
	struct memlimit_properties ml = { MEMLIMIT_MB, 0, MEMLIMIT_MB, 0 };
	if (!control || control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, getpid(), 0, &ml, sizeof(ml)) != 0) {
		say("could not raise the memory limit to %d MB (%s)", MEMLIMIT_MB,
		    control ? strerror(errno) : "no memorystatus_control");
	}
}

static LSApplicationWorkspace *
workspace(void)
{
	static LSApplicationWorkspace *ws;
	if (!ws) {
		Class c = NSClassFromString(@"LSApplicationWorkspace");
		if (!c) {
			(void)dlopen(MCS_PATH, RTLD_NOW);
			c = NSClassFromString(@"LSApplicationWorkspace");
		}
		if (!c) {
			(void)dlopen(CS_PATH, RTLD_NOW);
			c = NSClassFromString(@"LSApplicationWorkspace");
		}
		if (!c || ![c respondsToSelector:@selector(defaultWorkspace)]) {
			say("LSApplicationWorkspace not found");
			return nil;
		}
		ws = [c defaultWorkspace];
	}
	return ws;
}

/* The proxy of an app LaunchServices knows, or nil. */
static LSApplicationProxy *
known_app(NSString *bundleID)
{
	Class c = NSClassFromString(@"LSApplicationProxy");
	if (!c || ![c respondsToSelector:@selector(applicationProxyForIdentifier:)]) {
		return nil;
	}
	LSApplicationProxy *p = [c applicationProxyForIdentifier:bundleID];
	if (!p || !p.bundleURL) {
		return nil;
	}
	if ([p respondsToSelector:@selector(isInstalled)] && !p.isInstalled) {
		return nil;
	}
	return p;
}

/* How many apps LaunchServices lists, or -1 if it cannot say. */
static long
app_count(void)
{
	LSApplicationWorkspace *ws = workspace();
	if (!ws || ![ws respondsToSelector:@selector(allInstalledApplications)]) {
		return -1;
	}
	return (long)[ws allInstalledApplications].count;
}

static NSString *
standard_path(NSString *path)
{
	return path.stringByResolvingSymlinksInPath.stringByStandardizingPath;
}

static enum app_state
app_state(NSDictionary *app, bool log)
{
	NSString *bid = app[@"CFBundleIdentifier"];
	NSString *path = app[@"Path"];
	LSApplicationProxy *p = known_app(bid);
	enum app_state state = APP_OK;
	if (!p) {
		state = APP_MISSING;
	} else if ([p respondsToSelector:@selector(isPlaceholder)] && p.isPlaceholder) {
		state = APP_PLACEHOLDER;
	} else if (![standard_path(p.bundleURL.path) isEqualToString:standard_path(path)]) {
		state = APP_ELSEWHERE;
	}
	if (log) {
		switch (state) {
		case APP_OK:
			say("%s: registered (%s app at %s)", str(bid), str(p.applicationType), str(p.bundleURL.path));
			break;
		case APP_PLACEHOLDER:
			say("%s: only a placeholder is registered", str(bid));
			break;
		case APP_ELSEWHERE:
			say("%s: registered at %s, not at %s", str(bid), str(p.bundleURL.path), str(path));
			break;
		case APP_MISSING:
			say("%s: not registered (%s)", str(bid), str(path));
			break;
		}
	}
	return state;
}

/* Number of manifest apps in APP_OK state; logs each one if LOG. */
static unsigned
count_ok(NSArray *apps, bool log)
{
	unsigned ok = 0;
	for (NSDictionary *app in apps) {
		@autoreleasepool {
			ok += app_state(app, log) == APP_OK;
		}
	}
	return ok;
}

static NSArray *
manifest_apps(void)
{
	NSData *data = [NSData dataWithContentsOfFile:@MANIFEST];
	if (!data) {
		return @[];
	}
	NSError *err = nil;
	id plist = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable
	    format:NULL error:&err];
	id list = [plist isKindOfClass:[NSDictionary class]] ? plist[@"Apps"] : nil;
	if (![list isKindOfClass:[NSArray class]]) {
		say("%s: unreadable (%s)", MANIFEST, err ? str(err.description) : "no Apps array");
		return @[];
	}
	NSMutableArray *apps = [NSMutableArray array];
	for (id app in list) {
		if ([app isKindOfClass:[NSDictionary class]] &&
		    [app[@"CFBundleIdentifier"] isKindOfClass:[NSString class]] &&
		    [app[@"Path"] isKindOfClass:[NSString class]]) {
			[apps addObject:app];
		}
	}
	return apps;
}

/* Wait until Settings is registered and the app count holds still (first boot: lsd seeding). */
static void
wait_ready(void)
{
	double start = now_s(), steady = start;
	long last = -2;
	bool said = false;
	for (;;) {
		@autoreleasepool {
			long count = app_count();
			bool settings = known_app(READY_ID) != nil;
			double t = now_s();
			if (count != last) {
				last = count;
				steady = t;
			}
			if (settings && t - steady >= STABLE_S) {
				say("LaunchServices ready after %.0f s: %ld apps", t - start, count);
				return;
			}
			if (t - start >= READY_MAX_S) {
				say("LaunchServices not ready after %d s (%ld apps, Settings %s): going on", READY_MAX_S,
				    count, settings ? "known" : "unknown");
				return;
			}
			if (!said) {
				say("waiting for LaunchServices (%ld apps so far)", count);
				said = true;
			}
		}
		sleep(POLL_S);
	}
}

static void
list_apps(bool to_log)
{
	LSApplicationWorkspace *ws = workspace();
	if (!ws || ![ws respondsToSelector:@selector(allInstalledApplications)]) {
		say("allInstalledApplications not available");
		return;
	}
	for (LSApplicationProxy *p in [ws allInstalledApplications]) {
		@autoreleasepool {
			const char *id_ = str(p.applicationIdentifier), *type = str(p.applicationType);
			const char *path = str(p.bundleURL.path);
			const char *ph = [p respondsToSelector:@selector(isPlaceholder)] && p.isPlaceholder ? " (placeholder)" : "";
			if (to_log) {
				say("app %s %s %s%s", id_, type, path, ph);
			} else {
				printf("%-45s %-8s %s%s\n", id_, type, path, ph);
			}
		}
	}
}

static void
register_missing(NSArray *apps)
{
	LSApplicationWorkspace *ws = workspace();
	if (!ws) {
		return;
	}
	for (NSDictionary *app in apps) {
		@autoreleasepool {
			if (app_state(app, false) == APP_OK) {
				continue;
			}
			NSURL *url = [NSURL fileURLWithPath:app[@"Path"]];
			BOOL r = [ws registerApplication:url];
			say("registerApplication %s -> %d", str(app[@"Path"]), r);
		}
	}
}

static bool
rebuild(void)
{
	LSApplicationWorkspace *ws = workspace();
	SEL sel = @selector(_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:);
	if (!ws || ![ws respondsToSelector:sel]) {
		say("the LaunchServices rebuild call is not available");
		return false;
	}
	say("rebuilding the LaunchServices databases (system, internal and user apps)");
	double t0 = now_s();
	BOOL r = [ws _LSPrivateRebuildApplicationDatabasesForSystemApps:YES internal:YES user:YES];
	say("rebuild -> %d after %.0f s", r, now_s() - t0);
	return r;
}

/* Poll until every app is registered or SECS pass; returns how many are. */
static unsigned
wait_registered(NSArray *apps, int secs)
{
	double end = now_s() + secs;
	unsigned ok;
	while ((ok = count_ok(apps, false)) < apps.count && now_s() < end) {
		sleep(POLL_S);
	}
	return ok;
}

/* FrontBoard first, LaunchServices if that fails (as andromedad opens the Camera). */
static bool
open_app(NSString *bid)
{
	@autoreleasepool {
		if (!dlopen(FBS_PATH, RTLD_NOW)) {
			say("dlopen FrontBoardServices: %s", dlerror());
		}
		NSDictionary *opts = @{ @"__UnlockDevice": @YES, @"__ActivateSuspended": @NO };
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		__block bool ok = false;
		bool asked = true;
		Class svc = NSClassFromString(@"FBSOpenApplicationService");
		Class optc = NSClassFromString(@"FBSOpenApplicationOptions");
		Class sys = NSClassFromString(@"FBSSystemService");
		if (svc && optc && [svc respondsToSelector:@selector(serviceWithDefaultShellEndpoint)]) {
			say("opening %s (FBSOpenApplicationService)", str(bid));
			FBSOpenApplicationService *service = [svc serviceWithDefaultShellEndpoint];
			FBSOpenApplicationOptions *options = [optc optionsWithDictionary:opts];
			[service openApplication:bid withOptions:options completion:^(id process, NSError *error) {
				if (error) {
					say("open %s: %s", str(bid), str(error.description));
				} else {
					say("opened %s: %s", str(bid), str([(NSObject *)process description]));
				}
				ok = error == nil;
				dispatch_semaphore_signal(done);
			}];
		} else if (sys && [sys respondsToSelector:@selector(sharedService)]) {
			say("opening %s (FBSSystemService)", str(bid));
			FBSSystemService *shared = [sys sharedService];
			[shared openApplication:bid options:opts withResult:^(NSError *error) {
				say("open %s (FBSSystemService): %s", str(bid), error ? str(error.description) : "ok");
				ok = error == nil;
				dispatch_semaphore_signal(done);
			}];
		} else {
			say("FrontBoardServices classes not found");
			asked = false;
		}
		if (asked && dispatch_semaphore_wait(done,
		    dispatch_time(DISPATCH_TIME_NOW, (int64_t)ANSWER_S * (int64_t)NSEC_PER_SEC)) != 0) {
			say("open %s: no answer from FrontBoard in %d s", str(bid), ANSWER_S);
		}
		if (ok) {
			return true;
		}
		LSApplicationWorkspace *w = workspace();
		if (!w || ![w respondsToSelector:@selector(openApplicationWithBundleID:)]) {
			return false;
		}
		BOOL r = [w openApplicationWithBundleID:bid];
		say("open %s (LSApplicationWorkspace): %s", str(bid), r ? "ok" : "failed");
		return r;
	}
}

/* level 0: report only; 1: + registerApplication:; 2: + one rebuild. Returns 0 if all are registered. */
static int
check(int level, bool wait)
{
	NSArray *apps = manifest_apps();
	if (wait) {
		wait_ready();
	}
	if (apps.count == 0) {
		say("no apps in %s (install them with scripts/add-ipa.py and the root build)", MANIFEST);
		return 0;
	}
	unsigned ok = count_ok(apps, false);
	if (ok < apps.count && wait) {
		ok = wait_registered(apps, LATE_S);     /* the same seeding pass may still be on its way */
	}
	if (ok < apps.count && level >= 1) {
		register_missing(apps);
		ok = wait_registered(apps, 2 * POLL_S);
	}
	if (ok < apps.count && level >= 2 && rebuild()) {
		wait_ready();
		ok = wait_registered(apps, LATE_S);
	}
	ok = count_ok(apps, true);
	say("%u of %lu apps registered%s", ok, (unsigned long)apps.count,
	    ok < apps.count && level < 2 ? " (boot once with andrapps=2 to rebuild LaunchServices)" : "");
	return ok == apps.count ? 0 : 1;
}

static int
boot_job(void)
{
	char value[256];
	if (!bootarg("andrapps", value, sizeof(value)) || atoi(value) <= 0) {
		say("andrapps not set: nothing to do");
		return 0;
	}
	int level = atoi(value);
	say("andrapps=%d, %lu app(s) in %s", level, (unsigned long)manifest_apps().count, MANIFEST);
	(void)check(level >= 2 ? 2 : 1, true);
	if (bootarg("andrappslist", value, sizeof(value)) && atoi(value) > 0) {
		list_apps(true);
	}
	if (bootarg("andrappsopen", value, sizeof(value)) && strcmp(value, "1") != 0) {
		NSString *bid = [NSString stringWithUTF8String:value];
		if (!open_app(bid)) {
			sleep(30);                      /* SpringBoard may still be starting */
			(void)open_app(bid);
		}
	}
	return 0;                               /* never non-zero: launchd must not run us again */
}

int
main(int argc, char **argv)
{
	console_fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	raise_memory_limit();
	@autoreleasepool {
		if (argc == 1) {
			return boot_job();
		}
		if (argc == 2 && strcmp(argv[1], "--check") == 0) {
			return check(0, false);
		}
		if (argc == 2 && strcmp(argv[1], "--register") == 0) {
			return check(2, false);
		}
		if (argc == 2 && strcmp(argv[1], "--list") == 0) {
			list_apps(false);
			return 0;
		}
		if (argc == 3 && strcmp(argv[1], "--open") == 0) {
			return open_app([NSString stringWithUTF8String:argv[2]]) ? 0 : 1;
		}
		fprintf(stderr, "usage: andromeda-apps [--check | --register | --list | --open BUNDLE_ID]\n");
		return 2;
	}
}
