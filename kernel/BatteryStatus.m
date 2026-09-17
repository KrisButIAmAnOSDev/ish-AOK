//
//  BatteryStatus.m
//  iSH-AOK
//
//  Created by Michael Miller on 9/10/23.
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <os/lock.h>
#include <string.h>
#import "BatteryStatus.h"

// The one copy of the host's state that the kernel reads.
//
// It replaces printBatteryStatus(), which asked UIDevice afresh on every read
// and returned -[NSString UTF8String] of a temporary string. Two things were
// wrong with that. The pointer belonged to an autoreleased object that nothing
// kept alive once the function had returned, and fs/proc formatted from it
// afterwards. And UIDevice is main-thread-only, while the read came from
// whichever guest thread opened the file.
//
// So the main queue is now the only writer. It takes a reading at start and
// again whenever iOS says something changed. A reader holds the lock only long
// enough to copy the values out, and formats from its own copy.
static os_unfair_lock host_status_lock = OS_UNFAIR_LOCK_INIT;

// Big enough for any IANA name: the longest, America/Argentina/ComodRivadavia,
// is 32 bytes.
#define HOST_TIMEZONE_NAME_MAX 128

static struct {
    struct host_battery_status battery;
    enum host_thermal_state thermal;
    char zone_name[HOST_TIMEZONE_NAME_MAX];
} host_status = {
    // Until the first reading: nothing known. The guest boots after that
    // reading (ISHHostStatusStart is called first), so this is only ever seen
    // by something that ran before the app finished launching.
    .battery = {.state = HOST_BATTERY_UNKNOWN, .level = -1, .low_power_mode = -1},
    .thermal = HOST_THERMAL_UNKNOWN,
    .zone_name = "",
};

static enum host_battery_state host_battery_state_from(UIDeviceBatteryState state) {
    switch (state) {
        case UIDeviceBatteryStateUnplugged:
            return HOST_BATTERY_UNPLUGGED;
        case UIDeviceBatteryStateCharging:
            return HOST_BATTERY_CHARGING;
        case UIDeviceBatteryStateFull:
            return HOST_BATTERY_FULL;
        case UIDeviceBatteryStateUnknown:
        default:
            return HOST_BATTERY_UNKNOWN;
    }
}

static enum host_thermal_state host_thermal_state_from(NSProcessInfoThermalState state) {
    switch (state) {
        case NSProcessInfoThermalStateNominal:
            return HOST_THERMAL_NOMINAL;
        case NSProcessInfoThermalStateFair:
            return HOST_THERMAL_FAIR;
        case NSProcessInfoThermalStateSerious:
            return HOST_THERMAL_SERIOUS;
        case NSProcessInfoThermalStateCritical:
            return HOST_THERMAL_CRITICAL;
        default:
            // A state newer than this code: say so rather than guess which of
            // the four it is closest to.
            return HOST_THERMAL_UNKNOWN;
    }
}

// Main thread only.
static void host_status_refresh(void) {
    UIDevice *device = UIDevice.currentDevice;
    // Off, UIDevice answers "unknown" and -1 whatever the battery is doing.
    device.batteryMonitoringEnabled = YES;
    NSProcessInfo *process = NSProcessInfo.processInfo;

    struct host_battery_status battery = {
        .state = host_battery_state_from(device.batteryState),
        .level = device.batteryLevel,
        .low_power_mode = process.isLowPowerModeEnabled ? 1 : 0,
    };
    enum host_thermal_state thermal = host_thermal_state_from(process.thermalState);

    // Copied out of the NSString here, on the thread that owns it, so the
    // kernel never holds a pointer into an object.
    char zone_name[HOST_TIMEZONE_NAME_MAX] = "";
    NSString *name = NSTimeZone.localTimeZone.name;
    if (name == nil || ![name getCString:zone_name maxLength:sizeof(zone_name) encoding:NSUTF8StringEncoding])
        zone_name[0] = '\0';

    os_unfair_lock_lock(&host_status_lock);
    host_status.battery = battery;
    host_status.thermal = thermal;
    memcpy(host_status.zone_name, zone_name, sizeof(zone_name));
    os_unfair_lock_unlock(&host_status_lock);
}

void ISHHostStatusStart(void) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{
            ISHHostStatusStart();
        });
        return;
    }
    static BOOL started;
    if (started)
        return;
    started = YES;

    host_status_refresh();

    // Every one of these re-reads everything: a reading is a handful of
    // property reads, and one path is easier to get right than six. Some are
    // posted on whichever thread noticed the change, hence the main queue.
    // Becoming active is the catch-all for whatever changed while the app was
    // suspended.
    NSArray<NSNotificationName> *changes = @[
        UIDeviceBatteryLevelDidChangeNotification,
        UIDeviceBatteryStateDidChangeNotification,
        NSProcessInfoPowerStateDidChangeNotification,
        NSProcessInfoThermalStateDidChangeNotification,
        NSSystemTimeZoneDidChangeNotification,
        UIApplicationDidBecomeActiveNotification,
    ];
    for (NSNotificationName change in changes) {
        [NSNotificationCenter.defaultCenter addObserverForName:change
                                                        object:nil
                                                         queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *note) {
            // The system zone is cached per process until told otherwise.
            if ([note.name isEqualToString:NSSystemTimeZoneDidChangeNotification])
                [NSTimeZone resetSystemTimeZone];
            host_status_refresh();
        }];
    }
}

void hostBatteryStatus(struct host_battery_status *out) {
    if (out == NULL)
        return;
    os_unfair_lock_lock(&host_status_lock);
    *out = host_status.battery;
    os_unfair_lock_unlock(&host_status_lock);
}

enum host_thermal_state hostThermalState(void) {
    os_unfair_lock_lock(&host_status_lock);
    enum host_thermal_state thermal = host_status.thermal;
    os_unfair_lock_unlock(&host_status_lock);
    return thermal;
}

bool hostTimeZoneName(char *buf, size_t size) {
    if (buf == NULL || size == 0)
        return false;
    os_unfair_lock_lock(&host_status_lock);
    size_t len = strlen(host_status.zone_name);
    bool fits = len > 0 && len < size;
    if (fits)
        memcpy(buf, host_status.zone_name, len + 1);
    os_unfair_lock_unlock(&host_status_lock);
    if (!fits)
        buf[0] = '\0';
    return fits;
}
