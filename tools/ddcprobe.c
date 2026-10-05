/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Laine Jones */
/*
 * ddcprobe - can the monitor be asked what it supports (DDC2B / EDID) on a
 * Cirrus card (Picasso II, GBAPII++)?
 *
 * The GD542x/543x have two general-purpose pins on sequencer register SR08
 * that boards may wire to the VGA connector's DDC lines (pins 15 = clock,
 * 12 = data):
 *     bit 0  clock out      bit 2  clock in
 *     bit 1  data out       bit 7  data in
 *     bit 6  enable the pins
 * This bit-bangs an I2C read of the monitor's EDID (device $A0, 128 bytes)
 * and says what came back. It writes SR08 only and puts it back; the
 * picture is not touched. If the board doesn't wire the pins the lines
 * read stuck and nothing answers.
 */
#include <exec/types.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <stdio.h>

struct ExpansionBase *ExpansionBase;

static volatile UBYTE *regs;

#define SEQ_I 0x3c4
#define SEQ_D 0x3c5

static UBYTE out;                        /* SR08 value being driven */

static void wseq(UBYTE i, UBYTE v)
{
    Forbid();                            /* PrismD's pointer code uses the index too */
    regs[SEQ_I] = i;
    regs[SEQ_D] = v;
    Permit();
}

static UBYTE rseq(UBYTE i)
{
    UBYTE v;
    Forbid();
    regs[SEQ_I] = i;
    v = regs[SEQ_D];
    Permit();
    return v;
}

static void pause(void)
{
    volatile ULONG n;
    for (n = 0; n < 60; n++) ;           /* well over 5 us on a 50 MHz 68030 */
}

static void set(BOOL scl, BOOL sda)
{
    out = (out & ~3) | (scl ? 1 : 0) | (sda ? 2 : 0);
    wseq(0x08, out);
    pause();
}

static BOOL scl_in(void) { return (rseq(0x08) & 0x04) != 0; }
static BOOL sda_in(void) { return (rseq(0x08) & 0x80) != 0; }

static void i2c_start(void) { set(1, 1); set(1, 0); set(0, 0); }
static void i2c_stop(void)  { set(0, 0); set(1, 0); set(1, 1); }

/* send a byte; TRUE if the device acknowledged */
static BOOL i2c_write(UBYTE b)
{
    int i;
    BOOL ack;
    for (i = 7; i >= 0; i--) {
        BOOL bit = (b >> i) & 1;
        set(0, bit); set(1, bit); set(0, bit);
    }
    set(0, 1); set(1, 1);
    ack = !sda_in();
    set(0, 1);
    return ack;
}

static UBYTE i2c_read(BOOL last)
{
    UBYTE b = 0;
    int i;
    for (i = 0; i < 8; i++) {
        set(0, 1); set(1, 1);
        b = (b << 1) | (sda_in() ? 1 : 0);
        set(0, 1);
    }
    set(0, last ? 1 : 0); set(1, last ? 1 : 0); set(0, 1);   /* ack / nak */
    return b;
}

int main(void)
{
    static const UBYTE hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    struct ConfigDev *cd;
    UBYTE saved, edid[128], sum = 0;
    BOOL c0, d0, c1, d1, ackW, ackR = FALSE, ok = TRUE;
    int i;

    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37)))
        return 20;
    if (!(cd = FindConfigDev(NULL, 2167, 17)) && !(cd = FindConfigDev(NULL, 2167, 12))) {
        printf("ddcprobe: no Picasso II / GBAPII++\n");
        return 5;
    }
    regs = (volatile UBYTE *)cd->cd_BoardAddr;
    wseq(0x06, 0x12);                    /* unlock the extended registers */
    saved = rseq(0x08);
    printf("ddcprobe: registers at $%08lx, SR08 was $%02x\n", (unsigned long)regs, saved);

    /* raw view: what SR08 reads back for each value written */
    {
        static const UBYTE tryv[] = { 0x00, 0x01, 0x02, 0x03, 0x40, 0x41, 0x42, 0x43,
                                      0x04, 0x08, 0x10, 0x20, 0x80, 0xc3, 0xff };
        printf("  SR08 write -> read:");
        for (i = 0; i < (int)sizeof(tryv); i++) {
            wseq(0x08, tryv[i]);
            pause();
            printf(" %02x>%02x", tryv[i], rseq(0x08));
        }
        puts("");
        wseq(0x08, saved);
    }
    out = (saved & ~0x43) | 0x40;        /* pins enabled, both released */
    /* do the lines follow what is driven? */
    set(0, 0); c0 = scl_in(); d0 = sda_in();
    set(1, 1); c1 = scl_in(); d1 = sda_in();
    printf("  lines driven low:  clock reads %d, data reads %d\n", c0, d0);
    printf("  lines released:    clock reads %d, data reads %d\n", c1, d1);

    if (!d1) {
        /* a released I2C data line is pulled high; one that reads low can't
         * carry anything (and would look like an "acknowledge" to every byte) */
        wseq(0x08, saved);
        puts("ddcprobe: no DDC here - the data line reads low when released:");
        puts("          this board doesn't wire the chip's DDC data pin");
        return 5;
    }
    i2c_start();
    ackW = i2c_write(0xa0);
    if (ackW) {
        i2c_write(0x00);                 /* EDID offset 0 */
        i2c_start();
        ackR = i2c_write(0xa1);
        if (ackR)
            for (i = 0; i < 128; i++)
                edid[i] = i2c_read(i == 127);
    }
    i2c_stop();
    wseq(0x08, saved);

    printf("  monitor answered at $A0: %s%s\n", ackW ? "yes" : "no",
           ackW ? (ackR ? ", and to the read" : ", but not to the read") : "");
    if (!ackR) {
        printf("ddcprobe: no DDC here (%s)\n",
               (c0 == c1 && d0 == d1) ? "the lines don't follow the pins: not wired on this board"
                                      : "lines move but no monitor answers");
        return 5;
    }
    for (i = 0; i < 128; i++) sum += edid[i];
    for (i = 0; i < 8; i++) if (edid[i] != hdr[i]) ok = FALSE;
    printf("  EDID header %s, checksum %s\n", ok ? "good" : "WRONG", sum == 0 ? "good" : "WRONG");
    for (i = 0; i < 128; i++)
        printf("%02x%s", edid[i], (i & 15) == 15 ? "\n" : " ");
    if (ok && sum == 0) {
        UWORD m = ((UWORD)edid[8] << 8) | edid[9];
        printf("  maker %c%c%c, product $%02x%02x, EDID %u.%u\n", '@' + ((m >> 10) & 31),
               '@' + ((m >> 5) & 31), '@' + (m & 31), edid[11], edid[10], edid[18], edid[19]);
        printf("  established timings: $%02x $%02x $%02x\n", edid[35], edid[36], edid[37]);
        printf("ddcprobe: DDC WORKS on this card\n");
        return 0;
    }
    printf("ddcprobe: something answered but the data is not a valid EDID\n");
    return 5;
}
