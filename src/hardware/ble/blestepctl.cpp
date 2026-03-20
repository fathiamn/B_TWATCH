/****************************************************************************
 *   Copyright  2021  Guilhem Bonnefille <guilhem.bonnefille@gmail.com>
 ****************************************************************************/

#include "config.h"
#include "blestepctl.h"
#include "gadgetbridge.h"

#include "hardware/blectl.h"
#include "hardware/motion.h"
#include "hardware/ble/bleupdater.h"
#include "utils/bluejsonrequest.h"

#ifdef NATIVE_64BIT
  #include "utils/logging.h"
#else
  #include <Arduino.h>
#endif

/* -------------------------------------------------------------------------- */
/*  HIKE DATA — live updates during session                                    */
/* -------------------------------------------------------------------------- */

struct HikeData {
    uint32_t stp;
    uint32_t dist;
    uint32_t dur;
    // ★ active flag — controls whether live packets are sent
    bool active;
};

static inline bool operator==(const HikeData& a, const HikeData& b) {
    return a.stp == b.stp && a.dist == b.dist && a.dur == b.dur && a.active == b.active;
}
static inline bool operator!=(const HikeData& a, const HikeData& b) {
    return !(a == b);
}

class HikeBleUpdater : public BleUpdater<HikeData> {
public:
    HikeBleUpdater() : BleUpdater(5) {}

protected:
    bool notify(HikeData d) override {
        // ★ Skip entirely when no session is running
        if (!d.active) {
            log_d("Hike notify skipped: session not active");
            return true;
        }

        // ★ Buffer 160 bytes — comfortable headroom for uint32_t fields
        char msg[160] = {0};
        int written = snprintf(msg, sizeof(msg),
            "\r\n{\"t\":\"hike\",\"stp\":%lu,\"dst\":%lu,\"dur\":%lu}\r\n",
            (unsigned long)d.stp,
            (unsigned long)d.dist,
            (unsigned long)d.dur);

        // ★ Detect truncation before sending a malformed packet
        if (written < 0 || written >= (int)sizeof(msg)) {
            log_e("Hike msg truncated! written=%d buf=%d", written, (int)sizeof(msg));
            return false;
        }

        bool ret = gadgetbridge_send_msg(msg);
        log_d("Hike live: stp=%lu dst=%lu dur=%lu => %d",
              (unsigned long)d.stp, (unsigned long)d.dist, (unsigned long)d.dur, ret);
        return ret;
    }
};

static HikeBleUpdater hike_ble_updater;
// ★ Initialise active=false — nothing sends until session starts
static HikeData hike_data = {0, 0, 0, false};

/* -------------------------------------------------------------------------- */
/*  PUBLIC API — live updates                                                  */
/* -------------------------------------------------------------------------- */

void blestepctl_set_hike(uint32_t stp, uint32_t dist, uint32_t dur) {
    hike_data.stp  = stp;
    hike_data.dist = dist;
    hike_data.dur  = dur;
}

// ★ Call with true on session START, false on session STOP
void blestepctl_set_hike_active(bool active) {
    hike_data.active = active;
}

void blestepctl_update_hike(bool force) {
    hike_ble_updater.update(hike_data, force);
}

/* -------------------------------------------------------------------------- */
/*  ★ NEW — hike_end: one-shot final packet sent when session stops           */
/*  Pi uses "t":"hike_end" to know this is a completed session to store.      */
/* -------------------------------------------------------------------------- */

void blestepctl_send_hike_end(uint32_t stp, uint32_t dist, uint32_t dur) {
    char msg[160] = {0};
    int written = snprintf(msg, sizeof(msg),
        "\r\n{\"t\":\"hike_end\",\"stp\":%lu,\"dst\":%lu,\"dur\":%lu}\r\n",
        (unsigned long)stp,
        (unsigned long)dist,
        (unsigned long)dur);

    if (written < 0 || written >= (int)sizeof(msg)) {
        log_e("hike_end msg truncated! written=%d", written);
        return;
    }

    (void)gadgetbridge_send_msg(msg);
    log_d("hike_end sent: stp=%lu dst=%lu dur=%lu => %d",
          (unsigned long)stp, (unsigned long)dist, (unsigned long)dur, ret);
}

/* -------------------------------------------------------------------------- */
/*  STEP COUNTER — existing Gadgetbridge "act" delta message                   */
/* -------------------------------------------------------------------------- */

class StepcounterBleUpdater : public BleUpdater<int32_t> {
public:
    StepcounterBleUpdater() : BleUpdater(30) {}

protected:
    bool notify(int32_t stepcounter_val) override {
        uint32_t delta = stepcounter_val < last_value
                         ? (uint32_t)stepcounter_val
                         : (uint32_t)(stepcounter_val - last_value);

        char msg[64] = {0};
        int written = snprintf(msg, sizeof(msg),
            "\r\n{\"t\":\"act\", \"stp\":%lu}\r\n",
            (unsigned long)delta);

        if (written < 0 || written >= (int)sizeof(msg)) {
            log_e("Act msg truncated! written=%d", written);
            return false;
        }

        bool ret = gadgetbridge_send_msg(msg);
        log_d("Act: new=%d last=%d delta=%lu => %d",
              (int)stepcounter_val, (int)last_value, (unsigned long)delta, ret);
        return ret;
    }
};

static StepcounterBleUpdater stepcounter_ble_updater;
static int32_t stepcounter = 0;

/* -------------------------------------------------------------------------- */
/*  CALLBACKS                                                                  */
/* -------------------------------------------------------------------------- */

static bool blestepctl_bma_event_cb(EventBits_t event, void *arg);
static bool blestepctl_bluetooth_event_cb(EventBits_t event, void *arg);

void blestepctl_setup(void) {
    bma_register_cb(BMACTL_STEPCOUNTER, blestepctl_bma_event_cb, "ble step counter");
    gadgetbridge_register_cb(GADGETBRIDGE_CONNECT | GADGETBRIDGE_JSON_MSG,
                             blestepctl_bluetooth_event_cb,
                             "ble step counter");
}

static bool blestepctl_bma_event_cb(EventBits_t event, void *arg) {
    if (event == BMACTL_STEPCOUNTER) {
        stepcounter = *(int32_t *)arg;
        stepcounter_ble_updater.update(stepcounter);
        return true;
    }
    return false;
}

static bool blestepctl_bluetooth_event_cb(EventBits_t event, void *arg) {
    switch (event) {
    case GADGETBRIDGE_CONNECT:
        stepcounter_ble_updater.update(stepcounter, true);
        // ★ Only force-send hike on connect if session is currently active
        if (hike_data.active) {
            hike_ble_updater.update(hike_data, true);
        }
        return true;

    case GADGETBRIDGE_JSON_MSG: {
        BluetoothJsonRequest &request = *(BluetoothJsonRequest *)arg;

        if (request.isEqualKeyValue("t", "act") &&
            request.containsKey("stp") && request["stp"].as<bool>() &&
            request.containsKey("int")) {
            time_t timeout = request["int"].as<time_t>();
            stepcounter_ble_updater.setTimeout(timeout);
        }

        if (request.isEqualKeyValue("t", "hike") && request.containsKey("int")) {
            time_t timeout = request["int"].as<time_t>();
            hike_ble_updater.setTimeout(timeout);
        }

        return true;
    }
    default:
        return false;
    }
}

void blestepctl_update(bool force) {
    stepcounter_ble_updater.update(stepcounter, force);
}
