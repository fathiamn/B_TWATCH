/****************************************************************************
 *   Copyright  2021  Guilhem Bonnefille <guilhem.bonnefille@gmail.com>
 ****************************************************************************/

#ifndef _BLESTEPCTL_H
    #define _BLESTEPCTL_H

    #include "hardware/blectl.h"

    void blestepctl_setup( void );
    void blestepctl_update( bool force = false );

    // Live hike updates (sent every 5s during active session)
    void blestepctl_set_hike(uint32_t stp, uint32_t dist, uint32_t dur);
    void blestepctl_update_hike(bool force);

    // ★ Arms/disarms live sender — call true on START, false on STOP
    void blestepctl_set_hike_active(bool active);

    // ★ Sends one final packet when session ends — Pi stores this to SQLite
    void blestepctl_send_hike_end(uint32_t stp, uint32_t dist, uint32_t dur);

#endif // _BLESTEPCTL_H
