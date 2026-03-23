/*
© 2026 Monterro · Fathia & Bintang. All rights reserved.
*/
#ifndef _CALENDAR_CREATE_H
    #define _CALENDAR_CREATE_H

    #
    #define CALENDAR_CREATE_INFO_LOG               log_i
    #define CALENDAR_CREATE_DEBUG_LOG              log_d
    #define CALENDAR_CREATE_ERROR_LOG              log_e
    /**
     * @brief setup calendar create tile
     */
    void calendar_create_setup( void );
    /**
     * @brief get calendar overview tile number
     * 
     * @return  calendar overview tile number
     */
    uint32_t calendar_create_get_tile( void );
    /**
     * @brief set year, month and day to create a date entry
     * 
     * @param year      create year
     * @param month     create month
     * @param day       create day
     */
    void calendar_create_set_date( int year, int month, int day );
    /**
     * @brief set hour and min to create a date entry
     * 
     * @param year      create year
     * @param month     create month
     * @param day       create day
     */
    void calendar_create_set_time( int hour, int min );
    /**
     * @brief set the content is a date is exist on given year, month, day, hour and min
     */
    void calendar_create_set_content( void );
    /**
     * @brief clear content
     */
    void calendar_create_clear_content( void );

#endif // _CALENDAR_CREATE_H
