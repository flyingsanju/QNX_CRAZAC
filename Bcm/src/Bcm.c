/* Bcm - Body Control Module
 * Sends safety, telemetry and diagnostics at its own rates; all the logic is in sender_core.h. */
#define ECU_LABEL  "bcm"
#define SAFETY_MS    200     /* safety frame every 200 ms */
#define TELEMETRY_MS 100     /* telemetry every 100 ms */
#define DIAG_MS      50     /* diagnostics every 50 ms */
#include "sender_core.h"
