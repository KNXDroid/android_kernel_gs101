#ifndef _LINUX_DISPLAY_FPS_H
#define _LINUX_DISPLAY_FPS_H

#include <linux/types.h>

/**
 * @brief Get the current average display FPS.
 *
 * Provides the average Frames Per Second of the primary display,
 * calculated over a 3-second interval.
 *
 * @return u32 The approximate average FPS. Returns 0 if the display is off.
 */
u32 display_drm_get_average_fps(void);

#endif /* _LINUX_DISPLAY_FPS_H */

