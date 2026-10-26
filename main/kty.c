#include <stddef.h>
#include "kty.h"

typedef struct {
    float temp_c;
    float ohms;     /* Referenz: KTY81-1xx mit 1000 Ohm bei 25 Grad C */
} kty_point_t;

/* Kennlinie KTY81-1xx (Philips/NXP Datenblatt, Naeherung) */
static const kty_point_t kty_table[] = {
    { -55,  490 }, { -50,  515 }, { -40,  567 }, { -30,  624 },
    { -20,  684 }, { -10,  747 }, {   0,  815 }, {  10,  886 },
    {  20,  961 }, {  25, 1000 }, {  30, 1040 }, {  40, 1122 },
    {  50, 1209 }, {  60, 1299 }, {  70, 1392 }, {  80, 1490 },
    {  90, 1591 }, { 100, 1696 }, { 110, 1805 }, { 120, 1915 },
    { 125, 1970 }, { 130, 2023 }, { 140, 2124 }, { 150, 2211 },
};

#define KTY_TABLE_LEN (sizeof(kty_table) / sizeof(kty_table[0]))

bool kty_ohms_to_celsius(float ohms, int r25_ohms, float *temp_c)
{
    /* Auf die 1k-Referenztabelle normieren (2k-Typen etc.) */
    const float r = ohms * 1000.0f / (float)r25_ohms;

    if (r < kty_table[0].ohms || r > kty_table[KTY_TABLE_LEN - 1].ohms) {
        return false;
    }
    for (size_t i = 1; i < KTY_TABLE_LEN; i++) {
        if (r <= kty_table[i].ohms) {
            const kty_point_t *a = &kty_table[i - 1];
            const kty_point_t *b = &kty_table[i];
            const float f = (r - a->ohms) / (b->ohms - a->ohms);
            *temp_c = a->temp_c + f * (b->temp_c - a->temp_c);
            return true;
        }
    }
    return false;
}
