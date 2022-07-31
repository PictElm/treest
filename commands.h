#ifdef TREEST_COMMAND
#error This file should not be included outside treest.c
#endif // TREEST_COMMAND
#define TREEST_COMMAND(__x) {                                            \
    unsigned char user;                                                  \
    do {                                                                 \
        if (read(STDIN_FILENO, &user, 1) < 0) die("read");               \
    } while (127 < user || !command_map[user] || !command_map[user]());  \
}

#include "./treest.h"

#define putstr(__c) if (write(STDERR_FILENO, __c, strlen(__c)) < 0) die("write")
#define putln() putstr(is_raw ? "\r\n" : "\n")

#undef CTRL
#define CTRL(x) ( (~x&64) | (~x&64)>>1 | (x&31) )

#define TOGGLE(flag) flag = !(flag)

bool toggle_gflag(char flag) {
    switch (flag) {
        //case 'a':
        case 'A':
            TOGGLE(gflags.almost_all);
            dir_reload(&root);
            return true;
    }
    return false;
}

static char* prompt_raw(const char* c) {
    ssize_t cap = 1024;
    ssize_t len = 0;
    char* buf = malloc(cap * sizeof(char));

    char last;
    putstr(c);
    putstr(": ");
    while (true) {
        if (read(STDIN_FILENO, &last, 1) < 0) die("read");
        if (CTRL('C') == last || CTRL('D') == last || CTRL('G') == last || CTRL('[') == last) {
            putstr("- aborted");
            putln();
            return NULL;
        }
        if (CTRL('H') == last || CTRL('?') == last) {
            if (0 < len) {
                putstr("\b \b");
                len--;
            }
            continue;
        }
        if (CTRL('W') == last) {
            while (0 < len) {
                putstr("\b \b");
                len--;
                if (' ' == buf[len-1]) break;
            }
            continue;
        }
        if (CTRL('I') == last) continue;
        if (CTRL('J') == last || CTRL('M') == last) break;

        if (write(STDERR_FILENO, &last, 1) < 0) die("write")
        if (cap < len) {
            cap*= 2;
            buf = realloc(buf, cap * sizeof(char));
        }
        buf[len++] = last;
    }
    putln();
    buf[len] = '\0';

    return buf;
}

#ifdef FEAT_READLINE
#include <readline/readline.h>
#include <readline/history.h>
static char* prompt_rl(const char* c) {
    size_t len = strlen(c);
    char p[len+2];
    strcpy(p, c);
    strcpy(p+len, ": ");
    term_restore();
    char* r = readline(p);
    term_raw_mode();
    add_history(r);
    return r;
}

static char* prompt_sel(const char* c);
char* (* prompt)(const char* c) = prompt_sel;
static char* prompt_sel(const char* c) { return (prompt = isatty(STDIN_FILENO) ? prompt_rl : prompt_raw)(c); }
#else
#define prompt prompt_raw
#endif

static char prompt1(const char* c) {
    char r;
    putstr(c);
    putstr(": ");
    if (read(STDIN_FILENO, &r, 1) < 0) die("read");
    if (CTRL('C') == r || CTRL('D') == r || CTRL('G') == r || CTRL('J') == r || CTRL('M') == r || CTRL('[') == r) {
        putstr("- aborted");
        putln();
        return 0;
    }
    char w[2] = {r};
    putstr(w);
    putln();
    return r;
}

static struct Node* locate(const char* path) {
    if ('/' != *path) {
        putstr("! absolute path must start with a /");
        putln();
        return NULL;
    }

    ssize_t rlen = strlen(root.path);
    if (0 != memcmp(root.path, path, rlen)) {
        putstr("! unrelated root");
        putln();
        return NULL;
    }

    const char* cast = path + rlen;
    const char* head;
    bool istail = false;
    struct Node* curr = &root;
    do {
        head = cast+1;
        if (!(cast = strchr(head, '/'))) {
            cast = head + strlen(head);
            istail = true;
        }

        if (head == cast) continue;

        if ('.' == *head) {
            if ('/' == *(head+1) || '\0' == *(head+1)) continue;
            if ('.' == *(head+1) && ('/' == *(head+2) || '\0' == *(head+2))) {
                if (&root == curr) {
                    putstr("! '..' goes above root");
                    putln();
                    return NULL;
                }
                curr = curr->parent;
                continue;
            }
        }

        if (Type_DIR != curr->type) {
            putstr("! path element is not a directory");
            putln();
            return NULL;
        }

        if (!curr->as.dir.unfolded) {
            dir_unfold(curr);
            dir_fold(curr);
        }
        bool found = false;
        for (size_t k = 0; k < curr->count; k++) {
            struct Node* it = curr->as.dir.children[k];
            if (0 == memcmp(it->name, head, cast-head)) {
                found = true;
                curr = Type_LNK == it->type && it->as.link.tail
                    ? it->as.link.tail
                    : it;
                break;
            }
        }

        if (!found) {
            putstr("! path not found");
            putln();
            return NULL;
        }
    } while (!istail && *head);

    struct Node* up = curr;
    while (up != &root) {
        up = up->parent;
        dir_unfold(up);
    }

    return curr;
}

static char* quote(char* text) {
    size_t cap = 64;
    size_t len = 0;
    char* ab = malloc(cap * sizeof(char));

    ab[len++] = '\'';

    char* cast = text;
    while ((cast = strchr(text, '\''))) {
        size_t add = cast-text;
        if (cap < len+add+4) {
            cap*= 2;
            ab = realloc(ab, cap * sizeof(char));
        }
        memcpy(ab+len, text, add);
        len+= add;
        memcpy(ab+len, "'\\''", 4);
        len+= 4;
        text = cast+1;
    }
    size_t left = strlen(text);
    if (cap < len+left+2) ab = realloc(ab, (len+left+2) * sizeof(char));
    memcpy(ab+len, text, left);
    len+= left;

    ab[len++] = '\'';
    ab[len] = '\0';

    return ab;
}

static bool c_quit(void) {
    exit(EXIT_SUCCESS);
    return false;
}

static bool c_cquit(void) {
    exit(EXIT_FAILURE);
    return false;
}

static bool c_toggle(void) {
    char x = prompt1("toggle");
    if (x) {
        bool r = selected_printer->toggle(x);
        if (!r) {
            putstr("! no such flag");
            putln();
        }
        return r;
    }
    return false;
}

static bool c_refresh(void) {
    return true;
}

static bool c_previous(void) {
    struct Node* p = cursor->parent;
    if (p) {
        if (Type_LNK == p->type) p = p->as.link.tail;
        if (p && 0 < cursor->index) {
            cursor = p->as.dir.children[cursor->index - 1];
            return true;
        }
    }
    return false;
}

static bool c_next(void) {
    struct Node* p = cursor->parent;
    if (p) {
        if (Type_LNK == p->type) p = p->as.link.tail;
        if (p && cursor->index < p->count - 1) {
            cursor = p->as.dir.children[cursor->index + 1];
            return true;
        }
    }
    return false;
}

static bool c_child(void) {
    struct Node* d = cursor;
    if (Type_LNK == d->type) d = d->as.link.tail;
    if (d && Type_DIR == d->type) {
        dir_unfold(d);
        if (d->count) cursor = d->as.dir.children[0];
        return true;
    }
    return false;
}

static bool c_parent(void) {
    struct Node* p = cursor->parent;
    if (p) {
        cursor = p;
        return true;
    }
    return false;
}

static bool c_unfold(void) {
    struct Node* d = cursor;
    if (Type_LNK == d->type) d = d->as.link.tail;
    if (d && Type_DIR == d->type) {
        dir_unfold(d);
        return true;
    }
    return false;
}

static bool c_fold(void) {
    if (Type_DIR == cursor->type) {
        dir_fold(cursor);
        return true;
    }
    return false;
}

static void _recurse_foldall(struct Node* curr) {
    for (size_t k = 0; k < curr->count; k++) {
        struct Node* it = curr->as.dir.children[k];
        if (Type_LNK == it->type) it = it->as.link.tail;
        if (Type_DIR == it->type) _recurse_foldall(it);
    }
    dir_fold(curr);
}
static bool c_foldall(void) {
    _recurse_foldall(&root);
    dir_unfold(&root);
    cursor = &root;
    return true;
}

static bool c_promptunfold(void) {
    char* c = prompt("unfold-path");
    if (!c) return false;
    struct Node* found = locate(c);
    free(c);
    if (found) {
        struct Node* pre = cursor;
        cursor = found;
        c_unfold();
        cursor = pre;
        return true;
    }
    return false;
}

static bool c_promptfold(void) {
    char* c = prompt("fold-path");
    if (!c) return false;
    struct Node* found = locate(c);
    free(c);
    if (found) {
        struct Node* pre = cursor;
        cursor = found;
        c_fold();
        cursor = pre;
        return true;
    }
    return false;
}

static bool c_promptgounfold(void) {
    char* c = prompt("gounfold-path");
    if (!c) return false;
    struct Node* found = locate(c);
    free(c);
    if (found) {
        cursor = found;
        c_unfold();
        return true;
    }
    return false;
}

static bool c_promptgofold(void) {
    char* c = prompt("gofold-path");
    if (!c) return false;
    struct Node* found = locate(c);
    free(c);
    if (found) {
        cursor = found;
        c_fold();
        return true;
    }
    return false;
}

static bool c_command(void) {
    char* c = prompt("command");
    if (!c) return false;
    bool r = selected_printer->command(c);
    free(c);
    return r;
}

static bool c_shell(void) {
    if (0 == system(NULL)) {
        putstr("! no shell available");
        return false;
    }

    char* c = prompt("shell-command");
    if (!c) return false;
    ssize_t clen = strlen(c);

    char* quoted = quote(cursor->path);
    size_t nlen = strlen(quoted);

    char* com = malloc(clen * sizeof(char));
    char* into = com;

    char* head = c;
    char* tail;
    while ((tail = strstr(head, "{}"))) {
        memcpy(into, head, tail-head);
        into+= tail-head;

        clen+= nlen;
        char* pcom = com;
        com = realloc(com, clen * sizeof(char));
        into+= com - pcom;

        strcpy(into, quoted);
        into+= nlen;

        head = tail+2;
    }
    strcpy(into, head);
    free(c);

    term_restore();
    int _usl = system(com); // YYY
    free(com);
    term_raw_mode();

    putstr("! done");
    if (read(STDIN_FILENO, &_usl, 1) < 0) die("read");

    return true;
}

static bool c_pipe(void) {
    if (0 == system(NULL)) {
        putstr("! no shell available");
        return false;
    }

    char* c = prompt("pipe-command");
    if (!c) return false;
    ssize_t clen = strlen(c);

    char* quoted = quote(cursor->path);
    size_t nlen = strlen(quoted);

    clen+= nlen+1;

    char* com = malloc(clen * sizeof(char));
    char* into = com;

    char* head = c;
    char* tail;
    while ((tail = strstr(head, "{}"))) {
        memcpy(into, head, tail-head);
        into+= tail-head;

        clen+= nlen;
        char* pcom = com;
        com = realloc(com, clen * sizeof(char));
        into+= com - pcom;

        strcpy(into, quoted);
        into+= nlen;

        head = tail+2;
    }
    strcpy(into, head);
    free(c);

    into+= strlen(head);
    *into++ = '<';
    strcpy(into, quoted);
    *(into+nlen) = '\0';

    term_restore();
    int _usl = system(com); // YYY
    free(com);
    term_raw_mode();

    putstr("! done");
    if (read(STDIN_FILENO, &_usl, 1) < 0) die("read");

    return true;
}

// REM: `LC_ALL=C sort`
bool (* command_map[128])(void) = {
    [CTRL('C')]=c_quit,
    [CTRL('D')]=c_quit,
    [CTRL('L')]=c_refresh,
    ['!']=c_shell,
    ['-']=c_toggle,
    ['0']=c_foldall,
    [':']=c_command,
    ['C']=c_promptfold,
    ['H']=c_fold,
    ['L']=c_unfold,
    ['O']=c_promptunfold,
    ['Q']=c_cquit,
    ['c']=c_promptgofold,
    ['h']=c_parent,
    ['j']=c_next,
    ['k']=c_previous,
    ['l']=c_child,
    ['o']=c_promptgounfold,
    ['q']=c_quit,
    ['|']=c_pipe,
};

/*
static void do_command(char x) {
    switch (x) {
        case '^': break; // (prompt) go to basename starting with
        case '$': break; // (prompt) go to basename ending with
        case '/': break; // (prompt) go to basename matching

        case '?': break; // help? (prompt-1?)

        // TODO(fancy command override):
        //     deal properly with tree getting bigger
        //     than view (^E/^Y, ^N/^P, ^D/^U, ^F/^B)

        // https://stackoverflow.com/questions/51909557/mouse-events-in-terminal-emulator
        // but yeah, idk...
        case  5: // ^E (mouse down)
        case 25: // ^Y (mouse up)
            break;
    }
}
*/
