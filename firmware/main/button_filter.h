#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_US 30000ULL

typedef enum { BUTTON_NO_EDGE, BUTTON_PRESSED, BUTTON_RELEASED } button_edge_t;
typedef struct {
    bool stable, candidate, armed;
    uint64_t changed_us;
} button_filter_t;
typedef struct { button_edge_t edge; bool activate; } button_change_t;

static inline void button_filter_init(button_filter_t *f, bool pressed, uint64_t now_us) {
    *f=(button_filter_t){.stable=pressed,.candidate=pressed,.changed_us=now_us};
}

// Called once per GPIO sample. Only an observed level change restarts this
// button's debounce interval; unchanged samples allow it to settle.
static inline button_change_t button_filter_update(button_filter_t *f, bool pressed, uint64_t now_us) {
    button_change_t change={0};
    if(pressed!=f->candidate) {
        f->candidate=pressed;
        f->changed_us=now_us;
    }
    if(f->candidate==f->stable || now_us<f->changed_us || now_us-f->changed_us<BUTTON_DEBOUNCE_US) return change;
    f->stable=f->candidate;
    change.edge=f->stable ? BUTTON_PRESSED : BUTTON_RELEASED;
    if(f->stable) {
        f->armed=true;
    } else {
        // A key held at boot must complete a fresh debounced press before
        // its release may activate anything. Initial input is never an action.
        change.activate=f->armed;
        f->armed=false;
    }
    return change;
}
