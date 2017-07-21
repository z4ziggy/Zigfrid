/*******************************************************************************
 *
 *   _______ _________ _______  _______  _______ _________ ______  
 *  / ___   )\__   __/(  ____ \(  ____ \(  ____ )\__   __/(  __  \ 
 *  \/   )  |   ) (   | (    \/| (    \/| (    )|   ) (   | (  \  )
 *      /   )   | |   | |      | (__    | (____)|   | |   | |   ) |
 *     /   /    | |   | | ____ |  __)   |     __)   | |   | |   | |
 *    /   /     | |   | | \_  )| (      | (\ (      | |   | |   ) |
 *   /   (_/\___) (___| (___) || )      | ) \ \_____) (___| (__/  )
 *  (_______/\_______/(_______)|/       |/   \__/\_______/(______/ 
 *
 * A Passive RFID fuzzer (C) Copyright 2017 Elia Yehuda aka z4ziggy.
 *
 *
 * The AVR uses the 125khz frequency from the RFID reader for power & clock
 * therefor no external power is required. Once the code executes, it will set 
 * up EM41xx header & footer in a buffer, and start filling the remaining blob
 * with IDs from the list whilst incrementing each ID afterwards and send it 
 * using an interrupt call.
 *
 * The interrupt call reads the buffer, xoring every odd offset with previous
 * bit due to the Manchester encoding, and sends the correct bit, all in less
 * than ~20 clock cycles, to allow the main procedure to increment the buffer
 * when needed.
 * 
 * Non critical parts are coded in C for easier hacking. The Assembly parts are
 * required for ensuring exact clock cycles and some needed optimizations.
 *
 *
 * Based on avrfrid.S by Micah Elizabeth Scott.
 * http://scanlime.org/2008/09/using-an-avr-as-an-rfid-tag/
 *
 *
 ******************************************************************************/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>

/*******************************************************************************
 *
 * list of EM41xs IDs to send, each ID is 5 bytes long.
 * 1st byte = manufacture id, 4 bytes = card id.
 * each ID will be incremented.
 *
 * change as you like, as long as you retain the game rules.
 *
 ******************************************************************************/
uint8_t em_id_list[] = {
                        0x00, 0x00, 0x00, 0x00, 0x00 ,
                        0xFF, 0xFF, 0xFF, 0xFF, 0xFF ,
                        0x11, 0x11, 0x11, 0x11, 0x11 ,
                        0x22, 0x22, 0x22, 0x22, 0x22 ,
                        0x33, 0x33, 0x33, 0x33, 0x33 ,
                        0x44, 0x44, 0x44, 0x44, 0x44 ,
                        0x55, 0x55, 0x55, 0x55, 0x55 ,
                        0x66, 0x66, 0x66, 0x66, 0x66 ,
                        0x77, 0x77, 0x77, 0x77, 0x77 ,
                        0x88, 0x88, 0x88, 0x88, 0x88 ,
                        0x99, 0x99, 0x99, 0x99, 0x99 ,
                        0x12, 0x34, 0x56, 0x78, 0x9A 
                        };

/*******************************************************************************
 *
 * here begins the real code & logic. hack at your own risk.
 *
 ******************************************************************************/

/* number of times to resend ID before proceeding to next one */
#define MAX_SEND_COUNTER        8 * 3

/* send manchester bit every xx cycles (inclusive) */
#define MAX_TIMER0              31

/* pins to enable/disable for rf */
#define OUT_PINS                _BV(PINB3) | _BV(PINB4)

/* get nibbles of a byte */
#define NIBBLE_HIGH(x)          (x >> 4)
#define NIBBLE_LOW(x)           (x & 0x0F)

/* the array which stores the bits (0/1) to send. we're not initalizing it with 
 * the EM41xx header & footer but doing this in code instead since it uses
 * less space. */
uint8_t em_bits[64];

/* using registers for global offsets & counters */
volatile register uint8_t send_offset       __asm__("r13") ;
volatile register uint8_t send_counter      __asm__("r14") ;
volatile register uint8_t read_offset_id    __asm__("r15") ;
volatile register uint8_t send_bit          __asm__("r16") ;
volatile register uint8_t write_offset      __asm__("r17") ;

/* writes a manchester bit to em_bits[] & increment the write_offset */
static void write_bit(uint8_t bit) 
{
    uint8_t b = (bit) ? 0 : OUT_PINS;
    em_bits[write_offset++] = b;
}

/* writes a nibble and returns its parity */
static uint8_t write_nibble(uint8_t nibble) 
{
    uint8_t x, parity = 0;
    for(int i = 0; i < 4; i++) {
        x = (nibble >> (3 - i)) & 0x1;
        write_bit(x);
        parity ^= x;
    }
    return parity;
}

/* write two nibbles of a byte plus their parity bit */
static void write_byte(uint8_t c)
{
    write_bit( write_nibble( NIBBLE_HIGH(c) ) );
    write_bit( write_nibble( NIBBLE_LOW(c) ) );
}

/* load one byte of current ID from em_id_list[] */
static uint8_t read_byte(uint8_t offset) 
{
    return em_id_list[read_offset_id + offset];
}

/* increment a 40bit (5 bytes) number in a buffer */
static void inc_em_id(void)
{
    for (int8_t i = 4; i >= 0; i--) {
        if (em_id_list[read_offset_id + i]++) break;
    }
}

/* writes a static header (9 ones) at the begining of em_bits[] and a stop bit
 * (zero) at the end */
static void write_em_header_footer(void) 
{
    write_offset = 0;
    for(uint8_t i = 0; i < 9; i++) {
        write_bit(1);
    }
    write_offset = 63;
    write_bit(0);
}

/* translates current ID from em_id_list[] to manchester encoding and writes to 
 * em_bits[] 
 */
static void write_em_id(void) 
{
    uint8_t checksum = 0;
    write_offset = 9;
    for(uint8_t i = 0; i < 5; i++) {
        uint8_t c = read_byte(i);
        checksum ^= c;
        write_byte( c );
    }
    write_nibble( NIBBLE_HIGH(checksum) ^ NIBBLE_LOW(checksum) );
}

/* this interrupt procedure will be called every 32 clock cycles. it will 
 * increment the send_offset, send current manchester bit, and on each odd
 * send_offset it will xor the current bit (since manchester encoding are always
 * reverted) and prepare it for the next call, otherwise it will load the next 
 * bit to be sent on the next call.
 *
 * we want this procedure to use less than ~20 cycles to provide the main loop 
 * enough cycles to check for current send_offset and reset it or pause the
 * interrupt routine and increment to next ID when needed.
 */
ISR(TIMER0_COMPA_vect) __attribute__ ((naked));
ISR(TIMER0_COMPA_vect) 
{
    /* increment send_offset & send current manchester bit */
    asm volatile(
        "inc %[offset]\n"
        "out %[port], %[bit]\n"
        : [offset] "=r" (send_offset)
        : [port] "I" (_SFR_IO_ADDR(DDRB)), [bit] "r" (send_bit)
    );
    /* if send_offset is odd, xor send_bit & exit */
    if (send_offset & 1) {
        send_bit ^= OUT_PINS;
        /* avoid 'else' to save a few cycles (rjmp) */
        asm("reti\n");
    }
    /* load next bit & exit */
    send_bit = em_bits[send_offset/2];
    asm("reti\n");
}

/* setting timer0 to Clear Timer on Compare mode every 32 cycles */
static void set_timers(void)
{
    /* timer0 in CTC mode and clear other bits */
    TCCR0A  = _BV(WGM01);
    /* timer0 no prescaling */
    TCCR0B  = _BV(CS00);
    /* enable timer0 compare interrupt */
    TIMSK  |= _BV(OCIE0A);
    /* set timer0 current counter */
    TCNT0   = 0;
    /* set timer0 for every xx cycles */
    OCR0A   = MAX_TIMER0;
}

/* an endless loop to send IDs from em_id_list[] & increment them. */
int main(void)
{
    /* initalizing the timer */
    set_timers();

    /* setting debugging led */
    DDRB   |= _BV(PINB0);
    PORTB  ^= _BV(PINB0);

    /* set startup values */
    send_offset     = 255;
    read_offset_id  = 0;
    send_counter    = MAX_SEND_COUNTER;

    /* writing EM41xx header & footer to em_bits[] only once */
    write_em_header_footer();
    
    while (1) {
        /* are we at the end of em_bits[] ? */
        if (send_offset >= (sizeof(em_bits) * 2)) {

            /* set offset to zero & send_bit to 1st bit in buffer */
            send_offset = 0;
            send_bit = em_bits[send_offset];

            /* have we sent current ID enough times? */
            if (++send_counter >= MAX_SEND_COUNTER) {

                /* disable interrupts so nothing is sent atm */
                cli();

                /* reset counter */
                send_counter = 0;
                
                /* write the current ID in the list */
                write_em_id();

                /* increment the current ID in the list */
                inc_em_id();

                /* proceed to next ID in em_id_list[] */
                read_offset_id += 5;

                /* are we at the end of em_id_list[] ? */
                if (read_offset_id >= sizeof(em_id_list)) {
                    /* reset to 1st ID in em_id_list[] */
                    read_offset_id = 0;
                }

                /* enable interrupts - we're sending again, hooray! */
                sei();
            }

            /* toggle debug led - we're sending a new ID */
            PORTB ^= _BV(PINB0);
        }
    }

    /* we shouldn't be getting here. so long, and thanks for all the fish. */
    return 42;
}
