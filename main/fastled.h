#pragma once

#include "SmartLeds.h"
#include <stdint.h>

/// Force a variable reference to avoid compiler over-optimization.
/// Sometimes the compiler will do clever things to reduce
/// code size that result in a net slowdown, if it thinks that
/// a variable is not used in a certain location.
/// This macro does its best to convince the compiler that
/// the variable is used in this location, to help control
/// code motion and de-duplication that would result in a slowdown.
#define FORCE_REFERENCE(var) asm volatile("" : : "r"(var))

/// @cond
#define K255 255
#define K171 171
#define K170 170
#define K85 85
/// @endcond

static uint8_t scale8(uint8_t i, uint8_t scale) {
    return ((uint16_t)i * (uint16_t)(scale)) >> 8;
}

static uint8_t scale8_video(uint8_t i, uint8_t scale) {
    uint8_t j = (((int)i * (int)scale) >> 8) + ((i && scale) ? 1 : 0);
    return j;
}

static void hsv2rgb_rainbow(const Hsv& hsv, Apa102::ApaRgb& rgb) {
    // Yellow has a higher inherent brightness than
    // any other color; 'pure' yellow is perceived to
    // be 93% as bright as white.  In order to make
    // yellow appear the correct relative brightness,
    // it has to be rendered brighter than all other
    // colors.
    // Level Y1 is a moderate boost, the default.
    // Level Y2 is a strong boost.
    const uint8_t Y1 = 1;
    const uint8_t Y2 = 0;

    // G2: Whether to divide all greens by two.
    // Depends GREATLY on your particular LEDs
    const uint8_t G2 = 0;

    // Gscale: what to scale green down by.
    // Depends GREATLY on your particular LEDs
    const uint8_t Gscale = 0;

    uint8_t hue = hsv.h;
    uint8_t sat = hsv.s;
    uint8_t val = hsv.v;

    uint8_t offset = hue & 0x1F; // 0..31

    // offset8 = offset * 8
    uint8_t offset8 = offset;
    {
        // On ARM and other non-AVR platforms, we just shift 3.
        offset8 <<= 3;
    }

    uint8_t third = scale8(offset8, (256 / 3)); // max = 85

    uint8_t r, g, b;

    if (!(hue & 0x80)) {
        // 0XX
        if (!(hue & 0x40)) {
            // 00X
            //section 0-1
            if (!(hue & 0x20)) {
                // 000
                //case 0: // R -> O
                r = K255 - third;
                g = third;
                b = 0;
                FORCE_REFERENCE(b);
            } else {
                // 001
                //case 1: // O -> Y
                if (Y1) {
                    r = K171;
                    g = K85 + third;
                    b = 0;
                    FORCE_REFERENCE(b);
                }
                if (Y2) {
                    r = K170 + third;
                    //uint8_t twothirds = (third << 1);
                    uint8_t twothirds = scale8(offset8, ((256 * 2) / 3)); // max=170
                    g = K85 + twothirds;
                    b = 0;
                    FORCE_REFERENCE(b);
                }
            }
        } else {
            //01X
            // section 2-3
            if (!(hue & 0x20)) {
                // 010
                //case 2: // Y -> G
                if (Y1) {
                    //uint8_t twothirds = (third << 1);
                    uint8_t twothirds = scale8(offset8, ((256 * 2) / 3)); // max=170
                    r = K171 - twothirds;
                    g = K170 + third;
                    b = 0;
                    FORCE_REFERENCE(b);
                }
                if (Y2) {
                    r = K255 - offset8;
                    g = K255;
                    b = 0;
                    FORCE_REFERENCE(b);
                }
            } else {
                // 011
                // case 3: // G -> A
                r = 0;
                FORCE_REFERENCE(r);
                g = K255 - third;
                b = third;
            }
        }
    } else {
        // section 4-7
        // 1XX
        if (!(hue & 0x40)) {
            // 10X
            if (!(hue & 0x20)) {
                // 100
                //case 4: // A -> B
                r = 0;
                FORCE_REFERENCE(r);
                //uint8_t twothirds = (third << 1);
                uint8_t twothirds = scale8(offset8, ((256 * 2) / 3)); // max=170
                g = K171 - twothirds; //K170?
                b = K85 + twothirds;

            } else {
                // 101
                //case 5: // B -> P
                r = third;
                g = 0;
                FORCE_REFERENCE(g);
                b = K255 - third;
            }
        } else {
            if (!(hue & 0x20)) {
                // 110
                //case 6: // P -- K
                r = K85 + third;
                g = 0;
                FORCE_REFERENCE(g);
                b = K171 - third;

            } else {
                // 111
                //case 7: // K -> R
                r = K170 + third;
                g = 0;
                FORCE_REFERENCE(g);
                b = K85 - third;
            }
        }
    }

    // This is one of the good places to scale the green down,
    // although the client can scale green down as well.
    if (G2)
        g = g >> 1;
    if (Gscale)
        g = scale8_video(g, Gscale);

    // Scale down colors if we're desaturated at all
    // and add the brightness_floor to r, g, and b.
    if (sat != 255) {
        if (sat == 0) {
            r = 255;
            b = 255;
            g = 255;
        } else {
            uint8_t desat = 255 - sat;
            desat = scale8_video(desat, desat);

            uint8_t satscale = 255 - desat;
            //satscale = sat; // uncomment to revert to pre-2021 saturation behavior

            //nscale8x3_video( r, g, b, sat);
            if (r)
                r = scale8(r, satscale) + 1;
            if (g)
                g = scale8(g, satscale) + 1;
            if (b)
                b = scale8(b, satscale) + 1;
            uint8_t brightness_floor = desat;
            r += brightness_floor;
            g += brightness_floor;
            b += brightness_floor;
        }
    }

    // Now scale everything down if we're at value < 255.
    if (val != 255) {

        val = scale8_video(val, val);
        if (val == 0) {
            r = 0;
            g = 0;
            b = 0;
        } else {
            // nscale8x3_video( r, g, b, val);
            if (r)
                r = scale8(r, val) + 1;
            if (g)
                g = scale8(g, val) + 1;
            if (b)
                b = scale8(b, val) + 1;
        }
    }

    // Here we have the old AVR "missing std X+n" problem again
    // It turns out that fixing it winds up costing more than
    // not fixing it.
    // To paraphrase Dr Bronner, profile! profile! profile!
    //asm volatile(  ""  :  :  : "r26", "r27" );
    //asm volatile (" movw r30, r26 \n" : : : "r30", "r31");
    rgb.r = r;
    rgb.g = g;
    rgb.b = b;
}
