/*
 * AndromedaAudio.driver: the audio devices of an iPhone X, for iOS 16.7.16 on
 * the Xiaomi Mi Mix 3 5G (andromeda) while the phone has no audio driver
 * (phase A0 of docs/research/audio.md). Nothing here touches any hardware:
 * what iOS plays is counted and thrown away, what it records is silence.
 *
 * Why. Above the audio hardware layer (the HAL of CoreAudio, inside
 * mediaserverd) sits the virtual audio device,
 * /Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin, through which everything in
 * iOS plays and records. It knows an iPhone's own devices by their names
 * (UIDs) and makes its ports of them. With no device at all its default
 * device 'vdef' never exists and mediaserverd dies in a loop (SIGSEGV in
 * ApplyBeamFormerPreference <- VirtualAudio_PlugIn::SetPropertyData, as
 * analysed in ios-sweet's NOTES.md), so scripts/sweet-root-tweaks.py parks
 * VirtualAudio.plugin in HAL.disabled; then mediaserverd lives but nothing
 * that plays sound has anywhere to go (video playback stops on CoreAudio,
 * ios-flame's HANDOFF.md). This plug-in gives the HAL the devices VirtualAudio
 * looks for; install-root.sh puts VirtualAudio back next to it.
 *
 * Origin. This is HakiOS's HakiosAudio.driver
 * (userland/src/audio_plugin.c and audio_plugin.plist of
 * https://github.com/oosexgang/hakios-xnu-kernel at commit
 * 63fc2b4633bac3af8e57dadab9f5a8d4cb940438, by OOSEX GANG, the HakiOS
 * authors; MIT, notice below) with its kernel side taken out. Kept from it:
 * the AudioServerPlugIn declarations (the iOS SDK has no header for them),
 * the four devices and their names, their streams, the speaker's volume
 * control, the codec's data source, the clock and the report of properties
 * asked for and not had. Changed here: no /dev/hakios-audio, -mic, -haptic
 * and no volume ioctl (output is discarded, input is silence); log lines to
 * /dev/console and a reporting thread instead of writes from the I/O path;
 * the zero time stamp by division instead of a loop; the receiver experiment
 * (andraudiorcv=1, below); the host clock check at load; names and the
 * factory UUID, so that both plug-ins could never be confused.
 *
 *   MIT License
 *
 *   Copyright (c) 2025-2026 the HakiOS authors
 *   Copyright (c) 2026 the andromeda port contributors (changes listed above)
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 * The devices are HakiOS's choice: declared working by HakiOS on the POCO X5
 * Pro, and reported playing iOS's sounds by Scion on the Galaxy S9 (their
 * AUDIO.md, 3 November 2026), both on iOS 16.5. Not yet tried on this phone.
 *
 *   "Speaker"   stereo output, discarded, with a volume control (VirtualAudio
 *               sets it once to its top and scales the samples itself), and a
 *               stereo input that is always silent: VirtualAudio's speaker port
 *   "Hawking"   one silent input and nothing else: VirtualAudio will not build
 *               the speaker route without its second virtual device 'vhaw',
 *               whose only port is this input
 *   "Actuator"  one output channel, discarded: the Taptic Engine, which iOS
 *               plays its haptics on as waveforms
 *   "Codec"     one input channel, silent, with a data source control 'imic'
 *               (VirtualAudio takes a codec only with known microphone data
 *               sources): the built-in microphone port
 *
 * Receiver (boot-arg andraudiorcv=1, an experiment, off by default). On an
 * iPhone X the receiver is not a device of its own: VirtualAudio of 20H392
 * (Device_Codec_Aspen.cpp) makes "Built-In Receiver" of the Codec device when
 * the HAL says it has a mute control on output element 3 (element 4 gives the
 * built-in speaker port, 5 the hearing-aid port). Nothing documented: read
 * from the property addresses that code passes to AudioObjectHasProperty, in
 * a local copy of 20H392's VirtualAudio (no Apple file is in this
 * repository). With the boot-arg the codec gets a 4-channel output
 * (discarded) with a mute and a volume control on element 3. Nobody has seen
 * this work; whether VirtualAudio wants more of the codec for it, the report
 * of unknown properties will say.
 *
 * Every device runs on the system's clock: it claims to play exactly RATE
 * frames a second of mach_absolute_time(), and the HAL's I/O thread paces
 * itself by that. CoreAudio may assume a 24 MHz host clock where this phone's
 * timer runs at 19.2 MHz (UniXNU-sheng found that on iPadOS 27, see
 * docs/research/audio.md 2.3); at load the plug-in says what CoreAudio's
 * AudioGetHostClockFrequency() answers, and every REPORT_S seconds how many
 * frames a second the HAL really delivered.
 *
 * Log lines ("andromeda-audio: ...") go to /dev/console, which XNU's devfs
 * makes writable for everyone (0622), so they reach the persistent log and
 * the USB live log although mediaserverd runs as mobile; without it, stderr.
 * Nothing is written from the HAL's real-time I/O thread: it only counts.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* ---- the AudioServerPlugIn interface (macOS: CoreAudio/AudioServerPlugIn.h, AudioHardwareBase.h) ---- */

typedef UInt32 AudioObjectID;
typedef struct {
	UInt32 mSelector, mScope, mElement;
} AudioObjectPropertyAddress;
typedef struct {
	AudioStreamBasicDescription mFormat;
	AudioValueRange mSampleRateRange;
} AudioStreamRangedDescription;
typedef struct {
	UInt32 mClientID;
	pid_t mProcessID;
	Boolean mIsNativeEndian;
	CFStringRef mBundleID;
} AudioServerPlugInClientInfo;
typedef struct {
	UInt64 mIOCycleCounter;
	UInt32 mNominalIOBufferFrameSize;
	AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
	Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;

typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};

typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
	OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
	OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
	OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
	OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32 *);
	OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, UInt32 *, void *);
	OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, const void *);
	OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
	OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *, void *, void *);
	OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
};

#define kAudioServerPlugInTypeUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, \
	    0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define kAudioServerPlugInDriverInterfaceUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, \
	    0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

enum {
	kAudioHardwareUnknownPropertyError = 'who?', kAudioHardwareBadPropertySizeError = '!siz',
	kAudioHardwareIllegalOperationError = 'nope', kAudioHardwareBadObjectError = '!obj',
	kAudioHardwareUnsupportedOperationError = 'unop', kAudioDeviceUnsupportedFormatError = '!dat',

	kAudioObjectPropertyScopeGlobal = 'glob', kAudioObjectPropertyScopeInput = 'inpt', kAudioObjectPropertyScopeOutput = 'outp',

	kAudioObjectClassID = 'aobj', kAudioPlugInClassID = 'aplg', kAudioDeviceClassID = 'adev', kAudioStreamClassID = 'astr',

	kAudioObjectPropertyBaseClass = 'bcls', kAudioObjectPropertyClass = 'clas', kAudioObjectPropertyOwner = 'stdv',
	kAudioObjectPropertyName = 'lnam', kAudioObjectPropertyManufacturer = 'lmak', kAudioObjectPropertyOwnedObjects = 'ownd',

	kAudioPlugInPropertyBoxList = 'box#', kAudioPlugInPropertyTranslateUIDToBox = 'uidb',
	kAudioPlugInPropertyDeviceList = 'dev#', kAudioPlugInPropertyTranslateUIDToDevice = 'uidd',
	kAudioPlugInPropertyResourceBundle = 'rsrc',

	kAudioDevicePropertyDeviceUID = 'uid ', kAudioDevicePropertyModelUID = 'muid', kAudioDevicePropertyTransportType = 'tran',
	kAudioDevicePropertyRelatedDevices = 'akin', kAudioDevicePropertyClockDomain = 'clkd',
	kAudioDevicePropertyDeviceIsAlive = 'livn', kAudioDevicePropertyDeviceIsRunning = 'goin',
	kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt', kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
	kAudioDevicePropertyLatency = 'ltnc', kAudioDevicePropertyStreams = 'stm#', kAudioObjectPropertyControlList = 'ctrl',
	kAudioDevicePropertySafetyOffset = 'saft', kAudioDevicePropertyNominalSampleRate = 'nsrt',
	kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#', kAudioDevicePropertyIsHidden = 'hidn',
	kAudioDevicePropertyPreferredChannelsForStereo = 'dch2', kAudioDevicePropertyZeroTimeStampPeriod = 'ring',
	kAudioSelectorControlClassID = 'slct', kAudioDataSourceControlClassID = 'dsrc',
	kAudioSelectorControlPropertyCurrentItem = 'scci', kAudioSelectorControlPropertyAvailableItems = 'scai',
	kAudioSelectorControlPropertyItemName = 'scin',

	kAudioStreamPropertyIsActive = 'sact', kAudioStreamPropertyDirection = 'sdir', kAudioStreamPropertyTerminalType = 'term',
	kAudioStreamPropertyStartingChannel = 'schn', kAudioStreamPropertyLatency = 'ltnc',
	kAudioStreamPropertyVirtualFormat = 'sfmt', kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
	kAudioStreamPropertyPhysicalFormat = 'pft ', kAudioStreamPropertyAvailablePhysicalFormats = 'pfta',

	kAudioStreamTerminalTypeSpeaker = 'spkr', kAudioStreamTerminalTypeReceiverSpeaker = 'rspk',
	kAudioStreamTerminalTypeMicrophone = 'micr',

	kAudioServerPlugInIOOperationReadInput = 'read', kAudioServerPlugInIOOperationWriteMix = 'rite',

	kAudioLevelControlClassID = 'levl', kAudioVolumeControlClassID = 'vlme',
	kAudioBooleanControlClassID = 'togl', kAudioMuteControlClassID = 'mute',
	kAudioControlPropertyScope = 'cscp', kAudioControlPropertyElement = 'celm',
	kAudioLevelControlPropertyScalarValue = 'lcsv', kAudioLevelControlPropertyDecibelValue = 'lcdv',
	kAudioLevelControlPropertyDecibelRange = 'lcdr', kAudioLevelControlPropertyConvertScalarToDecibels = 'lcsd',
	kAudioLevelControlPropertyConvertDecibelsToScalar = 'lcds',
	kAudioBooleanControlPropertyValue = 'bcvl',
};

/* ---- the devices ---------------------------------------------------------------------------------- */

#define RATE                    48000
#define SAMPLE_BYTES            2       /* 16-bit signed integer, as HakiOS */
/* how often a device says where its clock is, in frames: also the largest I/O buffer the HAL will use */
#define TIMESTAMP_PERIOD        16384
#define VOLUME_RANGE_DB         48.0    /* the volume controls' scale: 3 dB for each of iOS's 16 steps */
#define TRANSPORT_BUILT_IN      'bltn'
#define MIC_SOURCE              'imic'  /* the iPhone's internal (bottom) microphone */
#define MANUFACTURER            CFSTR("andromeda")
#define RECEIVER_ELEMENT        3       /* VirtualAudio's receiver: a mute control on the codec's output element 3 */
#define CODEC_OUTPUT_CHANNELS   4       /* so that element 3 is a channel of it */
#define REPORT_S                10      /* the reporting thread's period */
#define IO_RUNS_TOLD            100     /* start/stop lines per device; after that one in IO_RUNS_TOLD */

/* Object numbers are ours to choose. A device's streams follow it: its output (if any), then its input. */
enum {
	OBJECT_PLUGIN = 1,
	OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, OBJECT_SPEAKER_INPUT,
	OBJECT_HAWKING, OBJECT_HAWKING_INPUT,
	OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT,
	OBJECT_CODEC, OBJECT_CODEC_INPUT,
	OBJECT_SPEAKER_VOLUME,          /* the speaker output's volume control */
	OBJECT_CODEC_SOURCE,            /* the codec input's data source control: its microphone */
	/* only with andraudiorcv=1 */
	OBJECT_CODEC_OUTPUT,            /* the codec's output, of which VirtualAudio makes the receiver */
	OBJECT_RECEIVER_MUTE,           /* mute control, output element RECEIVER_ELEMENT */
	OBJECT_RECEIVER_VOLUME,         /* volume control, output element RECEIVER_ELEMENT */
	OBJECT_LAST = OBJECT_RECEIVER_VOLUME
};

struct device {
	AudioObjectID object;
	CFStringRef uid;                /* the name VirtualAudio knows it by */
	const char *name;               /* the same, for the log */
	AudioObjectID output, input;    /* its streams; 0 = none */
	UInt32 output_channels, input_channels;
	/* its clock, running while anybody does I/O (with the lock held) */
	unsigned running;               /* how many clients have started I/O */
	UInt64 anchor;                  /* mach_absolute_time() when the first of them did */
	UInt64 periods;                 /* whole time stamp periods since */
	unsigned runs;                  /* I/O runs since load */
	UInt64 reported_out, reported_in;       /* frames at the reporting thread's last look */
	UInt64 reported_at;             /* and when it looked (mach time) */
	/* counted on the I/O thread, read by the others */
	_Atomic UInt64 out_frames, in_frames, cycles;
	_Atomic UInt32 peak;            /* the loudest output sample of the run, of 32768 */
};

static struct device devices[] = {
	{ .object = OBJECT_SPEAKER, .uid = CFSTR("Speaker"), .name = "Speaker",
	  .output = OBJECT_SPEAKER_OUTPUT, .input = OBJECT_SPEAKER_INPUT, .output_channels = 2, .input_channels = 2 },
	{ .object = OBJECT_HAWKING, .uid = CFSTR("Hawking"), .name = "Hawking",
	  .input = OBJECT_HAWKING_INPUT, .input_channels = 1 },
	{ .object = OBJECT_ACTUATOR, .uid = CFSTR("Actuator"), .name = "Actuator",
	  .output = OBJECT_ACTUATOR_OUTPUT, .output_channels = 1 },
	{ .object = OBJECT_CODEC, .uid = CFSTR("Codec"), .name = "Codec",
	  .input = OBJECT_CODEC_INPUT, .input_channels = 1 },    /* .output: set by initialize() with andraudiorcv=1 */
};
#define DEVICES                 (sizeof(devices) / sizeof(devices[0]))
#define CODEC                   (&devices[3])

/* The volume controls: on the speaker's output (HakiOS's), and the receiver's (andraudiorcv=1). */
struct level {
	AudioObjectID object, owner;
	UInt32 element;                 /* all are on the output */
	Float32 scalar;                 /* 0 to 1, as iOS last set it */
	const char *name;
};
static struct level levels[] = {
	{ OBJECT_SPEAKER_VOLUME, OBJECT_SPEAKER, 0, 1.0f, "Speaker volume" },
	{ OBJECT_RECEIVER_VOLUME, OBJECT_CODEC, RECEIVER_ELEMENT, 1.0f, "receiver volume" },
};
#define LEVELS                  (sizeof(levels) / sizeof(levels[0]))

static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Float64 ticks_per_frame;         /* mach_absolute_time() ticks */
static Boolean inactive[OBJECT_LAST + 1];       /* streams the HAL has switched off */
static bool receiver;                   /* andraudiorcv=1 */
static UInt32 receiver_muted;           /* the receiver's mute control */

/* ---- the log ------------------------------------------------------------------------------------- */

static int console = -2;                /* /dev/console; -1: not openable (stderr then); -2: not tried */

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
say(const char *format, ...)
{
	char line[320];
	int n = snprintf(line, sizeof(line), "andromeda-audio: ");
	va_list ap;
	va_start(ap, format);
	int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, format, ap);
	va_end(ap);
	size_t length = (size_t)n + (m < 0 ? 0 : (size_t)m);
	if (length > sizeof(line) - 2) {
		length = sizeof(line) - 2;
	}
	line[length++] = '\n';
	if (console == -2) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	}
	/* one write per line */
	(void)write(console >= 0 ? console : STDERR_FILENO, line, length);
}

static double
seconds(UInt64 ticks)
{
	return (double)ticks / (ticks_per_frame * RATE);
}

/* ---- helpers ---------------------------------------------------------------------------------------- */

/* The volume controls' scale: a straight line of decibels over VOLUME_RANGE_DB. */
static Float32
scalar_to_db(Float32 scalar)
{
	scalar = scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
	return (Float32)(-VOLUME_RANGE_DB * (1.0 - scalar));
}

static Float32
db_to_scalar(Float32 db)
{
	Float32 scalar = (Float32)(1.0 + db / VOLUME_RANGE_DB);

	return scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
}

/* The device with this object number, or the one this stream belongs to; NULL = neither. */
static struct device *
device_of(AudioObjectID object)
{
	for (unsigned i = 0; i < DEVICES; i++) {
		if (object == devices[i].object || (object != 0 && (object == devices[i].output || object == devices[i].input))) {
			return &devices[i];
		}
	}
	return NULL;
}

/* The volume control with this object number, if it is there. */
static struct level *
level_of(AudioObjectID object)
{
	if (object == OBJECT_RECEIVER_VOLUME && !receiver) {
		return NULL;
	}
	for (unsigned i = 0; i < LEVELS; i++) {
		if (levels[i].object == object) {
			return &levels[i];
		}
	}
	return NULL;
}

/* The one format of a stream: 16-bit samples at RATE, so many channels. */
static AudioStreamBasicDescription
format_of(AudioObjectID stream)
{
	const struct device *d = device_of(stream);
	UInt32 channels = stream == d->output ? d->output_channels : d->input_channels;

	return (AudioStreamBasicDescription) {
		       .mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
		       .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		       .mBytesPerPacket = SAMPLE_BYTES * channels, .mFramesPerPacket = 1,
		       .mBytesPerFrame = SAMPLE_BYTES * channels, .mChannelsPerFrame = channels, .mBitsPerChannel = 8 * SAMPLE_BYTES,
	};
}

/* NAME=N among the kernel's boot-args (kern.bootargs); 0 if absent, 1 if given bare. */
static long
boot_arg(const char *name)
{
	char args[1024], *state = NULL;
	size_t size = sizeof(args) - 1;
	size_t length = strlen(name);

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return 0;
	}
	args[size < sizeof(args) ? size : sizeof(args) - 1] = '\0';
	for (char *word = strtok_r(args, " \t", &state); word != NULL; word = strtok_r(NULL, " \t", &state)) {
		if (strncmp(word, name, length) == 0 && (word[length] == '\0' || word[length] == '=')) {
			return word[length] == '=' ? strtol(word + length + 1, NULL, 0) : 1;
		}
	}
	return 0;
}

/* ---- the plug-in --------------------------------------------------------------------------------- */

static HRESULT
query_interface(void *driver, REFIID uuid, LPVOID *interface)
{
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
	Boolean ours = requested != NULL &&
	    (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID));

	if (requested != NULL) {
		CFRelease(requested);
	}
	if (interface == NULL || !ours) {
		return E_NOINTERFACE;
	}
	*interface = driver;            /* one driver, never freed: no counting */
	return S_OK;
}

static ULONG
add_ref(__unused void *driver)
{
	return 1;
}

static ULONG
release(__unused void *driver)
{
	return 1;
}

static void *reporter(void *);

static OSStatus
initialize(__unused AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef the_host)
{
	mach_timebase_info_data_t timebase;
	pthread_t thread;
	pthread_attr_t attr;

	host = the_host;
	mach_timebase_info(&timebase);
	ticks_per_frame = 1e9 * timebase.denom / timebase.numer / RATE;
	receiver = boot_arg("andraudiorcv") == 1;
	if (receiver) {
		CODEC->output = OBJECT_CODEC_OUTPUT;
		CODEC->output_channels = CODEC_OUTPUT_CHANNELS;
	}
	say("loaded in %s (pid %d, uid %d): devices Speaker, Hawking, Actuator, Codec%s; output discarded, input silent",
	    getprogname(), (int)getpid(), (int)getuid(), receiver ? " with a 4-channel output and receiver controls (andraudiorcv=1)" : "");

	/* CoreAudio's idea of the host clock against the kernel's (docs/research/audio.md 2.3) */
	double host_hz = 1e9 * timebase.denom / timebase.numer;
	Float64 (*frequency)(void) = (Float64 (*)(void))dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency");
	if (frequency != NULL) {
		double ca_hz = frequency();
		say("host clock: mach_timebase_info %u/%u = %.0f Hz; CoreAudio's AudioGetHostClockFrequency() = %.0f Hz%s",
		    timebase.numer, timebase.denom, host_hz, ca_hz,
		    ca_hz > host_hz * 1.001 || ca_hz < host_hz * 0.999 ? ": they differ, CoreAudio's I/O timing will be off" : "");
	} else {
		say("host clock: mach_timebase_info %u/%u = %.0f Hz; AudioGetHostClockFrequency not found", timebase.numer,
		    timebase.denom, host_hz);
	}

	if (pthread_attr_init(&attr) == 0) {
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&thread, &attr, reporter, NULL) != 0) {
			say("no reporting thread (%s)", strerror(errno));
		}
		pthread_attr_destroy(&attr);
	}
	return 0;
}

static OSStatus
create_device(__unused AudioServerPlugInDriverRef driver, __unused CFDictionaryRef description,
    __unused const AudioServerPlugInClientInfo *client, __unused AudioObjectID *object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
destroy_device(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
device_client(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object,
    __unused const AudioServerPlugInClientInfo *client)
{
	return 0;
}

static OSStatus
configuration_change(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt64 action,
    __unused void *info)
{
	return 0;
}

/* ---- properties ---------------------------------------------------------------------------------- */

/* A property's value: `size` bytes of it, or a list with nothing in it, or (neither) no such property. */
struct value {
	UInt32 size;
	bool empty;
	bool string;                    /* a CFString: whoever is given it gets a reference of their own */
	union {
		UInt32 words[16];
		Float64 number;
		Float32 decimal;
		CFStringRef string;
		AudioValueRange range;
		AudioStreamBasicDescription format;
		AudioStreamRangedDescription ranged;
	} u;
};

#define WORD(x)         (v.u.words[0] = (x), v.size = sizeof(UInt32))
#define LIST(x)         (v.u.words[v.size / sizeof(UInt32)] = (x), v.size += sizeof(UInt32))
#define STRING(x)       (v.u.string = (x), v.string = true, v.size = sizeof(CFStringRef))
#define FLOAT(x)        (v.u.decimal = (x), v.size = sizeof(Float32))

/* What a control object answers that every control does. True if it answered. */
static bool
control_property(struct value *vp, UInt32 selector, UInt32 base, UInt32 class, AudioObjectID owner, UInt32 scope, UInt32 element)
{
	struct value v = *vp;

	switch (selector) {
	case kAudioObjectPropertyBaseClass:
		WORD(base);
		break;
	case kAudioObjectPropertyClass:
		WORD(class);
		break;
	case kAudioObjectPropertyOwner:
		WORD(owner);
		break;
	case kAudioObjectPropertyOwnedObjects:
		v.empty = true;
		break;
	case kAudioControlPropertyScope:
		WORD(scope);
		break;
	case kAudioControlPropertyElement:
		WORD(element);
		break;
	default:
		return false;
	}
	*vp = v;
	return true;
}

static struct value
property(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier)
{
	UInt32 selector = address->mSelector, scope = address->mScope;
	const struct device *d = device_of(object);
	const struct level *l = level_of(object);
	struct value v;

	memset(&v, 0, sizeof(v));
	if (l != NULL) {
		if (control_property(&v, selector, kAudioLevelControlClassID, kAudioVolumeControlClassID, l->owner,
		    kAudioObjectPropertyScopeOutput, l->element)) {
			return v;
		}
		switch (selector) {
		case kAudioLevelControlPropertyScalarValue:
			FLOAT(l->scalar);
			break;
		case kAudioLevelControlPropertyDecibelValue:
			FLOAT(scalar_to_db(l->scalar));
			break;
		case kAudioLevelControlPropertyDecibelRange:
			v.u.range.mMinimum = -VOLUME_RANGE_DB;
			v.u.range.mMaximum = 0;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			FLOAT(0);       /* (converts what it is given: get_property_data()) */
			break;
		}
		return v;
	}
	if (object == OBJECT_RECEIVER_MUTE) {
		if (receiver && !control_property(&v, selector, kAudioBooleanControlClassID, kAudioMuteControlClassID, OBJECT_CODEC,
		    kAudioObjectPropertyScopeOutput, RECEIVER_ELEMENT) && selector == kAudioBooleanControlPropertyValue) {
			WORD(receiver_muted);
		}
		return v;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		if (control_property(&v, selector, kAudioSelectorControlClassID, kAudioDataSourceControlClassID, OBJECT_CODEC,
		    kAudioObjectPropertyScopeInput, 0)) {
			return v;
		}
		switch (selector) {
		case kAudioSelectorControlPropertyCurrentItem:
		case kAudioSelectorControlPropertyAvailableItems:
			WORD(MIC_SOURCE);
			break;
		case kAudioSelectorControlPropertyItemName:
			if (qualifier_size == sizeof(UInt32) && qualifier != NULL && *(const UInt32 *)qualifier == MIC_SOURCE) {
				STRING(CFSTR("Bottom"));
			} else {
				STRING(CFSTR(""));
			}
			break;
		}
		return v;
	}
	if (object != OBJECT_PLUGIN && d == NULL) {
		return v;
	}
	switch (selector) {
	case kAudioObjectPropertyBaseClass:
		WORD(kAudioObjectClassID);
		return v;
	case kAudioObjectPropertyClass:
		WORD(object == OBJECT_PLUGIN ? kAudioPlugInClassID : object == d->object ? kAudioDeviceClassID : kAudioStreamClassID);
		return v;
	case kAudioObjectPropertyOwner:
		WORD(object == OBJECT_PLUGIN ? 0 : object == d->object ? OBJECT_PLUGIN : d->object);
		return v;
	}
	if (object == OBJECT_PLUGIN) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			for (unsigned i = 0; i < DEVICES; i++) {
				LIST(devices[i].object);
			}
			break;
		case kAudioPlugInPropertyBoxList:
			v.empty = true;
			break;
		case kAudioPlugInPropertyTranslateUIDToBox:
			WORD(0);
			break;
		case kAudioPlugInPropertyTranslateUIDToDevice:
			WORD(0);
			for (unsigned i = 0; i < DEVICES; i++) {
				if (qualifier_size == sizeof(CFStringRef) && qualifier != NULL && *(const CFStringRef *)qualifier != NULL &&
				    CFEqual(*(const CFStringRef *)qualifier, devices[i].uid)) {
					WORD(devices[i].object);
				}
			}
			break;
		case kAudioPlugInPropertyResourceBundle:
			STRING(CFSTR(""));
			break;
		}
	} else if (object == d->object) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyName:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			STRING(d->uid);
			break;
		case kAudioDevicePropertyTransportType:
			WORD(TRANSPORT_BUILT_IN);
			break;
		case kAudioDevicePropertyRelatedDevices:
			WORD(d->object);
			break;
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden:
			WORD(0);
			break;
		case kAudioDevicePropertyDeviceIsAlive:
			WORD(1);
			break;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			/* as HakiOS: the devices with an output, but not the codec's experimental one */
			WORD(d->output != 0 && d->object != OBJECT_CODEC);
			break;
		case kAudioDevicePropertyDeviceIsRunning:
			WORD(d->running != 0);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			/* the streams of one direction, or of both (and the controls the device owns) */
			if (scope != kAudioObjectPropertyScopeInput && d->output != 0) {
				LIST(d->output);
			}
			if (scope != kAudioObjectPropertyScopeOutput && d->input != 0) {
				LIST(d->input);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC && scope != kAudioObjectPropertyScopeOutput) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC && receiver &&
			    scope != kAudioObjectPropertyScopeInput) {
				LIST(OBJECT_RECEIVER_MUTE);
				LIST(OBJECT_RECEIVER_VOLUME);
			}
			v.empty = v.size == 0;
			break;
		case kAudioObjectPropertyControlList:
			if (d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (d->object == OBJECT_CODEC) {
				LIST(OBJECT_CODEC_SOURCE);
				if (receiver) {
					LIST(OBJECT_RECEIVER_MUTE);
					LIST(OBJECT_RECEIVER_VOLUME);
				}
			}
			v.empty = v.size == 0;
			break;
		case kAudioDevicePropertyNominalSampleRate:
			v.u.number = RATE;
			v.size = sizeof(Float64);
			break;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			v.u.range.mMinimum = v.u.range.mMaximum = RATE;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioDevicePropertyPreferredChannelsForStereo:
			v.u.words[0] = 1;
			v.u.words[1] = 2;
			v.size = 2 * sizeof(UInt32);
			break;
		case kAudioDevicePropertyZeroTimeStampPeriod:
			WORD(TIMESTAMP_PERIOD);
			break;
		}
	} else {
		switch (selector) {
		case kAudioStreamPropertyIsActive:
			WORD(!inactive[object]);
			break;
		case kAudioStreamPropertyDirection:
			WORD(object == d->input);
			break;
		case kAudioStreamPropertyTerminalType:
			WORD(object == d->input ? kAudioStreamTerminalTypeMicrophone :
			    object == OBJECT_CODEC_OUTPUT ? kAudioStreamTerminalTypeReceiverSpeaker : kAudioStreamTerminalTypeSpeaker);
			break;
		case kAudioStreamPropertyStartingChannel:
			WORD(1);
			break;
		case kAudioStreamPropertyLatency:
			WORD(0);
			break;
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			v.u.format = format_of(object);
			v.size = sizeof(AudioStreamBasicDescription);
			break;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			v.u.ranged.mFormat = format_of(object);
			v.u.ranged.mSampleRateRange.mMinimum = v.u.ranged.mSampleRateRange.mMaximum = RATE;
			v.size = sizeof(AudioStreamRangedDescription);
			break;
		}
	}
	return v;
}
#undef WORD
#undef LIST
#undef STRING
#undef FLOAT

/*
 * What the HAL (or VirtualAudio through it) asks of an object that it does not have, the first times
 * for each object, selector and scope: what a device VirtualAudio will not take may lack.
 */
#define UNKNOWN_TOLD            64
static UInt32 unknown_told[UNKNOWN_TOLD][3];
static unsigned unknown_count;
static pthread_mutex_t unknown_lock = PTHREAD_MUTEX_INITIALIZER;

static void
unknown(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	UInt32 s = address->mSelector, c = address->mScope;
	const struct device *d = device_of(object);
	bool tell = true;

	pthread_mutex_lock(&unknown_lock);
	for (unsigned i = 0; i < unknown_count && tell; i++) {
		tell = !(unknown_told[i][0] == object && unknown_told[i][1] == s && unknown_told[i][2] == c);
	}
	if (tell && unknown_count < UNKNOWN_TOLD) {
		unknown_told[unknown_count][0] = object;
		unknown_told[unknown_count][1] = s;
		unknown_told[unknown_count][2] = c;
		unknown_count++;
	} else {
		tell = false;
	}
	pthread_mutex_unlock(&unknown_lock);
	if (tell) {
		say("asked of object %u (%s) what it does not have: '%c%c%c%c' (%#x), scope '%c%c%c%c', element %u",
		    (unsigned)object, d != NULL ? d->name : "-",
		    (int)(s >> 24 & 0xff), (int)(s >> 16 & 0xff), (int)(s >> 8 & 0xff), (int)(s & 0xff), (unsigned)s,
		    (int)(c >> 24 & 0xff), (int)(c >> 16 & 0xff), (int)(c >> 8 & 0xff), (int)(c & 0xff), (unsigned)address->mElement);
	}
}

static Boolean
has_property(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address)
{
	struct value v;

	if (address == NULL) {
		return false;
	}
	v = property(object, address, 0, NULL);
	if (v.size == 0 && !v.empty) {
		unknown(object, address);
	}
	return v.size != 0 || v.empty;
}

static OSStatus
is_property_settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, Boolean *settable)
{
	const struct device *d = device_of(object);

	if (address == NULL || settable == NULL || !has_property(driver, object, client, address)) {
		return kAudioHardwareUnknownPropertyError;
	}
	/* the rate and the formats can be "set", to the ones there are; the controls can be set */
	if (level_of(object) != NULL) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
		    address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_RECEIVER_MUTE) {
		*settable = address->mSelector == kAudioBooleanControlPropertyValue;
	} else if (object == OBJECT_CODEC_SOURCE) {
		*settable = address->mSelector == kAudioSelectorControlPropertyCurrentItem;
	} else if (object == OBJECT_PLUGIN || d == NULL) {
		*settable = false;
	} else if (object == d->object) {
		*settable = address->mSelector == kAudioDevicePropertyNominalSampleRate;
	} else {
		*settable = address->mSelector == kAudioStreamPropertyIsActive ||
		    address->mSelector == kAudioStreamPropertyVirtualFormat || address->mSelector == kAudioStreamPropertyPhysicalFormat;
	}
	return 0;
}

static OSStatus
get_property_data_size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *size)
{
	if (address == NULL || size == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	pthread_mutex_lock(&lock);
	*size = property(object, address, qualifier_size, qualifier).size;
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
get_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 room, UInt32 *size,
    void *data)
{
	struct value v;

	if (address == NULL || size == NULL || data == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	if (level_of(object) != NULL && (address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ||
	    address->mSelector == kAudioLevelControlPropertyConvertDecibelsToScalar)) {
		/* what is in the buffer, converted where it is */
		if (room < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)data = address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ?
		    scalar_to_db(*(Float32 *)data) : db_to_scalar(*(Float32 *)data);
		*size = sizeof(Float32);
		return 0;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address, qualifier_size, qualifier);
	pthread_mutex_unlock(&lock);
	if (v.size > room) {
		/* a list may be asked for in part; anything else must fit */
		if (address->mSelector != kAudioObjectPropertyOwnedObjects && address->mSelector != kAudioDevicePropertyStreams &&
		    address->mSelector != kAudioPlugInPropertyDeviceList && address->mSelector != kAudioObjectPropertyControlList) {
			return kAudioHardwareBadPropertySizeError;
		}
		v.size = room - room % sizeof(UInt32);
	}
	memcpy(data, &v.u, v.size);
	if (v.string && v.size == sizeof(CFStringRef)) {
		CFRetain(v.u.string);
	}
	*size = v.size;
	return 0;
}

static OSStatus
set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, __unused UInt32 qualifier_size, __unused const void *qualifier, UInt32 size,
    const void *data)
{
	Boolean settable = false;
	struct level *l = level_of(object);

	if (address == NULL || data == NULL || is_property_settable(driver, object, client, address, &settable) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (!settable) {
		return kAudioHardwareIllegalOperationError;
	}
	if (l != NULL) {
		AudioObjectPropertyAddress changed[2] = {
			{ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 },
			{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
		};
		Float32 wanted;

		if (size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		wanted = *(const Float32 *)data;
		wanted = address->mSelector == kAudioLevelControlPropertyDecibelValue ? db_to_scalar(wanted) :
		    wanted < 0 ? 0 : wanted > 1 ? 1 : wanted;
		pthread_mutex_lock(&lock);
		l->scalar = wanted;
		pthread_mutex_unlock(&lock);
		/* nothing to give it to: only said */
		say("%s set to %.3f (%.1f dB)", l->name, wanted, scalar_to_db(wanted));
		if (host != NULL) {
			host->PropertiesChanged(host, l->object, 2, changed);
		}
		return 0;
	}
	if (object == OBJECT_RECEIVER_MUTE) {
		AudioObjectPropertyAddress changed = { kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, 0 };

		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		receiver_muted = *(const UInt32 *)data != 0;
		pthread_mutex_unlock(&lock);
		say("receiver %s", *(const UInt32 *)data != 0 ? "muted" : "unmuted");
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_RECEIVER_MUTE, 1, &changed);
		}
		return 0;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		/* the one microphone there is */
		return size == sizeof(UInt32) ? (*(const UInt32 *)data == MIC_SOURCE ? 0 : kAudioHardwareIllegalOperationError) :
		       kAudioHardwareBadPropertySizeError;
	}
	if (address->mSelector == kAudioStreamPropertyIsActive) {
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		inactive[object] = *(const UInt32 *)data == 0;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		return size == sizeof(Float64) && *(const Float64 *)data == RATE ? 0 : kAudioDeviceUnsupportedFormatError;
	}
	/* a format: the one we have, or nothing */
	if (size != sizeof(AudioStreamBasicDescription)) {
		return kAudioHardwareBadPropertySizeError;
	} else {
		const AudioStreamBasicDescription *wanted = data, format = format_of(object);

		return wanted->mSampleRate == format.mSampleRate && wanted->mFormatID == format.mFormatID &&
		       wanted->mChannelsPerFrame == format.mChannelsPerFrame && wanted->mBitsPerChannel == format.mBitsPerChannel &&
		       (wanted->mFormatFlags & kAudioFormatFlagIsSignedInteger) ? 0 : kAudioDeviceUnsupportedFormatError;
	}
}

/* ---- I/O ------------------------------------------------------------------------------------------- */

/*
 * The log lines of I/O are made with the lock held and written after it is let go: the I/O thread
 * takes the lock for its time stamps and must not wait on the console.
 */
#define LINE                    256

/* With the lock held: what the device's I/O run did, as one line. */
static void
describe_run(char line[LINE], struct device *d, UInt64 now)
{
	UInt64 out = atomic_load_explicit(&d->out_frames, memory_order_relaxed);
	UInt64 in = atomic_load_explicit(&d->in_frames, memory_order_relaxed);
	UInt64 cycles = atomic_load_explicit(&d->cycles, memory_order_relaxed);
	UInt32 peak = atomic_exchange_explicit(&d->peak, 0, memory_order_relaxed);
	double s = seconds(now - d->anchor);
	char rate[32] = "";

	if (s >= 1.0) {         /* a rate of a shorter run says nothing */
		snprintf(rate, sizeof(rate), "%.0f a second, ", (double)(out > in ? out : in) / s);
	}
	snprintf(line, LINE, "%s: I/O run %u stops after %.2f s: %llu frames out (%speak %u of 32768, discarded), "
	    "%llu frames in (silence), %llu I/O cycles", d->name, d->runs, s, (unsigned long long)out, rate, (unsigned)peak,
	    (unsigned long long)in, (unsigned long long)cycles);
}

static OSStatus
start_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);
	unsigned run = 0;

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running++ == 0) {
		d->anchor = d->reported_at = mach_absolute_time();
		d->periods = d->reported_out = d->reported_in = 0;
		atomic_store_explicit(&d->out_frames, 0, memory_order_relaxed);
		atomic_store_explicit(&d->in_frames, 0, memory_order_relaxed);
		atomic_store_explicit(&d->cycles, 0, memory_order_relaxed);
		atomic_store_explicit(&d->peak, 0, memory_order_relaxed);
		run = ++d->runs;
	}
	pthread_mutex_unlock(&lock);
	if (run != 0 && (run <= IO_RUNS_TOLD || run % IO_RUNS_TOLD == 0)) {
		say("%s: I/O run %u starts", d->name, run);
	}
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);
	char line[LINE] = "";

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running != 0 && --d->running == 0 && (d->runs <= IO_RUNS_TOLD || d->runs % IO_RUNS_TOLD == 0)) {
		describe_run(line, d, mach_absolute_time());
	}
	pthread_mutex_unlock(&lock);
	if (line[0] != '\0') {
		say("%s", line);
	}
	return 0;
}

/* Where the device's clock was last at a multiple of TIMESTAMP_PERIOD frames, and when. */
static OSStatus
get_zero_time_stamp(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client,
    Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
	Float64 ticks_per_period = ticks_per_frame * TIMESTAMP_PERIOD;
	UInt64 now = mach_absolute_time();
	struct device *d = device_of(object);

	if (d == NULL || sample_time == NULL || host_time == NULL || seed == NULL) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (now > d->anchor) {
		UInt64 periods = (UInt64)((Float64)(now - d->anchor) / ticks_per_period);

		if (periods > d->periods) {
			d->periods = periods;   /* never backwards */
		}
	}
	*sample_time = (Float64)d->periods * TIMESTAMP_PERIOD;
	*host_time = d->anchor + (UInt64)((Float64)d->periods * ticks_per_period);
	*seed = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
will_do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    UInt32 operation, Boolean *will_do, Boolean *in_place)
{
	if (will_do == NULL || in_place == NULL) {
		return kAudioHardwareIllegalOperationError;
	}
	*will_do = operation == kAudioServerPlugInIOOperationReadInput || operation == kAudioServerPlugInIOOperationWriteMix;
	*in_place = true;
	return 0;
}

static OSStatus
begin_end_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    __unused UInt32 operation, __unused UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

/* On the HAL's real-time thread: no lock, no log, only counting. */
static OSStatus
do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, AudioObjectID stream,
    __unused UInt32 client, UInt32 operation, UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle, void *buffer,
    __unused void *secondary)
{
	struct device *d = device_of(stream);

	if (d == NULL || stream == d->object) {
		return kAudioHardwareBadObjectError;
	}
	if (operation == kAudioServerPlugInIOOperationReadInput && stream == d->input) {
		/* every input: silence */
		if (buffer != NULL) {
			memset(buffer, 0, (size_t)frames * SAMPLE_BYTES * d->input_channels);
		}
		atomic_fetch_add_explicit(&d->in_frames, frames, memory_order_relaxed);
		atomic_fetch_add_explicit(&d->cycles, 1, memory_order_relaxed);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == d->output) {
		/* every output: looked at, then dropped */
		const int16_t *samples = buffer;
		size_t count = (size_t)frames * d->output_channels;
		UInt32 peak = 0;

		for (size_t i = 0; samples != NULL && i < count; i++) {
			int value = samples[i] < 0 ? -(int)samples[i] : samples[i];

			peak = (UInt32)value > peak ? (UInt32)value : peak;
		}
		if (peak > atomic_load_explicit(&d->peak, memory_order_relaxed)) {
			atomic_store_explicit(&d->peak, peak, memory_order_relaxed);
		}
		atomic_fetch_add_explicit(&d->out_frames, frames, memory_order_relaxed);
		atomic_fetch_add_explicit(&d->cycles, 1, memory_order_relaxed);
	}
	return 0;
}

/*
 * Every REPORT_S seconds, for each device whose I/O runs and has moved since the last look: how many
 * frames a second the HAL gave it, measured on the kernel's clock. A HAL that believes in another host
 * clock frequency shows here as a rate away from 48000.
 */
static void *
reporter(__unused void *unused)
{
	char lines[DEVICES][LINE];

	for (;;) {
		sleep(REPORT_S);
		pthread_mutex_lock(&lock);
		for (unsigned i = 0; i < DEVICES; i++) {
			struct device *d = &devices[i];
			UInt64 now = mach_absolute_time();
			UInt64 out = atomic_load_explicit(&d->out_frames, memory_order_relaxed);
			UInt64 in = atomic_load_explicit(&d->in_frames, memory_order_relaxed);
			double s = seconds(now - d->reported_at);

			lines[i][0] = '\0';
			if (d->running == 0 || (out == d->reported_out && in == d->reported_in) || s <= 0) {
				continue;
			}
			snprintf(lines[i], LINE, "%s: over the last %.1f s %.0f frames a second out, %.0f in (I/O run %u going for %.1f s)",
			    d->name, s, (double)(out - d->reported_out) / s, (double)(in - d->reported_in) / s, d->runs,
			    seconds(now - d->anchor));
			d->reported_out = out;
			d->reported_in = in;
			d->reported_at = now;
		}
		pthread_mutex_unlock(&lock);
		for (unsigned i = 0; i < DEVICES; i++) {
			if (lines[i][0] != '\0') {
				say("%s", lines[i]);
			}
		}
	}
	return NULL;
}

static AudioServerPlugInDriverInterface interface = {
	NULL, query_interface, add_ref, release, initialize, create_device, destroy_device, device_client, device_client,
	configuration_change, configuration_change, has_property, is_property_settable, get_property_data_size,
	get_property_data, set_property_data, start_io, stop_io, get_zero_time_stamp, will_do_io_operation,
	begin_end_io_operation, do_io_operation, begin_end_io_operation,
};
static AudioServerPlugInDriverInterface *driver = &interface;

/* The factory named in the bundle's Info.plist. */
__attribute__((visibility("default"))) void *AndromedaAudio_Create(CFAllocatorRef allocator, CFUUIDRef type);

void *
AndromedaAudio_Create(__unused CFAllocatorRef allocator, CFUUIDRef type)
{
	return type != NULL && CFEqual(type, kAudioServerPlugInTypeUUID) ? &driver : NULL;
}
