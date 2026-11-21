/* yunix/apps/yvim.c - yvim, a small modal text editor in the style of vi.
 *
 * Usage, from the shell:  yvim <file>
 *
 *   Normal mode   h j k l / arrows   move
 *                 0 $  Home End      start / end of the line
 *                 gg G               first / last line
 *                 x  Delete          delete the character under the cursor
 *                 dd                 delete the line
 *                 i a I A o O        start typing (before, after, line start,
 *                                    line end, new line below, new line above)
 *                 :                  command line
 *   Insert mode   type text, Enter, Backspace, Tab (4 spaces), Esc to leave
 *   Commands      :w  :q  :q!  :wq  :x
 *
 * yvim is a Yunix program: it only uses what seaweed.h offers, like any
 * other code above the kernel.
 *
 * The whole file sits in one flat array of characters, with '\n' between
 * lines and the cursor as an index into that array. Fish caps a file at
 * FISH_FILE_MAX bytes, so there is no point in anything cleverer: moving
 * a few kilobytes with memmove on every keystroke is instant.
 */

#include "seaweed.h"
#include "string.h"
#include "apps.h"

#define TEXT_ROWS   (VGA_HEIGHT - 1)     /* the last row is the status line */
#define TEXT_MAX    (FISH_FILE_MAX - 1)  /* leave room for the final newline */
#define CMD_MAX     64
#define TAB_WIDTH   4

enum mode { MODE_NORMAL, MODE_INSERT, MODE_COMMAND };

static char      text[TEXT_MAX];
static size_t    length;
static size_t    cursor;        /* index into text */
static size_t    top;           /* first line on the screen */
static size_t    left;          /* first column on the screen */
static size_t    want_col;      /* the column j and k try to keep */
static enum mode mode;
static int       dirty;
static int       running;
static int       pending;       /* first key of dd or gg, or 0 */

static char      filename[FISH_NAME_MAX];
static char      command[CMD_MAX];
static size_t    command_len;
static char      message[VGA_WIDTH + 1];
static int       message_is_error;

/* --- small helpers --- */

static void set_message(const char *text_, int is_error)
{
    size_t i;

    for (i = 0; text_[i] != '\0' && i < VGA_WIDTH; i++) message[i] = text_[i];
    message[i] = '\0';
    message_is_error = is_error;
}

/* Appends 'add' to 'buffer' (of size 'size') and returns the new length. */
static size_t append(char *buffer, size_t pos, size_t size, const char *add)
{
    for (size_t i = 0; add[i] != '\0' && pos + 1 < size; i++) buffer[pos++] = add[i];
    buffer[pos] = '\0';
    return pos;
}

static size_t append_number(char *buffer, size_t pos, size_t size, size_t value)
{
    char digits[12];

    utoa((unsigned int)value, digits, 10);
    return append(buffer, pos, size, digits);
}

/* --- finding lines in the flat buffer --- */

static size_t line_start(size_t pos)
{
    while (pos > 0 && text[pos - 1] != '\n') pos--;
    return pos;
}

/* Index of the '\n' that ends the line holding 'pos', or 'length'. */
static size_t line_end(size_t pos)
{
    while (pos < length && text[pos] != '\n') pos++;
    return pos;
}

static size_t line_number(size_t pos)
{
    size_t n = 0;

    for (size_t i = 0; i < pos; i++) if (text[i] == '\n') n++;
    return n;
}

static size_t line_count(void)
{
    return line_number(length) + 1;
}

/* Index where line 'n' starts, or 'length' if there is no such line. */
static size_t line_offset(size_t n)
{
    size_t pos = 0;

    while (n > 0 && pos < length) {
        if (text[pos] == '\n') n--;
        pos++;
    }
    return n == 0 ? pos : length;
}

static size_t cursor_col(void)
{
    return cursor - line_start(cursor);
}

/* In normal mode the cursor sits on a character, never just past the end
 * of a line - unless the line is empty. */
static void clamp_normal(void)
{
    size_t start = line_start(cursor);
    size_t end = line_end(cursor);

    if (end > start && cursor >= end) cursor = end - 1;
}

/* --- editing --- */

static int insert_char(char c)
{
    if (length >= TEXT_MAX) {
        set_message("File is full (Fish files hold 4096 bytes)", 1);
        return 0;
    }

    memmove(text + cursor + 1, text + cursor, length - cursor);
    text[cursor++] = c;
    length++;
    dirty = 1;
    return 1;
}

/* Removes 'count' characters starting at 'pos'. */
static void delete_range(size_t pos, size_t count)
{
    if (pos >= length) return;
    if (count > length - pos) count = length - pos;

    memmove(text + pos, text + pos + count, length - pos - count);
    length -= count;
    dirty = 1;
}

static void delete_line(void)
{
    size_t start = line_start(cursor);
    size_t end = line_end(cursor);

    if (length == 0) return;

    /* Take the newline with it: the one after the line, or for the last
     * line the one before it. */
    if (end < length)     end++;
    else if (start > 0)   start--;

    delete_range(start, end - start);

    cursor = start < length ? start : length;
    cursor = line_start(cursor);
}

/* --- moving --- */

static void move_vertical(int down)
{
    size_t start = line_start(cursor);
    size_t target, target_len;

    if (down) {
        size_t end = line_end(cursor);
        if (end >= length) return;          /* already on the last line */
        target = end + 1;
    } else {
        if (start == 0) return;             /* already on the first line */
        target = line_start(start - 1);
    }

    target_len = line_end(target) - target;
    cursor = target + (want_col < target_len ? want_col : target_len);
    if (mode == MODE_NORMAL) clamp_normal();
}

/* Keys that move the cursor the same way in normal and insert mode.
 * Returns 1 if the key was one of them. */
static int handle_motion(int key)
{
    switch (key) {
    case KEY_LEFT:
        if (cursor > line_start(cursor)) cursor--;
        break;
    case KEY_RIGHT:
        if (cursor < line_end(cursor) &&
            (mode == MODE_INSERT || cursor + 1 < line_end(cursor))) cursor++;
        break;
    case KEY_UP:    move_vertical(0); return 1;   /* keep want_col */
    case KEY_DOWN:  move_vertical(1); return 1;
    case KEY_HOME:  cursor = line_start(cursor); break;
    case KEY_END:
        cursor = line_end(cursor);
        if (mode == MODE_NORMAL) clamp_normal();
        break;
    default:
        return 0;
    }

    want_col = cursor_col();
    return 1;
}

/* --- saving --- */

static int save_file(void)
{
    static char out[FISH_FILE_MAX + 1];
    char note[VGA_WIDTH + 1];
    size_t n = 0;
    int result;

    /* Files end with a newline, as on any Unix. The buffer leaves room for it. */
    memcpy(out, text, length);
    n = length;
    if (n > 0) out[n++] = '\n';
    out[n] = '\0';

    result = fish_create(filename);
    if (result < 0 && result != FISH_ERR_EXISTS) {
        set_message(fish_error(result), 1);
        return 0;
    }

    result = fish_write(filename, out);
    if (result < 0) {
        set_message(fish_error(result), 1);
        return 0;
    }

    dirty = 0;

    n = 0;
    n = append(note, n, sizeof(note), "\"");
    n = append(note, n, sizeof(note), filename);
    n = append(note, n, sizeof(note), "\" ");
    n = append_number(note, n, sizeof(note), length > 0 ? line_count() : 0);
    n = append(note, n, sizeof(note), "L, ");
    n = append_number(note, n, sizeof(note), length > 0 ? length + 1 : 0);
    n = append(note, n, sizeof(note), "B written");
    set_message(note, 0);
    return 1;
}

/* --- the command line --- */

static void run_command(void)
{
    command[command_len] = '\0';

    if (strcmp(command, "w") == 0) {
        save_file();
    } else if (strcmp(command, "q") == 0) {
        if (dirty) set_message("No write since last change (add ! to override)", 1);
        else       running = 0;
    } else if (strcmp(command, "q!") == 0) {
        running = 0;
    } else if (strcmp(command, "wq") == 0 || strcmp(command, "x") == 0) {
        if (save_file()) running = 0;
    } else if (command_len > 0) {
        char note[VGA_WIDTH + 1];
        size_t n = append(note, 0, sizeof(note), "Not an editor command: ");
        append(note, n, sizeof(note), command);
        set_message(note, 1);
    }

    mode = MODE_NORMAL;
}

static void handle_command_key(int key)
{
    if (key == 27) {                       /* Esc: cancel */
        mode = MODE_NORMAL;
    } else if (key == '\n') {
        run_command();
    } else if (key == '\b') {
        if (command_len == 0) mode = MODE_NORMAL;
        else command_len--;
    } else if (key >= ' ' && key < 127 && command_len < CMD_MAX - 1) {
        command[command_len++] = (char)key;
    }
}

/* --- insert mode --- */

static void handle_insert_key(int key)
{
    if (key == 27) {                       /* Esc: back to normal mode */
        mode = MODE_NORMAL;
        if (cursor > line_start(cursor)) cursor--;
        clamp_normal();
        want_col = cursor_col();
        return;
    }

    if (handle_motion(key)) return;

    if (key == '\b') {
        if (cursor > 0) {
            cursor--;
            delete_range(cursor, 1);       /* at a line start this joins lines */
        }
    } else if (key == KEY_DELETE) {
        delete_range(cursor, 1);
    } else if (key == '\t') {
        for (int i = 0; i < TAB_WIDTH; i++) if (!insert_char(' ')) break;
    } else if (key == '\n' || (key >= ' ' && key < 127)) {
        insert_char((char)key);
    }

    want_col = cursor_col();
}

/* --- normal mode --- */

static void handle_normal_key(int key)
{
    /* The second key of dd or gg. Anything else cancels the first. */
    if (pending != 0) {
        int first = pending;
        pending = 0;

        if (first == 'd' && key == 'd') delete_line();
        if (first == 'g' && key == 'g') cursor = 0;
        want_col = cursor_col();
        return;
    }

    if (handle_motion(key)) return;

    switch (key) {
    case 'h': handle_motion(KEY_LEFT);  return;
    case 'l': handle_motion(KEY_RIGHT); return;
    case 'j': handle_motion(KEY_DOWN);  return;
    case 'k': handle_motion(KEY_UP);    return;
    case '0': handle_motion(KEY_HOME);  return;
    case '$': handle_motion(KEY_END);   return;

    case 'G':
        cursor = line_start(length);
        break;

    case 'd':
    case 'g':
        pending = key;
        return;

    case 'x':
    case KEY_DELETE:
        if (cursor < line_end(cursor)) delete_range(cursor, 1);
        break;

    case 'i':
        mode = MODE_INSERT;
        break;
    case 'a':
        if (cursor < line_end(cursor)) cursor++;
        mode = MODE_INSERT;
        break;
    case 'I':
        cursor = line_start(cursor);
        mode = MODE_INSERT;
        break;
    case 'A':
        cursor = line_end(cursor);
        mode = MODE_INSERT;
        break;
    case 'o':
        cursor = line_end(cursor);
        if (insert_char('\n')) mode = MODE_INSERT;
        break;
    case 'O':
        cursor = line_start(cursor);
        if (insert_char('\n')) { cursor--; mode = MODE_INSERT; }
        break;

    case ':':
        mode = MODE_COMMAND;
        command_len = 0;
        break;

    default:
        break;
    }

    if (mode == MODE_NORMAL) clamp_normal();
    want_col = cursor_col();
}

/* --- drawing --- */

static void draw_text(const char *s, size_t x, size_t y,
                      enum vga_color fg, enum vga_color bg)
{
    for (size_t i = 0; s[i] != '\0' && x + i < VGA_WIDTH; i++) {
        vga_put_at(x + i, y, s[i], fg, bg);
    }
}

static void draw(void)
{
    size_t line = line_number(cursor);
    size_t col = cursor_col();
    size_t pos, lines = line_count();
    char status[VGA_WIDTH + 1];
    size_t n;

    /* Scroll so the cursor stays on the screen. */
    if (line < top) top = line;
    if (line >= top + TEXT_ROWS) top = line - TEXT_ROWS + 1;
    if (col < left) left = col;
    if (col >= left + VGA_WIDTH) left = col - VGA_WIDTH + 1;

    pos = line_offset(top);
    for (size_t row = 0; row < TEXT_ROWS; row++) {
        size_t x = 0;

        if (top + row >= lines) {
            /* Past the end of the file, vi draws a tilde. */
            vga_put_at(0, row, '~', VGA_LIGHT_BLUE, VGA_BLACK);
            x = 1;
        } else {
            size_t end = line_end(pos);

            for (size_t i = pos + left; i < end && x < VGA_WIDTH; i++, x++) {
                char c = text[i];
                if (c < ' ' || c > '~') c = ' ';   /* tabs and the like */
                vga_put_at(x, row, c, VGA_LIGHT_GREY, VGA_BLACK);
            }
            pos = end < length ? end + 1 : length;
        }

        for (; x < VGA_WIDTH; x++) vga_put_at(x, row, ' ', VGA_LIGHT_GREY, VGA_BLACK);
    }

    /* The status line. */
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        vga_put_at(x, TEXT_ROWS, ' ', VGA_LIGHT_GREY, VGA_BLACK);
    }

    if (mode == MODE_COMMAND) {
        vga_put_at(0, TEXT_ROWS, ':', VGA_WHITE, VGA_BLACK);
        for (size_t i = 0; i < command_len && i + 1 < VGA_WIDTH; i++) {
            vga_put_at(i + 1, TEXT_ROWS, command[i], VGA_WHITE, VGA_BLACK);
        }
        vga_move_cursor(command_len + 1 < VGA_WIDTH ? command_len + 1 : VGA_WIDTH - 1,
                        TEXT_ROWS);
        return;
    }

    if (message[0] != '\0') {
        draw_text(message, 0, TEXT_ROWS,
                  message_is_error ? VGA_WHITE : VGA_LIGHT_GREY,
                  message_is_error ? VGA_RED   : VGA_BLACK);
    } else if (mode == MODE_INSERT) {
        draw_text("-- INSERT --", 0, TEXT_ROWS, VGA_WHITE, VGA_BLACK);
    }

    /* Right side: name, [+] when there are unsaved changes, line,column. */
    n = append(status, 0, sizeof(status), filename);
    if (dirty) n = append(status, n, sizeof(status), " [+]");
    n = append(status, n, sizeof(status), "  ");
    n = append_number(status, n, sizeof(status), line + 1);
    n = append(status, n, sizeof(status), ",");
    n = append_number(status, n, sizeof(status), col + 1);
    if (n < VGA_WIDTH - 40) {
        draw_text(status, VGA_WIDTH - 1 - n, TEXT_ROWS, VGA_LIGHT_CYAN, VGA_BLACK);
    }

    vga_move_cursor(col - left, line - top);
}

/* --- starting and stopping --- */

/* Is there a directory called 'name' here? fish_read cannot tell a
 * directory from a missing file, so look through the listing. */
static int is_directory(const char *name)
{
    int handle = fish_first();
    const char *entry;
    int is_dir;

    while ((handle = fish_next(handle, &entry, &is_dir, 0)) != 0) {
        if (strcmp(entry, name) == 0) return is_dir;
    }
    return 0;
}

static int open_file(const char *name)
{
    const char *data;
    size_t size = 0;

    if (strlen(name) >= FISH_NAME_MAX) {
        vga_write("yvim: file name is too long\n");
        return 0;
    }
    if (is_directory(name)) {
        vga_write("yvim: is a directory\n");
        return 0;
    }

    strcpy(filename, name);
    length = 0;

    data = fish_read(name, &size);
    if (data != 0) {
        /* Drop the final newline; save_file puts it back. */
        if (size > 0 && data[size - 1] == '\n') size--;
        if (size > TEXT_MAX) size = TEXT_MAX;
        memcpy(text, data, size);
        length = size;
        set_message("", 0);
    } else {
        set_message("[New File]", 0);
    }

    return 1;
}

int yvim_run(const char *name)
{
    if (name[0] == '\0') {
        vga_write("Usage: yvim <file>\n");
        return 1;
    }
    if (!open_file(name)) return 1;

    cursor = 0;
    top = 0;
    left = 0;
    want_col = 0;
    mode = MODE_NORMAL;
    dirty = 0;
    pending = 0;
    running = 1;

    while (running) {
        int key;

        draw();

        while ((key = keyboard_getchar()) == 0) power_idle();

        /* A message stays up until the next key. */
        if (mode != MODE_COMMAND) message[0] = '\0';

        switch (mode) {
        case MODE_NORMAL:  handle_normal_key(key);  break;
        case MODE_INSERT:  handle_insert_key(key);  break;
        case MODE_COMMAND: handle_command_key(key); break;
        }
    }

    /* Hand the screen back to the shell. */
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
    return 0;
}
