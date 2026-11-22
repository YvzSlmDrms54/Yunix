/* seaweed/kernel/isr.c - what actually runs when an interrupt fires.
 *
 * Both C functions here are called from boot/isr.s.
 */

#include "idt.h"
#include "io.h"
#include "vga.h"
#include "paging.h"
#include "program.h"

static const char *exception_names[32] = {
    "Divide by zero",          "Debug",
    "Non-maskable interrupt",  "Breakpoint",
    "Overflow",                "Bound range exceeded",
    "Invalid opcode",          "Device not available",
    "Double fault",            "Coprocessor segment overrun",
    "Invalid TSS",             "Segment not present",
    "Stack-segment fault",     "General protection fault",
    "Page fault",              "Reserved",
    "x87 floating point",      "Alignment check",
    "Machine check",           "SIMD floating point",
    "Virtualization",          "Control protection",
    "Reserved",                "Reserved",
    "Reserved",               "Reserved",
    "Reserved",               "Reserved",
    "Hypervisor injection",   "VMM communication",
    "Security",               "Reserved"
};

/* One handler slot per IRQ. NULL means nobody registered one. */
static irq_handler_t irq_handlers[16] = { 0 };

void irq_install_handler(int num, irq_handler_t handler)
{
    if (num >= 0 && num < 16) irq_handlers[num] = handler;
}

/* Prints a value as 0x followed by exactly eight hex digits. */
static void write_hex(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[11];

    text[0] = '0';
    text[1] = 'x';
    for (int i = 0; i < 8; i++) {
        text[2 + i] = digits[(value >> (28 - 4 * i)) & 0xF];
    }
    text[10] = '\0';
    vga_write(text);
}

/* A page fault's error code says why the access failed. */
static void explain_page_fault(uint32_t err_code)
{
    uint32_t address = paging_fault_address();

    vga_write("Address: ");
    write_hex(address);
    if (address < 0x1000) vga_write("  (a NULL pointer)");
    vga_putchar('\n');

    vga_write("Reason:  ");
    vga_write((err_code & 0x1) ? "protection violation" : "page not mapped");
    vga_write((err_code & 0x2) ? ", on a write" : ", on a read");
    vga_write((err_code & 0x4) ? ", from user mode" : ", from the kernel");
    if (err_code & 0x10) vga_write(", fetching code");
    vga_putchar('\n');
}

/* A ring 3 program broke a rule: the kernel itself is fine, so stop the
 * program - with the status Linux would give it - and carry on. */
static void kill_program(struct registers *regs)
{
    uint32_t status = STATUS_SIGSEGV;

    if (regs->int_no == 0)  status = STATUS_SIGFPE;     /* divide by zero */
    if (regs->int_no == 6)  status = STATUS_SIGILL;     /* invalid opcode */

    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    vga_write("\n");
    vga_write(program_current_name());
    vga_write(" was stopped: ");
    vga_write(regs->int_no < 32 ? exception_names[regs->int_no] : "unknown exception");
    vga_write(" at ");
    write_hex(regs->eip);
    vga_putchar('\n');
    if (regs->int_no == 14) explain_page_fault(regs->err_code);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    program_end(status);
}

/* Called for CPU exceptions (interrupts 0-31). */
void isr_handler(struct registers *regs)
{
    /* The low two bits of the saved CS say which ring was running. */
    if ((regs->cs & 3) == 3 && program_running()) kill_program(regs);

    vga_set_color(VGA_WHITE, VGA_RED);
    vga_write("\n*** KERNEL PANIC ***\n");

    if (regs->int_no < 32) {
        vga_write(exception_names[regs->int_no]);
    } else {
        vga_write("Unknown exception");
    }
    vga_putchar('\n');

    /* Where it happened. Look the address up with
     *     objdump -d seaweed.bin | grep -B5 <address without 0x>
     * to find the instruction, and the function it is in. */
    vga_write("EIP:     ");
    write_hex(regs->eip);
    vga_write("   Error code: ");
    write_hex(regs->err_code);
    vga_putchar('\n');

    if (regs->int_no == 14) explain_page_fault(regs->err_code);

    vga_write("System halted.\n");

    /* Nothing sensible left to do. Stop for good. */
    for (;;) __asm__ volatile ("cli; hlt");
}

/* Called for hardware interrupts (32-47). */
void irq_handler(struct registers *regs)
{
    int irq = (int)regs->int_no - 32;

    if (irq >= 0 && irq < 16 && irq_handlers[irq] != 0) {
        irq_handlers[irq](regs);
    }

    /* Tell the controllers we are done, or they will never send again.
     * IRQs 8-15 come through the slave, so it needs telling too. */
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}