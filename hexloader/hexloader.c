#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/pgmspace.h>
#include <avr/boot.h>
#include <avr/wdt.h>
#include <util/delay.h>


// Constants

#define VERSION                     "1.0"
#ifndef GIT_VERSION
#define GIT_VERSION
#endif
#define DEBUG

#define UBRR                        16      //< 34 = 56.6K, 16 = 115.2K bps, 8 = 230.4K bps
#define RX_BUFFER_LEN               1024    //< receive buffer length
#define TX_BUFFER_LEN               32      //< transmit buffer length
#define LF                          10      //< \n ascii
#define CR                          13      //< \r ascii
#define ESC                         27      //< ESC ascii
#define BS                          8       //< backspace ascii

#define ERROR_RX_DATA_OVERRUN       1       ///< UART data overrun
#define ERROR_RX_FRAME_ERROR        2       ///< UART frame error
#define ERROR_RX_BUFFER_OVERFLOW    4       ///< UART #rx_buffer overflow

#define MODE_FLASH                  0       ///< #flash_hex_line mode (flash)
#define MODE_VERIFY                 1       ///< #flash_hex_line mode (verify)

#define FLASH_WAITING               0       ///< waiting for the first ihex line
#define FLASH_GOING_ON              1       ///< an ihex is in progress
#define FLASH_OK                    2       ///< ihex eof received and flash done ok
#define FLASH_ERROR                 3       ///< flash (eg. FCS mismatch) or verify errors

#define PAGE_SIZE                   128     ///< atmega328p page size
#define MAX_LINE_LEN                64      ///< 16 hex bytes/line as generated by objcopy
#define FLASH_SIZE                  32768   ///< atmega328p total flash

#define BOOTAPP_SIG_1               0xb0    // boot into app signature
#define BOOTAPP_SIG_2               0xaa


// Macros

#define LED_ON() PORTB |= _BV(PORTB5)       /**< Turn on the LED */
#define LED_OFF() PORTB &= ~_BV(PORTB5)     /**< Turn off the LED */

/** Check if signature found in r2/r3, boot app is requested */
#define BOOT_APP() (r2 == BOOTAPP_SIG_1 && r3 == BOOTAPP_SIG_2)

/** Sleep while condition holds true.  */
#define IDLE_WHILE(condition) \
    do { \
        cli(); \
        if (condition) { \
            sleep_enable(); \
            sei(); \
            sleep_cpu(); \
            sleep_disable(); \
        } else { \
            sei(); \
            break; \
        } \
        sei(); \
    } while (1);

// Variables

/**
 * Boot signature
 * Registers r2 and r3 are used to switch between bootloader and app at
 * reset time.
 */
register uint8_t r2 asm("r2");
register uint8_t r3 asm("r3");

volatile uint8_t rx_buffer[RX_BUFFER_LEN];  ///< UART receive buffer
volatile uint8_t tx_buffer[TX_BUFFER_LEN];  ///< UART transmit buffer
volatile uint16_t rx_head, rx_tail, tx_head, tx_tail;
volatile uint8_t uart_error;                ///< one of #ERROR_RX_DATA_OVERRUN, #ERROR_RX_FRAME_ERROR or #ERROR_RX_BUFFER_OVERFLOW   
volatile uint32_t clock;                    ///< number of milliseconds since boot */
volatile uint32_t t0;
volatile uint16_t breathing_led;


char line[MAX_LINE_LEN];        ///< Buffer containing hex lines or commands
uint8_t page[PAGE_SIZE];        ///< Buffer containing the current page (to be flashed or verified)
uint16_t current_page_address;  ///< This is the current page address. Page address = byte address / PAGE_SIZE

///////////////////////////////////////////////////////////////////////
// ISR routines 
///////////////////////////////////////////////////////////////////////

/**
 * UART RX ISR.
 * Called when the hardware UART receives a byte.
 */
ISR(USART_RX_vect)
{
    uint8_t status = UCSR0A;
    uint8_t data = UDR0;
    uint16_t new_head = (rx_head + 1) % RX_BUFFER_LEN;

    if (status & _BV(DOR0))
        uart_error |= ERROR_RX_DATA_OVERRUN;
    if (status & _BV(FE0))
        uart_error |= ERROR_RX_FRAME_ERROR;

    // If head meets tail -> overflow and ignore the received byte
    if (new_head == rx_tail) {
        uart_error |= ERROR_RX_BUFFER_OVERFLOW;
    }
    else {
        rx_buffer[rx_head] = data;
        rx_head = new_head;
    }
    // delay watchdog reboot while pending rx data
    wdt_reset();
}

/**
 * UART Data Register Empty ISR.
 * Called when the UART is ready to accept a new byte for transmission.
 */
ISR(USART_UDRE_vect)
{
    if (tx_head == tx_tail) {
        // Buffer is empty, disable UDRE int
        UCSR0B &= ~_BV(UDRIE0);
    }
    else {
        // Send byte
        UDR0 = tx_buffer[tx_tail];
        tx_tail = (tx_tail + 1) % TX_BUFFER_LEN;
    }
    // delay the reboot until no more pending tx
    wdt_reset();
}

/**
 * Timer 0 comparator A ISR.
 * Called when timer 0 counts up to OCR0A. This is used to keep track
 * of time and also for the breathing LED (software PWM).
 */
ISR(TIMER0_COMPA_vect)
{
    clock++;
    LED_ON();
    if (clock % 8 == 0) {
        // breath in the lower half brightness range of the led (0 .. OCR0A / 2)
        if (++breathing_led > OCR0A) breathing_led = 0;
        if (breathing_led < OCR0A / 2)
            OCR0B = breathing_led;
        else
            OCR0B = OCR0A - breathing_led;
    }
}

/**
 * Time 0 comparator B ISR.
 * Called when timer 0 counts up to OCR0B. This is used for the breathing
 * LED (software PWM).
 */
ISR(TIMER0_COMPB_vect)
{
    LED_OFF();
}


///////////////////////////////////////////////////////////////////////
// Conversion utils
///////////////////////////////////////////////////////////////////////

/**
  * Convert a nibble to hex.
  * @param x the nibble
  * @return an hex char [0-9A-Z]
  */
uint8_t nibble_to_hex(uint8_t x)
{
    x &= 0xf;
    if (x >= 0 && x <= 9)
        return x + '0';
    else
        return x + 'A' - 10;
}

/**
  * Convert a byte to hex.
  * @param x the byte
  * @return two hex chars packed in a word (leftmost char in the MSB)
  */
uint16_t byte_to_hex(uint8_t x)
{
    return nibble_to_hex(x >> 4) << 8 | nibble_to_hex(x & 0xf);
}

/**
 * Convert a word to hex.
 * @param x the word (16 bits)
 * @return 4 hex chars packed in a uint32_t (leftmost char in the MSB)
 */
uint32_t word_to_hex(uint16_t x)
{
    return (uint32_t)byte_to_hex(x >> 8) << 16 | byte_to_hex(x & 0xff);
}

/**
 * Convert an ascii hex nibble to decimal.
 * This is used to decode the hex file.
 * @param x the ascii nibble [0-9a-fA-F]
 * @return a value between 0 and 15
 */
uint8_t hex_nibble_to_dec(char x)
{
    if (x >= '0' && x <= '9')
        return x - '0';
    else if (x >= 'A' && x <= 'F')
        return x - 'A' + 10;
    else if (x >= 'a' && x <= 'f')
        return x - 'a' + 10;
    else
        return 0;
}

/**
 * Convert an ascii hex byte to decimal.
 * @param s a 2-byte array containing the ascii hex value
 * @return a value between 0 and 255
 */
uint8_t hex_byte_to_dec(char *s)
{
    return 16 * hex_nibble_to_dec(s[0]) + hex_nibble_to_dec(s[1]);
}

/**
 * Convert an ascii hex word to decimal
 * @param a 4-byte array containing the ascii hex value
 * @return a value between 0 and 65535
 */
uint16_t hex_word_to_dec(char *s)
{
    return 256 * hex_byte_to_dec(s) + hex_byte_to_dec(s + 2);
}


///////////////////////////////////////////////////////////////////////
// Timing functions 
///////////////////////////////////////////////////////////////////////

/**
 * Start the timer.
 * Use the timer in CTC (clear timer on compare) mode to count up to OCR0A
 * and generate interrupts on both OCR0A/OCR0B matches.
 *
 * With a prescaler = 64, OCR0A = 249 and CPU clock = 16 MHz, the period
 * is 0.996ms (~ 1ms).
 */
void timer_init()
{
    TCCR0A = _BV(WGM01);                // CTC mode (count up to OCR0A)
    OCR0A = 240;                        // 249 * 64 / 16M = 0.996 ms
    TCCR0B = _BV(CS01) | _BV(CS00);     // clk/64 prescaler
    TIMSK0 = _BV(OCIE0A) | _BV(OCIE0B); // Interrupt on both A, B match
}

/**
 * Current time in ms.
 * @return the number of milliseconds since reset
 */
uint32_t millis()
{
    uint32_t m;
    cli();      // read atomically
    m = clock;
    sei();
    return m;
}


///////////////////////////////////////////////////////////////////////
// UART functions
///////////////////////////////////////////////////////////////////////

/**
 * Start the UART.
 * Set the UART to 2x speed mode, 115.2K bauds, 8N1 and enable rx 
 * interrupts.
 */
void uart_init()
{
    // Set baud rate
    UBRR0H = (uint8_t) (UBRR >> 8);
    UBRR0L = (uint8_t) (UBRR);

    // double speed
    UCSR0A = _BV(U2X0);

    // Enable receiver and transmitter, generate interrupts on RX, DRE
    UCSR0B = _BV(RXCIE0) | _BV(RXEN0) | _BV(TXEN0);

    // 8,N,1
    UCSR0C = (3 << UCSZ00);
}

/**
 * Send a byte.
 * If the tx queue is full, it will sleep (ie. block) until space becomes
 * available.
 * @param c the byte
 */
void uart_send_byte(uint8_t c)
{
    uint16_t new_head = (tx_head + 1) % TX_BUFFER_LEN;

    IDLE_WHILE(tx_tail == new_head);

    tx_buffer[tx_head] = c;
    cli();      // atomically update the head
    tx_head = new_head;
    sei();

    // Enable UDRE int, this will trigger the UDRE ISR
    UCSR0B |= _BV(UDRIE0);
}

/**
  * Flush the tx buffer.
  */
void uart_flush()
{
    IDLE_WHILE(tx_tail != tx_head);
}

/**
 * Send a PROGMEM string.
 * @param s the string, which *must* be in program space
 */
void uart_send_string(char const s[])
{
    char c;
    while ((c = pgm_read_byte(s++)))
        uart_send_byte(c);
}

/**
 * Send a uint16_t in decimal format.
 * @param x the uint16_t value
 */
void uart_send_int(uint16_t x)
{
    if (x < 10)
        uart_send_byte(x + '0');
    else {
        uart_send_int(x / 10);
        uart_send_byte(x % 10 + '0');
    }
}

/**
 * Print out a byte in hexadecimal.
 * @param x the byte
 */
void uart_send_byte_hex(uint8_t x)
{
    uint16_t h = byte_to_hex(x);
    uart_send_byte(h >> 8);
    uart_send_byte(h & 0xff);
}

/**
 * Print out a word (uint16_t) in hexadecimal.
 * @param x the word
 */
void uart_send_int_hex(uint16_t x)
{
    uint32_t h = word_to_hex(x);
    uart_send_byte(h >> 24);
    uart_send_byte((h >> 16) & 0xff);
    uart_send_byte((h >> 8) & 0xff);
    uart_send_byte(h & 0xff);
}

/**
 * Receive a byte.
 * @return an int16_t with the byte, or -1 if no data available
 */
int16_t uart_recv_byte() 
{
    if (rx_tail == rx_head)
        return -1;    // no data

    cli();
    int16_t c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUFFER_LEN;
    sei();

    return c;
}

/**
 * Check if there is incoming data over the UART.
 * @return true if data available
 */
int8_t uart_available()
{
    return (rx_tail != rx_head);
}


///////////////////////////////////////////////////////////////////////
// Reboot functions
///////////////////////////////////////////////////////////////////////

/** Force a reboot.
 * Reboots the AVR by setting a 15 ms watchdog timer. Interrupts are
 * allowed, so that pending rx or tx data gets flushed.
 */
void reboot() {
    wdt_enable(WDTO_15MS);
    // wait for pending uart i/o to flush
    //for (;;);
    IDLE_WHILE(1);
}

/** Reboot to bootloader.
 * Sets a 15 ms watchdog timer and spinlocks until the watchdog reboots
 * the AVR. Registers r2 = r3 = 0 are used to signal bootloader run.
 */
void reboot_to_bootloader()
{
    uart_send_string(PSTR("Rebooting into bootloader\r\n\r\n"));
    r2 = r3 = 0;
    reboot();
}

/** Reboot to user app.
 * Sets a 15 ms watchdog timer and spinlocks until the watchdog reboots
 * the AVR. Registers r2 = r3 = 0 are used to signal bootloader run.
 */
void reboot_to_app()
{
    uart_send_string(PSTR("Have a nice day!\r\n\r\n"));
    r2 = BOOTAPP_SIG_1;
    r3 = BOOTAPP_SIG_2;
    wdt_enable(WDTO_15MS);  // will cli() for us
    for (;;);
}


///////////////////////////////////////////////////////////////////////
// Input parsing
///////////////////////////////////////////////////////////////////////

/**
 * Get a line of #MAX_LINE_LEN max bytes.
 * @return 1 if there is a valid line in the #line buffer.
 */
uint8_t get_line()
{
    static uint8_t len = 0;

    if (uart_error & ERROR_RX_BUFFER_OVERFLOW) {
        uart_send_string(PSTR("\r\nUART error: buffer overflow (try a lower baud rate)\r\n"));
        reboot_to_bootloader();
    }
    //if (uart_error & ERROR_RX_FRAME_ERROR)
    //    uart_send_string(PSTR("\r\nUART error: frame error\r\n"));
    if (uart_error & ERROR_RX_DATA_OVERRUN) {
        uart_send_string(PSTR("\r\nUART error: data overrun\r\n"));
        reboot_to_bootloader();
    }

    if (uart_available()) {
        char c = uart_recv_byte();
        if (c == ESC) {
            uart_send_string(PSTR("\r\n"));
            line[0] = '\0';
            len = 0;
            return 1;
        }
        if (c == CR || c == LF) {
            line[len] = '\0';
            if (len > 0) {
                len = 0;
                if (line[0] != ':') {   // no echo if receiving hex
                    uart_send_string(PSTR("\r\n"));
                }
                return 1;
            }
        }
        else {
            if (len < MAX_LINE_LEN - 1) {
                line[len++] = c;
                if (line[0] != ':') {   // no echo if receiving hex
                    uart_send_byte(c);
                }
            }
            else if (len == MAX_LINE_LEN - 1) {
                line[len++] = '\0';
            }
        }
    }
    return 0;
}

/**
 * Show a comand prompt.
 */
void prompt()
{
    uart_send_string(PSTR(">: "));
}


///////////////////////////////////////////////////////////////////////
// Flashing functions
///////////////////////////////////////////////////////////////////////

/** Point out an error with carets.
 * @param col the column to point out
 * @param carets the number of carets
 */
void point_out_error(uint8_t col, uint8_t carets)
{
    int i;
    for (i = 0; i < col; i++)
        uart_send_byte(' ');
    for (i = 0; i < carets; i++)
        uart_send_byte('^');
    uart_send_string(PSTR("\r\n"));
}

/**
 * Dump an ihex encoded line.
 */
void dump_line()
{
    int i;
    for (i = 0; line[i]; i++) {
        uart_send_byte(line[i]);
    }
    uart_send_string(PSTR("\r\n"));
}

/**
 * Empty the current page with 0xff (nop).
 */
void new_page()
{
    int i;
    for (i = 0; i < PAGE_SIZE; i++)
        page[i] = 0xff;
}

/**
 * Flash the current page.
 * Writes the current page at #current_page_address from the data in
 * the #page buffer.
 */
void write_current_page()
{
    uint16_t i;

#if 0
    uart_send_byte(CR);
    uart_send_byte('p');
    uart_send_byte('=');
    uart_send_int(current_page_address);
    uart_send_byte(CR);
    uart_send_byte(LF);
#endif

    boot_spm_busy_wait();
    cli();
    boot_page_erase(current_page_address * PAGE_SIZE);
    sei();
    boot_spm_busy_wait();

    for (i = 0; i < PAGE_SIZE; i += 2) {
        // make little endian words by swapping every two bytes
        uint16_t word = page[i] | (page[i+1] << 8);
        cli();
        boot_page_fill(current_page_address * PAGE_SIZE + i, word);
        sei();
    }

    cli();
    boot_page_write(current_page_address * PAGE_SIZE);
    sei();
    boot_spm_busy_wait();

    LED_OFF();
}

/**
 * Validate an address.
 * Addresses in the ihex file must start at 0 and be monotonically 
 * increasing.
 * @param last_address last address
 * @param address currant address
 * @return true if valid
 */
uint8_t is_address_valid(uint16_t last_address, uint16_t address) 
{
    if (last_address == 0xffff && address != 0) {
        uart_send_string(PSTR("\r\nFirst address must be 0:\r\n"));
        dump_line();
        point_out_error(3, 4);
        return 0;
    }

    if (last_address != 0xffff && address < last_address) {
        uart_send_string(PSTR("\r\nAddresses must be increasing:\r\n"));
        dump_line();
        point_out_error(3, 4);
        return 0;
    }
    return 1;
}

/**
 * Decode an hex line and flash if we have PAGE_SIZE bytes already.
 * @return flash status (#FLASH_GOING_ON, #FLASH_OK, #FLASH_ERROR)
 */
uint8_t flash_hex_line(uint8_t mode)
{
    static uint16_t last_address = 0xffff;   // keep track of the last address flashed
    uint16_t address;
    uint8_t checksum = 0;
    uint8_t count;
    uint8_t record_type;
    int i;

    count = hex_byte_to_dec(line + 1);
    address = hex_word_to_dec(line + 3);
    record_type = hex_byte_to_dec(line + 7);

    if (record_type == 0 && ! is_address_valid(last_address, address))
        return FLASH_ERROR;

    checksum = count + (uint8_t)address + (address >> 8) + record_type;

    for (i = 0; i < count; i++) {
        uint8_t b = hex_byte_to_dec(line + 9 + i * 2);

        if (mode == MODE_FLASH) {
            if ((address + i) / PAGE_SIZE != current_page_address) {
                // current page is ready to write
                write_current_page();
                new_page();
                current_page_address = (address + i) / PAGE_SIZE;
            }

            page[(address + i) % PAGE_SIZE] = b;
        }
        else {
            if (pgm_read_byte(address + i) != b) {
                uart_send_string(PSTR("\r\nHex and flash mismatch:\r\n"));
                dump_line();
                point_out_error(9 + i * 2, 2);
                return FLASH_ERROR;
            }
        }
        checksum += b;
    }

    checksum += hex_byte_to_dec(line + 9 + count * 2);

    if (checksum != 0) {
        uart_send_string(PSTR("\r\nChecksum error in line:\r\n"));
        dump_line();
        return FLASH_ERROR;
    }

#if 0
    for (i = 0; line[i]; i++) {
        uart_send_byte(line[i]);
    }

    uart_send_byte('/');
    uart_send_int(checksum);
    uart_send_byte('/');
#endif

#if 0
    uart_send_byte(LF);
    uart_send_byte('l');
    uart_send_byte('=');
    uart_send_int((rx_head + RX_BUFFER_LEN - rx_tail) % RX_BUFFER_LEN);
    uart_send_byte(CR);
    uart_send_byte(LF);
#endif


    if (record_type == 1) {        // End of file
        if (mode == MODE_FLASH) {
            // flush the current page
            write_current_page();

            // re-enable RWW area
            boot_rww_enable_safe();

            // We're not supposed to flash again from the same incarnation of
            // the bootloader, but clean up anyway.
            new_page();
            current_page_address = 0;
            last_address = 0xffff;
        }
        return FLASH_OK;
    }
    else {
        if (mode == MODE_FLASH) {
            uart_send_string(PSTR("\rFlashed: "));
        }
        else {
            uart_send_string(PSTR("\rVerified: "));
        }
        uart_send_int(address + count);
        last_address = address + count - 1;
        return FLASH_GOING_ON;
    }
}

/**
 * Dump the entire flash contents.
 */
void dump_flash()
{
    uint8_t checksum = 0;
    uint16_t address;
    for (address = 0; address < FLASH_SIZE; address++) {
        uint8_t b;
        if (address % 16 == 0) {
            uart_send_string(PSTR("\r\n:10"));
            uart_send_int_hex(address);
            uart_send_byte_hex(0);
            checksum = - 0x10 - (address >> 8) - (address & 0xff);
        }
        b = pgm_read_byte(address);
        uart_send_byte_hex(b);
        checksum -= b;
        if (address % 16 == 15) {
            uart_send_byte_hex(checksum);
        }
    }
    uart_send_string(PSTR("\r\n:00000001FF\r\n"));
}


///////////////////////////////////////////////////////////////////////
// Bootloader sequence
///////////////////////////////////////////////////////////////////////

/**
 * Parse the last entered line and run a command.
 */
void run_command()
{
    switch(line[0]) {
        case 'q':
            reboot_to_app();
            break;
        case 'r':
            reboot_to_bootloader();
            break;
        case 'd':
            dump_flash();
            prompt();
            break;
        case 'h':
            uart_send_string(PSTR(
                " q\treboot to app\r\n"
                " r\treboot to bootloader\r\n"
                " d\tdump flash in hex format\r\n"
                " esc\tabort current command\r\n"
            ));
            prompt();
            break;
        case '\0':
            prompt();
            break;
        default:
            uart_send_string(PSTR("'h' for help\r\n"));
            prompt();
            break;           
    }
}

/**
 * Bootloader sequence.
 */
void bootloader()
{
    uint8_t flash_status;
    uint8_t mode;

    // Move ISR vector table to the bootloader
    MCUCR = _BV(IVCE);
    MCUCR = _BV(IVSEL);

    // init led pin direction
    DDRB |= _BV(DDB5);

    // init uart and timer
    uart_init();
    timer_init();
    sei();

    // Idle mode is the only mode that will keep the UART running
    set_sleep_mode(SLEEP_MODE_IDLE);

    // prepare a new empty page
    new_page();

    // Run through the two modes: first flash, then verify
    for (mode = MODE_FLASH; mode <= MODE_VERIFY; mode++) {
        if (mode == MODE_FLASH) {
            uart_send_string(PSTR(
                "AVR Hexloader " VERSION "." GIT_VERSION "\r\n"
                "Paste your hex file, 'h' for help\r\n"));
        }
        else {
            uart_send_string(PSTR("Paste again to verify\r\n"));
        }
        prompt();

        flash_status = FLASH_WAITING;
        do {
            if (get_line()) {
                if (line[0] == ':') {
                    if (flash_status == FLASH_WAITING) {
                        t0 = millis();
                    }
                    flash_status = flash_hex_line(mode);
                }
                else {
                    run_command();
                }
            }
        } while (flash_status == FLASH_GOING_ON || flash_status == FLASH_WAITING);

        if (flash_status != FLASH_OK) {
            reboot_to_bootloader();
        }

        uart_send_string(PSTR(" OK! ("));
        uart_send_int(millis() - t0);
        uart_send_string(PSTR("ms)\r\n"));
    }
    reboot_to_app();
}

/**
 * Entry point.
 * Decides whether to run the bootloader or the user app.
 */
int main() {
    // Disable the watchdog if set
    if ((MCUSR & _BV(WDRF)) && BOOT_APP()) {
        // MCUSR &= ~_BV(WDRF);
        wdt_disable();
    }

    // Boot into app if any of:
    // - just powered on (no external or watchdog reset)
    // - bootloader 0xb0aa signature found (boot into app)
    if (!(MCUSR & (_BV(EXTRF) | _BV(WDRF))) || BOOT_APP()) {
        // Pass the reboot reason (MCUSR) to the app and clear it
        r2 = r3 = 0;
        asm("jmp 0");
    }
    else {
        r2 = BOOTAPP_SIG_1;
        r3 = BOOTAPP_SIG_2;
        bootloader();
    }
    return 0;
}