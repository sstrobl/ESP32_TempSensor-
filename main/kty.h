#pragma once

#include <stdbool.h>

/**
 * Wandelt den KTY-Widerstand (Ohm) in Grad Celsius um.
 * Liefert false, wenn der Widerstand ausserhalb der Kennlinie liegt
 * (Kurzschluss / Leitungsbruch).
 */
bool kty_ohms_to_celsius(float ohms, int r25_ohms, float *temp_c);
