/* NovaOS - PS/2 keyboard driver */
#include "keyboard.h"
#include "io.h"
#include "idt.h"
#include "irq.h"

#define KB_BUF_SIZE 256
static volatile u16 kbuf[KB_BUF_SIZE];
static volatile u32 kbuf_head = 0, kbuf_tail = 0;

static int shift = 0, caps = 0;
static int extended = 0;      /* set after the 0xE0 prefix byte */

/* Extended (0xE0-prefixed) codes are reported to the shell in a private
 * range so a plain character can never collide with them: the shell sees
 * 0x100 + <base scancode>, e.g. 0x100+'A' for the up arrow.  Without this
 * the arrows produced nothing at all and the shell had no history. */
#define KEY_EXT 0x100

/* US QWERTY scan code set 1 -> ASCII */
static const char sc_plain[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0
};

static const char sc_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0
};

static void kb_enqueue(u8 c) {
    u32 next = (kbuf_head + 1) % KB_BUF_SIZE;
    if (next == kbuf_tail) return;   /* full: drop */
    kbuf[kbuf_head] = c;
    kbuf_head = next;
}

static void kb_enqueue_u16(u16 c) {
    u32 next = (kbuf_head + 1) % KB_BUF_SIZE;
    if (next == kbuf_tail) return;   /* full: drop */
    kbuf[kbuf_head] = c;
    kbuf_head = next;
}

static void keyboard_handler(struct regs *r) {
    (void)r;
    u8 sc = inb(0x60);

    if (sc == 0xE0) { extended = 1; return; }        /* prefix: next is ext */

    if (extended) {
        extended = 0;
        if (sc & 0x80) return;                       /* ext release: ignore */
        /* arrows / home / end / del are all we care about for now */
        switch (sc) {
        case 0x48: case 0x50:                        /* up, down   */
        case 0x4B: case 0x4D:                        /* left, right*/
        case 0x47: case 0x4F: case 0x49: case 0x51:  /* home,end,pgup,pgdn */
        case 0x53:                                   /* delete     */
            kb_enqueue_u16((u16)(KEY_EXT + sc));
            break;
        default:
            break;
        }
        return;
    }

    if (sc == 0x2A || sc == 0x36) shift = 1;         /* shift down */
    else if (sc == 0xAA || sc == 0xB6) shift = 0;    /* shift up */
    else if (sc == 0x3A) caps = !caps;
    else if (sc & 0x80) { /* key release: ignore */ }
    else {
        const char *map = shift ? sc_shift : sc_plain;
        char c = map[sc & 0x7F];
        if (c >= 'a' && c <= 'z' && caps) c = (char)(c - 32);
        if (c) kb_enqueue((u8)c);
    }
}

void keyboard_init(void) {
    register_irq_handler(1, keyboard_handler);
    irq_enable(1);
    /* flush any pending byte */
    if (inb(0x64) & 0x01) inb(0x60);
}

int keyboard_pending(void) {
    return kbuf_head != kbuf_tail;
}

int keyboard_getc(void) {
    if (kbuf_head == kbuf_tail) return 0;
    u16 c = kbuf[kbuf_tail];
    kbuf_tail = (kbuf_tail + 1) % KB_BUF_SIZE;
    return (int)c;
}
