#include <stdbool.h>
#include <stdint.h>

#include "hal/usb_serial_jtag_ll.h"

// Keep the IDF-initialized USB Serial/JTAG clock, PHY, and pins unchanged.
// This early path uses neither ROM printf, RTOS drivers, nor heap allocation.
bool shizuku_c6_usb_console_write_and_flush(const char *text, unsigned length)
{
    // Polling below is bounded by iterations, not by elapsed time.
    const unsigned spin_limit = 1000000;
    unsigned sent = 0;
    unsigned stalled = 0;

    while (sent < length) {
        if (!usb_serial_jtag_ll_txfifo_writable()) {
            unsigned spins = 0;
            while (!usb_serial_jtag_ll_txfifo_writable() && spins++ < spin_limit) {
            }
            if (spins > spin_limit) {
                return false;
            }
        }

        const int written = usb_serial_jtag_ll_write_txfifo((const uint8_t *)(text + sent), length - sent);
        if (written <= 0) {
            if (++stalled >= spin_limit) {
                return false;
            }
        } else {
            sent += (unsigned)written;
            stalled = 0;
        }
    }

    // Clear only after queuing this marker, so a previous packet's completion cannot satisfy this send.
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    usb_serial_jtag_ll_txfifo_flush();
    __asm__ volatile("fence iorw, iorw" ::: "memory");

    // spin_limit is an iteration cap, not an elapsed-time deadline.
    for (unsigned spins = 0; spins < spin_limit; ++spins) {
        if (usb_serial_jtag_ll_get_intraw_mask() & USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY) {
            return true;
        }
    }
    return false;
}
