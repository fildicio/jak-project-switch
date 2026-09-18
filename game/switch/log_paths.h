#pragma once

/*!
 * Where the Switch port's diagnostic files live.
 *
 * These used to be fixed at the SD card root ("sdmc:/gk_boot_log.txt" and friends). With one
 * NRO per game (SWITCH_GAME in the root CMakeLists) that is actively harmful: every game
 * *appends* to the same files, so a jak1 run and a jak2 run interleave in one boot log with
 * nothing to tell them apart, and the truncate-on-boot stdout sink belongs to whichever game
 * booted last. A Jak 2 post-mortem was already misread this way -- jak1's VAG/DGO lines in the
 * shared log looked like the jak2 process loading jak1's data.
 *
 * Each install already owns sdmc:/switch/<game>/ (the NRO, data/, saves and GOAL logs all live
 * there), so put these there too. Without SWITCH_GAME_NAME, keep the original root paths.
 */
#ifdef SWITCH_GAME_NAME
#define SWITCH_LOG_PATH(file_name) "sdmc:/switch/" SWITCH_GAME_NAME "/" file_name
#else
#define SWITCH_LOG_PATH(file_name) "sdmc:/" file_name
#endif
