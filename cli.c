/* z80ctrl (https://github.com/jblang/z80ctrl)
 * Copyright 2018 J.B. Langston
 *
 * Permission is hereby granted, free of charge, to any person obtaining a 
 * copy of this software and associated documentation files (the "Software"), 
 * to deal in the Software without restriction, including without limitation 
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, 
 * and/or sell copies of the Software, and to permit persons to whom the 
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file cli.c Monitor command-line interface
 */

#include <avr/pgmspace.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ff.h"
#include "bus.h"
#include "z80.h"
#include "cli.h"
#include "ihex.h"
#include "util.h"
#include "disasm.h"
#include "diskemu.h"
#include "sioemu.h"
#include "diskio.h"
#include "uart.h"
#include "tms.h"

/**
 * SD card filesystem
 */
FATFS fs;

/**
 * UART stdio stream
 */
FILE uart_str = FDEV_SETUP_STREAM(uart_putchar, uart_getchar, _FDEV_SETUP_RW);


/**
 * Load an Intel HEX file from disk or stdin
 */
void cli_loadhex(int argc, char *argv[])
{
    FIL fil;
    FILE file;
    FRESULT fr;
    ihex_res result;
    if (argc < 2) {
        printf_P(PSTR("loading from console; enter blank line to cancel\n"));
        result = load_ihex(stdin);
    } else {
        printf_P(PSTR("loading from %s\n"), argv[1]);
        if ((fr = f_open(&fil, argv[1], FA_READ)) == FR_OK) {
            fdev_setup_stream(&file, fatfs_putchar, fatfs_getchar, _FDEV_SETUP_RW);
            fdev_set_udata(&file, &fil);
            result = load_ihex(&file);
            if ((fr = f_close(&fil)) != FR_OK)
                printf_P(PSTR("error closing file: %S\n"), strlookup(fr_text, fr));
        } else {
            printf_P(PSTR("error opening file: %S\n"), strlookup(fr_text, fr));
            return;
        }        
    }
    printf_P(PSTR("loaded %d bytes total from %04x-%04x"), result.total, result.min, result.max);
    if (result.errors > 0)
        printf_P(PSTR(" with %d errors"), result.errors);
    printf_P(PSTR("\n"));
}

/**
 * Save an Intel HEX file to disk or stdout
 */
void cli_savehex(int argc, char *argv[])
{
    FRESULT fr;
    FILE file;
    FIL fil;
    if (argc < 3) {
        printf_P(PSTR("usage: savehex <start> <end> [file]\n"));
        return;
    }
    uint16_t start = strtoul(argv[1], NULL, 16) & 0xffff;
    uint16_t end = strtoul(argv[2], NULL, 16) & 0xffff;
    if (argc == 4) {
        if ((fr = f_open(&fil, argv[3], FA_WRITE | FA_CREATE_ALWAYS)) == FR_OK) {
            fdev_setup_stream(&file, fatfs_putchar, fatfs_getchar, _FDEV_SETUP_RW);
            fdev_set_udata(&file, &fil);
            if (save_ihex(start, end, &file) == EOF)
                printf_P(PSTR("error writing file"));
            if ((fr = f_close(&fil)) != FR_OK)
                printf_P(PSTR("error closing file: %s\n"), strlookup(fr_text, fr));
        } else {
            printf_P(PSTR("error opening file: %S\n"), strlookup(fr_text, fr));
        }        
    } else {
        if (save_ihex(start, end, stdout) == EOF)
            printf_P(PSTR("error writing file"));
    }
}

/**
 * Load a binary file to specifie address from disk with optional offset and length
 */
void cli_loadbin(int argc, char *argv[])
{
    FIL fil;
    FILE file;
    FRESULT fr;
    UINT br;
    uint8_t buf[256];
    uint16_t offset = 0;
    uint32_t len = 0x10000;
    uint8_t flash = (strcmp_P(argv[0], PSTR("flash")) == 0);
    uint8_t tms = (strcmp_P(argv[0], PSTR("tmslbin")) == 0);
    if (argc < 3) {
        printf_P(PSTR("usage: %s <start addr> <filename> [offset] [length]\n"), argv[0]);
        return;
    }
    uint16_t start = strtoul(argv[1], NULL, 16) & 0xffff;
    char *filename = argv[2];
    if (argc >= 4)
        offset = strtoul(argv[3], NULL, 16) & 0xffff;
    if (argc >= 5)
        len = strtoul(argv[4], NULL, 16) & 0xffff;
    if ((fr = f_open(&fil, filename, FA_READ)) != FR_OK) {
        printf_P(PSTR("error opening file: %S\n"), strlookup(fr_text, fr));
        return;
    } else if ((fr = f_lseek(&fil, offset)) != FR_OK) {
        printf_P(PSTR("seek error: %S\n"), strlookup(fr_text, fr));
    } else {
        while ((fr = f_read(&fil, buf, 256, &br)) == FR_OK) {
            if (br > len)
                br = len;
#ifdef IORQ_OUTPUT
            if (flash)
                flash_write(start, buf, br);
            else
#endif
            if (tms)
                tms_write(start, buf, br);
            else
                write_mem(start, buf, br);
            if (br < 256)
                break;
            start += br;
            len -= br;
        }
        if (fr != FR_OK)
            printf_P(PSTR("read error: %S\n"), strlookup(fr_text, fr));
    }
    if ((fr = f_close(&fil)) != FR_OK)
        printf_P(PSTR("error closing file: %S\n"), strlookup(fr_text, fr));
}


#ifdef IORQ_OUTPUT
/**
 * Erase a flash sector or entire chip
 */
void cli_erase(int argc, char *argv[])
{
    if (argc < 2) {
        printf_P(PSTR("usage: erase <addr> | erase all\n"));
        return;
    }
    uint32_t addr;
    if (strcmp_P(argv[1], PSTR("all")) == 0)
        addr = 0x80000;
    else
        addr = strtoul(argv[1], NULL, 16);
    flash_erase(addr);
}
#endif

/**
 * Save an region of memory to a binary file on disk
 */
void cli_savebin(int argc, char *argv[])
{
    FRESULT fr;
    FILE file;
    FIL fil;
    UINT bw;
    uint16_t len = 256;
    uint8_t buf[256];
    if (argc < 4) {
        printf_P(PSTR("usage: savebin <start> <end> [file]\n"));
        return;
    }
    uint16_t start = strtoul(argv[1], NULL, 16) & 0xffff;
    uint16_t end = strtoul(argv[2], NULL, 16) & 0xffff;
    if ((fr = f_open(&fil, argv[3], FA_WRITE | FA_CREATE_ALWAYS)) == FR_OK) {
        while (start <= end) {
            if (end - start + 1 < len)
                len = end - start + 1;
            read_mem(start, buf, len);
            if ((fr = f_write(&fil, buf, len, &bw)) != FR_OK) {
                printf_P(PSTR("write error: %S\n"), strlookup(fr_text, fr));
                break;
            }
            start += len;
        }
        if ((fr = f_close(&fil)) != FR_OK)
            printf_P(PSTR("error closing file: %s\n"), strlookup(fr_text, fr));
    } else {
        printf_P(PSTR("error opening file: %S\n"), strlookup(fr_text, fr));
    }        
}

/**
 * Disassemble code from memory
 */
void cli_disasm(int argc, char *argv[])
{
    uint16_t start, end;
    if (argc < 2) {
        printf_P(PSTR("usage: disasm <start> [end]\n"));
        return;
    }
    start = strtoul(argv[1], NULL, 16) & 0xffff;
    if (argc < 3)
        end = start + 0xf;
    else
        end = strtoul(argv[2], NULL, 16) & 0xffff;
    disasm_mem(start, end);
}

/**
 * Generate a hex dump from memory
 */
void cli_dump(int argc, char *argv[])
{
    uint8_t tms = (strcmp_P(argv[0], PSTR("tmsdump")) == 0);
    if (argc < 2) {
        printf_P(PSTR("usage: dump <start> [end]\n"));
        return;
    }
    uint16_t start = strtoul(argv[1], NULL, 16) & 0xffff;
    uint16_t end;
    if (argc < 3)
        end = start + 0xff;
    else
        end = strtoul(argv[2], NULL, 16) & 0xffff;
    uint8_t buf[16];
    uint8_t buflen = 16;
    uint32_t i = start;
    uint8_t j;

    printf_P(PSTR("%04x-%04x\n"), start, end);
    while (i <= end) {
        printf_P(PSTR("%04X   "), i);
        if (tms)
            tms_read(i, buf, buflen);
        else
            read_mem(i, buf, buflen);
        for (j = 0; j < buflen; j++) {
            printf_P(PSTR("%02X "), buf[j]);
            if (j % 4 == 3)
                printf_P(PSTR(" "));
        }
        printf_P(PSTR(" "));
        for (j = 0; j < buflen; j++, i++) {
            if (0x20 <= buf[j] && buf[j] <= 0x7e)
                printf_P(PSTR("%c"), buf[j]);
            else
                printf_P(PSTR("."));
        }
        printf_P(PSTR("\n"));
    }
}

/**
 * Run the processor at full speed from an optional starting address
 */
void cli_run(int argc, char *argv[])
{
    if (argc >= 2) {
        uint16_t addr = strtoul(argv[1], NULL, 16) & 0xffff;
        z80_reset(addr);
    }
    z80_run();
}

/**
 * Reset the processor and optionally set up a reset vector
 */
void cli_reset(int argc, char *argv[])
{
    uint16_t addr = 0;
    if (argc >= 2) {
        addr = strtoul(argv[1], NULL, 16) & 0xffff;
    }
    z80_reset(addr);
}

/**
 * Run the processor with debugging enabled from an optional starting address
 */
void cli_debug(int argc, char *argv[])
{
    if (argc >= 2) {
        uint16_t addr = strtoul(argv[1], NULL, 16) & 0xffff;
        z80_reset(addr);
    }
    z80_debug(0);
}

/**
 * Run the processor for a specified number of instructions
 */
void cli_step(int argc, char *argv[])
{
    uint32_t cycles = 1;
    if (argc >= 2)
        cycles = strtoul(argv[1], NULL, 10);            
    z80_debug(cycles);
}

/**
 * Reduce the Z80 clock speed by the specified factor
 */
void cli_clkdiv(int argc, char *argv[])
{
    uint8_t tmp = 0;
    if (argc >= 2)
        tmp = strtoul(argv[1], NULL, 10);
    if (tmp > 0)
        clkdiv = tmp;
    else
        printf_P(PSTR("current clkdiv=%d\nusage: clkdiv <divider>\n"), clkdiv);
}

/**
 * Interrogate or set breakpoint and watch ranges
 */
void cli_breakwatch(int argc, char *argv[])
{
    range *ranges;
    uint8_t type;

    if (strcmp_P(argv[0], PSTR("break")) == 0)
        ranges = breaks;
    else
        ranges = watches;
    
    // If no parameters given, show current status
    if (argc == 1) {
        printf_P(PSTR("%s status:\n"), argv[0]);
        for (uint8_t i = 0; i < DEBUGCNT; i++) {
            if (!ENABLED(ranges, i))
                printf_P(PSTR("\t%S\tdisabled\n"), strlookup(debug_names, i));
            else
                printf_P(PSTR("\t%S\t%04x-%04x\n"), strlookup(debug_names, i), ranges[i].start, ranges[i].end);
        }
        printf_P(PSTR("\nusage:\n\t%s <type> [start] [end]\n"), argv[0]);
        printf_P(PSTR("\t%s <type> off to disable type\n"), argv[0]);
        printf_P(PSTR("\t%s off to disable all\n"), argv[0]);
        return;
    }
    if (strcmp_P(argv[1], PSTR("off")) == 0) {
        // turn off all ranges
        for (uint8_t i = 0; i < DEBUGCNT; i++) {
            ranges[i].start = 0xffff;
            ranges[i].end = 0;
        }
        return;
    } else {
        // find the debugging type that the user specified
        for (type = 0; type < DEBUGCNT; type++)
            if (strcmp_P(argv[1], strlookup(debug_names, type)) == 0)
                break;
        if (type == DEBUGCNT) {
            printf_P(PSTR("error: unknown type\n"));
            return;
        }
        if (argc == 2) {
            // no range specified, enable for 0x0000-0xffff
            ranges[type].start = 0;
            ranges[type].end = 0xffff;
        } else if (argc >= 3) {
            if (strcmp_P(argv[2], PSTR("off")) == 0) {
                ranges[type].start = 0xffff;
                ranges[type].end = 0;
            } else {
                // get starting address
                ranges[type].start = strtoul(argv[2], NULL, 16);
                if (argc >= 4)
                    // get ending address if specified
                    ranges[type].end = strtoul(argv[3], NULL, 16);
                else
                    // if no ending address, start and end are the same
                    ranges[type].end = ranges[type].start;
            }
        }
    }
}

/**
 * Show a directory of files on the SD Card
 */
void cli_dir(int argc, char *argv[])
{
 	FRESULT fr;
    FILINFO finfo;
    DIR dir;
	UINT cnt;
    char fname[14];

    if (argc == 1)
        argv[1] = "/";

    fr = f_opendir(&dir, argv[1]);
    if (fr) {
        printf_P(PSTR("error reading directory: %S\n"), strlookup(fr_text, fr));
        return;
    }
    cnt = 0;
    for(;;) {
        fr = f_readdir(&dir, &finfo);
        if (fr != FR_OK) { 
            printf_P(PSTR("error reading directory: %S\n"), strlookup(fr_text, fr)); 
            break; 
        }
        if ((cnt % 5 == 0 && cnt != 0) || !finfo.fname[0])
            printf_P(PSTR("\n"));
        if (!finfo.fname[0]) break;
        strcpy(fname, finfo.fname);
        if (finfo.fattrib & AM_DIR)
            strcat(fname, "/");
        printf_P(PSTR("%-14s"), fname);
        cnt++;
    }
    if (fr)
        printf_P(PSTR("error reading directory: %S\n"), strlookup(fr_text, fr));
    else
        printf_P(PSTR("%u item(s)\n"), cnt);
}


/**
 * Verify specified number of bytes from external memory against a buffer
 */
int verify_mem(uint16_t start, uint16_t end, uint8_t *src, uint8_t log)
{
    uint8_t buf[256];
    int errors = 0;
    uint16_t j = 0, buflen = 256;
    uint32_t i = start;

    while (i <= end) {
        if (end - i + 1 < buflen)
            buflen = end - i + 1;
        read_mem(start, buf, buflen);
        for (j = 0; j < buflen; i++, j++) {
            if (buf[j] != src[i]) {
                if (log)
                    printf_P(PSTR("%04x: expected %02x but read %02x\n"), start+i, src[i], buf[j]);
                errors++;
            }
        }
    }
    return errors;
}

/**
 * Fill memory with a specified byte for a specified range
 */
void cli_fill(int argc, char*argv[]) {
    if (argc != 4) {
        printf_P(PSTR("usage: fill <start> <end> <value>\n"));
        return;
    }
    uint8_t tms = (strcmp_P(argv[0], PSTR("tmsfill")) == 0);
    uint16_t start = strtoul(argv[1], NULL, 16) & 0xffff;
    uint16_t end = strtoul(argv[2], NULL, 16) & 0xffff;
    uint8_t buf[256];
    enum {ASC = 256, DESC};
    if (strcmp_P(argv[3], PSTR("asc")) == 0) {
        for (uint16_t i = 0; i < 256; i++)
            buf[i] = i;
    } else if (strcmp_P(argv[3], PSTR("desc")) == 0) {
        for (uint16_t i = 0; i < 256; i++)
            buf[i] = 255 - i;
    } else {
        uint8_t value = strtoul(argv[3], NULL, 16) & 0xff;
        for (uint16_t i = 0; i < 256; i++)
            buf[i] = value;
    }
    
    for (;;) {
        if (end - start > 256) {
            if (tms)
                tms_write(start, buf, 256);
            else
                write_mem(start, buf, 256);
            start += 256;
        } else {
            if (tms)
                tms_write(start, buf, end - start + 1);
                write_mem(start, buf, end - start + 1);
            break;
        }
    }
}

/**
 * Poke values into memory
 */
void cli_poke(int argc, char *argv[])
{
    uint8_t value;
    if (argc < 2)
        printf_P(PSTR("usage: poke <start> [value]\n"));
    uint16_t addr = strtoul(argv[1], NULL, 16) & 0xffff;
    if (argc == 3) {
        value = strtoul(argv[2], NULL, 16) & 0xff;
        write_mem(addr, &value, 1);
        return;
    }
    printf_P(PSTR("enter valid hex to replace; blank to leave unchanged; 'x' to exit.\n"));
    for (;;) {
        char buf[16];
        read_mem(addr, &value, 1);
        printf_P(PSTR("%04X=%02X : "), addr, value);
        if (fgets(buf, sizeof buf - 1, stdin) == NULL) {
            break;
        } else {
            char *end;
            value = strtoul(buf, &end, 16);
            if (*buf == '\n' || *buf == '\r' || *buf == '\0') {
                addr++;
                continue;
            } else if (end == buf) {
                break;
            } else {
                write_mem(addr, &value, 1);
                addr++;
            }
        }
    }
    
}

#ifdef IORQ_OUTPUT

/**
 * Output value to an IO register
 */
void cli_out(int argc, char *argv[])
{
    if (argc < 2) {
        printf_P(PSTR("usage: out <addr> <value>\n"));
        return;
    }
    uint8_t addr = strtoul(argv[1], NULL, 16) & 0xff;
    uint8_t value = strtoul(argv[2], NULL, 16) & 0xff;
    io_out(addr, value);
}

/**
 * Input value from an IO register
 */
void cli_in(int argc, char *argv[])
{
    if (argc < 2) {
        printf_P(PSTR("usage: in <addr>\n"));
        return;
    }
    uint8_t addr = strtoul(argv[1], NULL, 16) & 0xff;
    printf_P(PSTR("Read %02x from %02x\n"), io_in(addr), addr);
}

/**
 * Set the active pages in memory
 */
void cli_page(int argc, char *argv[])
{
    uint8_t bank, page = 0;
    if (argc < 2) {
        printf_P(PSTR("usage: page <page1> [page2] [page3] [page4]\n"));
        return;
    }
    for (bank = 0; bank < 4; bank++) {
        if (bank + 1 < argc)
            page = strtoul(argv[bank+1], NULL, 16) & 0xff;
        else
            page++;
        mem_page(bank, page);
    }
}

#endif

/**
 * Boot from a disk
 */
void cli_boot(int argc, char*argv[])
{
    if (argc == 2) {
        drive_mount(0, argv[1]);
    }
    if(drive_bootload()) {
        z80_reset(0);
        z80_run();
    }
}

/**
 * Display the bus status
 */
void cli_bus(int argc, char *argv[]) 
{
    z80_buslog(bus_status());
}

/**
 * Mount a disk image
 */
void cli_mount(int argc, char *argv[])
{
    if (argc != 3) {
        printf_P(PSTR("usage: mount <drive #> <filename>\n"));
        return;
    }
    uint8_t drv = strtoul(argv[1], NULL, 10);
    char *filename = argv[2];
    drive_mount(drv, filename);
}

/**
 * Unmount a disk image
 */
void cli_unmount(int argc, char *argv[])
{
    if (argc != 2) {
        printf_P(PSTR("usage: unmount <drive #>\n"));
        return;
    }
    uint8_t drv = strtoul(argv[1], NULL, 10);
    drive_unmount(drv);
}

/**
 * Attach a virtual UART to a physical one
 */
void cli_attach(int argc, char *argv[])
{
    if (argc != 3) {
        printf_P(PSTR("usage: attach <virtual uart> <physical uart>\n"));
        return;
    }
    uint8_t virtual = strtoul(argv[1], NULL, 10) & 1;
    uint8_t physical = strtoul(argv[2], NULL, 10) & 1;
    z80_uart[virtual] = physical;
}

/**
 * Set the baud rate for the UART
 */
void cli_baud(int argc, char *argv[]) {
    if (argc != 3) {
        printf_P(PSTR("usage: baud <uart> <baud>\n"));
        return;
    }
    uint8_t uart = strtoul(argv[1], NULL, 10) & 1;
    uint32_t requested = strtoul(argv[2], NULL, 10);
    uint16_t ubrr = 0;
    uint32_t actual = 0;
    // Find closest possible actual baud rate
    for (;;) {
        actual = (uint32_t)F_CPU / ((uint32_t)16 * ((uint32_t)ubrr + (uint32_t)1));
        if (actual <= requested)
            break;
        ubrr++;
        // stop if we have maxed out UBRR
        if (ubrr == 0)
            break;
    }
    printf_P(PSTR("UART %u: requested: %lu, actual: %lu\n"), uart, requested, actual);
    uart_flush();
    uart_init(uart, ubrr);
}

/**
 * Clear the screen
 */
void cli_cls(int argc, char *argv[])
{
    printf_P(PSTR("\e[0m\e[;H\e[2J"));
}

void cli_dispatch(char *buf);

#define WHITESPACE " \t\r\n"
#define MAXBUF 80
#define MAXARGS 8
#define AUTOEXEC "autoexec.z8c"

/**
 * Execute the commands in a file
 */
void cli_exec(char *filename)
{
    FIL fil;
    FILE file;
    FRESULT fr;
    char buf[MAXBUF];
    if ((fr = f_open(&fil, filename, FA_READ)) == FR_OK) {
        fdev_setup_stream(&file, fatfs_putchar, fatfs_getchar, _FDEV_SETUP_RW);
        fdev_set_udata(&file, &fil);
        while (fgets(buf, sizeof buf - 1, &file) != NULL && strlen(buf) != 0) {
            printf_P(PSTR("%s>%s"), filename, buf);
            cli_dispatch(buf);
        }
        if ((fr = f_close(&fil)) != FR_OK)
            printf_P(PSTR("error closing file: %S\n"), strlookup(fr_text, fr));
    } else {
        // don't show a file not found error for autoexec.bat
        if (fr != FR_NO_FILE || strcmp_P(filename, PSTR(AUTOEXEC)) != 0)
            printf_P(PSTR("error opening file: %S\n"), strlookup(fr_text, fr));
    }
}

/**
 * Submit the commands in a batch file
 */
void cli_do(int argc, char *argv[]) 
{
    if (argc < 2) {
        printf_P(PSTR("usage: do <filename>\n"));
        return;
    }
    cli_exec(argv[1]);
}

/**
 * Lookup table of monitor command names
 */
const char cli_cmd_names[] PROGMEM = 
    "attach\0"
    "baud\0"
    "boot\0"
    "bus\0"
    "break\0"
    "c\0"
    "clkdiv\0"
    "cls\0"
    "debug\0"
    "dir\0"
    "disasm\0"
    "do\0"
    "dump\0"
#ifdef IORQ_OUTPUT
    "erase\0"
#endif
    "fill\0"
#ifdef IORQ_OUTPUT
    "flash\0"
#endif
    "help\0"
#ifdef IORQ_OUTPUT
    "in\0"
#endif
    "loadbin\0"
    "loadhex\0"
    "mount\0"
#ifdef IORQ_OUTPUT
    "out\0"
    "page\0"
#endif
    "poke\0"
    "run\0"
    "reset\0"
    "savebin\0"
    "savehex\0"
    "s\0"
    "step\0"
    "tmsdump\0"
    "tmsfill\0"
    "tmslbin\0"
    "unmount\0"
    "watch";

/**
 * Lookup table of help text for monitor commands
 */
const char cli_cmd_help[] PROGMEM =
    "attach virtual uart to physical uart\0"        // attach
    "configure UART baud rate\0"                    // baud
    "boot from specified disk image\0"              // boot
    "display low-level bus status\0"                // bus
    "set breakpoints\0"                             // break
    "shorthand to continue debugging\0"             // c
    "set Z80 clock divider\0"                       // clkdiv
    "clear screen\0"                                // cls
    "debug code at address\0"                       // debug
    "shows directory listing\0"                     // dir
    "disassembles memory location\0"                // disasm
    "exeucte a batch file\0"                        // do
    "dump memory in hex and ascii\0"                // dump
#ifdef IORQ_OUTPUT
    "erase flash ROM\0"                             // erase
#endif
    "fill memory with byte\0"                       // fill
#ifdef IORQ_OUTPUT
    "flash file to ROM\0"                           // flash
#endif
    "list available commands\0"                     // help
#ifdef IORQ_OUTPUT
    "read a value from a port\0"                    // in
#endif
    "load binary file to memory\0"                  // loadbin
    "load intel hex file to memory\0"               // loadhex
    "mount a disk image\0"                          // mount
#ifdef IORQ_OUTPUT
    "write a value to a port\0"                     // out
    "select active memory pages\0"                  // page
#endif
    "poke values into memory\0"                     // poke
    "execute code at address\0"                     // run
    "reset the processor, with optional vector\0"   // reset
    "save binary file from memory\0"                // savebin
    "save intel hex file from memory\0"             // savehex
    "shorthand for step\0"                          // s
    "step processor N cycles\0"                     // step
    "dump tms memory in hex and ascii\0"            // tmsdump
    "fill tms memory with byte\0"                   // tmsfill
    "load binary file to tms memory\0"              // tmslbin
    "unmount a disk image\0"                        // unmount
    "set watch points";                             // watch

void cli_help(int argc, char *argv[]);

/**
 * Lookup table of function pointers for monitor commands
 */
void * const cli_cmd_functions[] PROGMEM = {
    &cli_attach,
    &cli_baud,
    &cli_boot,
    &cli_bus,
    &cli_breakwatch,
    &cli_debug, // c
    &cli_clkdiv,
    &cli_cls,
    &cli_debug,
    &cli_dir,
    &cli_disasm,
    &cli_do,
    &cli_dump,
#ifdef IORQ_OUTPUT
    &cli_erase,
#endif
    &cli_fill,
#ifdef IORQ_OUTPUT
    &cli_loadbin, // flash
#endif
    &cli_help,
#ifdef IORQ_OUTPUT
    &cli_in,
#endif
    &cli_loadbin,
    &cli_loadhex,
    &cli_mount,
#ifdef IORQ_OUTPUT
    &cli_out,
    &cli_page,
#endif
    &cli_poke,
    &cli_run,
    &cli_reset,
    &cli_savebin,
    &cli_savehex,
    &cli_step,  // s
    &cli_step,
    &cli_dump,      // tmsdump
    &cli_fill,      // tmsfill
    &cli_loadbin,   // tmslbin
    &cli_unmount,
    &cli_breakwatch
};

#define NUM_CMDS (sizeof(cli_cmd_functions)/sizeof(void *))

/**
 * List available commands with help text
 */
void cli_help(int argc, char *argv[])
{
    int i;
    printf_P(PSTR("available commands:\n"));
    for (i = 0; i < NUM_CMDS; i++) {
        printf_P(PSTR("%S\t%S\n"), strlookup(cli_cmd_names, i), strlookup(cli_cmd_help, i));
    }
}

/**
 * Dispatch a single command
 */
void cli_dispatch(char *buf) 
{
    char *cmd;
    char *argv[MAXARGS];
    int argc;
    int i;
    void (*cmd_func)(int, char*[]);
    if ((argv[0] = strtok(buf, WHITESPACE)) == NULL)
        return;            
    for (argc = 1; argc < MAXARGS; argc++) {
        if ((argv[argc] = strtok(NULL, WHITESPACE)) == NULL)
            break;
    }
    for (i = 0; i < NUM_CMDS; i++) {
        if (strcmp_P(argv[0], strlookup(cli_cmd_names, i)) == 0) {
            cmd_func = pgm_read_ptr(&cli_cmd_functions[i]);
            break;
        }
    }
    if (i == NUM_CMDS)
        printf_P(PSTR("unknown command: %s. type help for list.\n"), argv[0]);
    else
        cmd_func(argc, argv);
}

/**
 * Main command-line loop for monitor
 */
void cli_loop(void) 
{
    char buf[MAXBUF];
    printf_P(PSTR(
        "z80ctrl 0.9 by J.B. Langston\n\n"
        "type help to list available commands\n"));
    for (;;) {
        printf_P(PSTR("z80ctrl>"));
        if (fgets(buf, sizeof buf - 1, stdin) != NULL) {
            cli_dispatch(buf);
        }
    }
}

/**
 * z80ctrl entry point
 */
int main(void)
{
    FRESULT fr;

    uart_init(0, UBRR115200);
    uart_init(1, UBRR115200);
    stdout = stdin = &uart_str;

    disk_initialize(DRV_MMC);
    if ((fr = f_mount(&fs, "", 1)) != FR_OK)
        printf_P(PSTR("error mounting drive: %S\n"), strlookup(fr_text, fr));

    bus_init();

    cli_exec(AUTOEXEC);
    tms_init();
    cli_loop();
}