/*
 * Breakbeat Generator Module UI
 *
 * Fresh start: Uses Schwung's official menu system as the main UI.
 * Does not wrap sound_generator_ui.mjs to avoid garbled text and conflicts.
 */

import { createEnum, createValue, createBack } from '/data/UserData/schwung/shared/menu_items.mjs';
import { createMenuState, handleMenuInput } from '/data/UserData/schwung/shared/menu_nav.mjs';
import { createMenuStack } from '/data/UserData/schwung/shared/menu_stack.mjs';
import { drawMenuList, drawMenuHeader, drawMenuFooter, menuLayoutDefaults, drawRect } from '/data/UserData/schwung/shared/menu_layout.mjs';

const g_loop_names = [
    "action", "amen", "apache", "around", "boogiewoogie", "delight", "do", "eeloil", "fireeater", "funkydrummer", "groove", "hitormiss", "hotline", "hungup_0", "hungup_1", "impeach", "king", "kool", "marymary", "mechanicalman", "movement", "newday", "neworleans", "riffin", "rill", "ripple", "sesame", "sneakin", "sport", "squib", "swat", "think", "useme"
];

const length_options = ["0.25", "0.5", "1", "2", "4", "8"];

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* State */
let menuState;
let menuStack;
let needsRedraw = true;

/* Live performance overlay — shows held slice + active macros (e.g. "A:3 .5x
 * REV") while the user is manually triggering, and nothing otherwise. Driven by
 * the DSP "perf_status" param, which returns "" when no pad/macro is engaged.
 * Drawn with the same primitives as the host's shift-knob overlay (there is no
 * shared overlay factory module on device): a centred, blanked, bordered box.
 * Visibility is tracked via non-empty perf_status, so it persists exactly while
 * held (no auto-fade), which suits momentary perf pads. */
let lastFx = '';

/* Draw the live FX overlay box, mirroring the shift-knob overlay look. Uses the
 * global draw primitives (print/fill_rect) and drawRect from menu_layout. */
function drawFxOverlay(text) {
    const bw = 110, bh = 28;
    const bx = Math.floor((SCREEN_WIDTH - bw) / 2);
    const by = Math.floor((SCREEN_HEIGHT - bh) / 2);

    fill_rect(bx, by, bw, bh, 0);   /* blank the menu behind the box */
    drawRect(bx, by, bw, bh, 1);    /* border                       */

    const label = 'LIVE';
    const labelX = Math.floor((SCREEN_WIDTH - label.length * 6) / 2);
    print(labelX, by + 5, label, 1);

    const t = String(text || '');
    const textX = Math.floor((SCREEN_WIDTH - t.length * 6) / 2);
    print(textX, by + 16, t, 1);
}

/* Initialize */
globalThis.init = function() {
    console.log("Breakbeat UI starting fresh...");

    menuState = createMenuState();
    menuStack = createMenuStack();

    /* Define parameters menu */
    const paramsMenu = [
        createEnum('Loop', {
            get: () => host_module_get_param('loop') || '0',
            set: (v) => host_module_set_param('loop', v),
            options: g_loop_names
        }),
        createEnum('Length', {
            get: () => host_module_get_param('length') || '2',
            set: (v) => host_module_set_param('length', v),
            options: length_options
        }),
        createValue('Complexity', {
            get: () => parseInt(host_module_get_param('complexity')) || 50,
            set: (v) => host_module_set_param('complexity', String(v)),
            min: 0, max: 100, step: 5
        })
    ];

    menuStack.push({ title: 'Breakbeat', items: paramsMenu });

    lastFx = '';

    needsRedraw = true;
    console.log("Breakbeat UI ready");
};

/* Tick */
globalThis.tick = function() {
    /* Poll the live performance state each tick. Redraw whenever it changes:
     * appearing, changing tokens, or disappearing on release. */
    const fx = host_module_get_param('perf_status') || '';
    if (fx !== lastFx) {
        lastFx = fx;
        needsRedraw = true;
    }

    if (needsRedraw) {
        clear_screen();

        const current = menuStack.current();
        drawMenuHeader(current.title);

        drawMenuList({
            items: current.items,
            selectedIndex: menuState.selectedIndex,
            listArea: {
                topY: menuLayoutDefaults.listTopY,
                bottomY: menuLayoutDefaults.listBottomWithFooter
            },
            valueAlignRight: true,
            getLabel: (item) => item.label,
            getValue: (item) => {
                // Get current value for display
                if (item.get) {
                    const val = item.get();
                    if (item.options) {
                        const idx = parseInt(val);
                        return item.options[idx] || val;
                    }
                    return String(val);
                }
                return "";
            }
        });

        drawMenuFooter("Jog:scroll Click:edit");

        /* Draw the live FX overlay last so it sits on top of the menu. Only
         * while a pad/macro is held (perf_status non-empty). */
        if (lastFx) {
            drawFxOverlay(lastFx);
        }

        needsRedraw = false;
    }
};

/* MIDI Input */
globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const cc = data[1];
    const value = data[2];

    if (status === 0xB0) {  // CC message
        const current = menuStack.current();
        const result = handleMenuInput({
            cc, value,
            items: current.items,
            state: menuState,
            stack: menuStack,
            shiftHeld: false,
            onBack: () => { /* Can't go back from root */ }
        });

        if (result.needsRedraw) {
            needsRedraw = true;
        }
    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* External MIDI goes directly to DSP */
};
