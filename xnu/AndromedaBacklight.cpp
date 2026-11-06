/*
 * Mi Mix 3 5G (andromeda): a minimal AppleARMBacklight service for iOS's
 * CoreBrightness (backboardd), patch 0093. Created only with andrbl=2, by
 * bsd/dev/andromeda_backlight.c once a DCS 0x51 write has worked.
 *
 * What backboardd looks for, from our boot log: a service matching
 * IOPropertyMatch {backlight-control = true} (also as a matching
 * notification) and the path IODeviceTree:/backlight. What it then expects,
 * from two other ports (facts only, no code copied; see CREDITS.md):
 *   - MakrSas/ios-topaz (iOS 16.7.16 20H392, the same userland; patches
 *     0104/0105/0110): CoreBrightness takes the backlight only if the service
 *     conforms to the class AppleARMBacklight; it sets "brightness-nits" on it
 *     (and IOMFBBrightnessLevel, nits in 16.16, on the framebuffer, which this
 *     file does not handle); with a backlight service it also runs digital
 *     dimming, a colour filter over every frame, which made their CPU-rendered
 *     UI drop below 1 fps, so they made the service opt-in.
 *   - UniXNU-sheng AppleARMBacklight.cpp (UniXNU contributors, iPadOS 27,
 *     BSD-2-Clause-Patent): CoreBrightness reads min/max from
 *     IODisplayParameters {brightness {min, max, value}} and sets
 *     {brightness, FadePeriod} through setProperties; without the
 *     AppleARMBacklight conformance a published backlight-control blocked the
 *     display server.
 * Our kernel is built from source without Apple's AppleARMPlatform kext, so
 * the class name AppleARMBacklight is free here and gives the conformance.
 * The ios-sweet kernel has no sandbox policy for IOKit properties.
 *
 * setProperties never waits for the panel: it hands the newest level to the
 * sender thread (andromeda_backlight_request) and returns. Every key set is
 * logged (the first 48), so the first boot shows what CoreBrightness sends.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>

extern "C" int andromeda_backlight_request(unsigned level);
extern "C" int andromeda_backlight_publish(unsigned level, unsigned max_level, unsigned max_nits);

class AppleARMBacklight : public IOService {
	OSDeclareDefaultStructors(AppleARMBacklight);
public:
	unsigned fMaxLevel;
	unsigned fMaxNits;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	void publishLevel(unsigned level);
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

void
AppleARMBacklight::publishLevel(unsigned level)
{
	OSDictionary *brightness = OSDictionary::withCapacity(3);
	OSDictionary *parameters = OSDictionary::withCapacity(1);
	OSNumber *min = OSNumber::withNumber(0ULL, 32);
	OSNumber *max = OSNumber::withNumber((unsigned long long)fMaxLevel, 32);
	OSNumber *value = OSNumber::withNumber((unsigned long long)level, 32);
	if (brightness && parameters && min && max && value) {
		brightness->setObject("min", min);
		brightness->setObject("max", max);
		brightness->setObject("value", value);
		parameters->setObject("brightness", brightness);
		setProperty("IODisplayParameters", parameters);
	}
	OSSafeReleaseNULL(min);
	OSSafeReleaseNULL(max);
	OSSafeReleaseNULL(value);
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(parameters);
	setProperty("brightness-nits", (unsigned long long)level * fMaxNits / fMaxLevel, 32);
}

IOReturn
AppleARMBacklight::setProperties(OSObject *properties)
{
	static unsigned logged;
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	if (dict == NULL || fMaxLevel == 0 || fMaxNits == 0) {
		return kIOReturnBadArgument;
	}
	OSCollectionIterator *it = OSCollectionIterator::withCollection(dict);
	const OSSymbol *key;
	while (it != NULL && (key = OSDynamicCast(OSSymbol, it->getNextObject())) != NULL) {
		OSObject *object = dict->getObject(key);
		OSNumber *number = OSDynamicCast(OSNumber, object);
		if (logged < 48) {
			++logged;
			IOLog("ANDROMEDA BL: CoreBrightness set %s = %s %llu\n", key->getCStringNoCopy(),
			    object ? object->getMetaClass()->getClassName() : "null",
			    number ? (unsigned long long)number->unsigned64BitValue() : 0ULL);
		}
	}
	OSSafeReleaseNULL(it);

	OSNumber *brightness = OSDynamicCast(OSNumber, dict->getObject("brightness"));
	OSNumber *nits = OSDynamicCast(OSNumber, dict->getObject("brightness-nits"));
	uint64_t level;
	if (brightness != NULL) {
		level = brightness->unsigned64BitValue();       /* IODisplayParameters units */
	} else if (nits != NULL) {
		uint64_t value = nits->unsigned64BitValue();
		if (value >= 0x10000) {
			value >>= 16;                           /* 16.16 nits */
		}
		level = value * fMaxLevel / fMaxNits;
	} else {
		return kIOReturnSuccess;                        /* e.g. FadePeriod alone */
	}
	if (level > fMaxLevel) {
		level = fMaxLevel;
	}
	int err = andromeda_backlight_request((unsigned)level);
	if (err == 0) {
		publishLevel((unsigned)level);
	}
	return err == 0 ? kIOReturnSuccess : kIOReturnNotReady;
}

/* Called once from andromeda_backlight.c's thread; 0 on success. */
extern "C" int
andromeda_backlight_publish(unsigned level, unsigned max_level, unsigned max_nits)
{
	if (max_level == 0 || max_nits == 0 || level > max_level) {
		return -1;
	}
	AppleARMBacklight *backlight = OSTypeAlloc(AppleARMBacklight);
	if (backlight == NULL || !backlight->init()) {
		OSSafeReleaseNULL(backlight);
		return -1;
	}
	backlight->fMaxLevel = max_level;
	backlight->fMaxNits = max_nits;
	backlight->setProperty("backlight-control", true);
	backlight->setProperty("max-nits", (unsigned long long)max_nits, 32);
	backlight->publishLevel(level);
	/* Under the framebuffer (as ios-topaz), else under the platform expert. */
	IOService *parent = NULL;
	OSDictionary *match = IOService::serviceMatching("AppleCLCD");
	if (match != NULL) {
		parent = IOService::copyMatchingService(match);
		match->release();
	}
	bool attached = backlight->attach(parent != NULL ? parent : (IOService *)IOService::getPlatform());
	IOLog("ANDROMEDA BL: AppleARMBacklight under %s: %s\n", parent != NULL ? "AppleCLCD" : "platform",
	    attached ? "attached" : "ATTACH FAILED");
	OSSafeReleaseNULL(parent);
	if (!attached) {
		backlight->release();
		return -1;
	}
	backlight->registerService();
	backlight->release();           /* the registry keeps it */
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
