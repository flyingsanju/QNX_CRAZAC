/* Powertrain - Powertrain/Engine Control Unit
 * Sends safety, telemetry and diagnostics at its own rates; all the logic is in sender_core.h. */
#define ECU_LABEL  "powertrain"
#define SAFETY_MS    100     /* safety frame every 100 ms */
#define TELEMETRY_MS 10     /* telemetry every 10 ms */
#define DIAG_MS      200     /* diagnostics every 200 ms */
#include "sender_core.h"
