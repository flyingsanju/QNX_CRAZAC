/* Chassis - Chassis/Safety Control Module
 * Sends safety, telemetry and diagnostics at its own rates; all the logic is in sender_core.h. */
#define ECU_LABEL  "chassis"
#define SAFETY_MS    50     /* safety frame every 50 ms */
#define TELEMETRY_MS 50     /* telemetry every 50 ms */
#define DIAG_MS      100     /* diagnostics every 100 ms */
#include "sender_core.h"
