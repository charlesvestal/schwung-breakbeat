/*
 * Breakbeat Generator Module UI
 *
 * Fresh start: Uses Schwung's official menu system as the main UI.
 * Does not wrap sound_generator_ui.mjs to avoid garbled text and conflicts.
 */

import { createEnum, createValue, createBack } from '/data/UserData/schwung/shared/menu_items.mjs';
import { createMenuState, handleMenuInput } from '/data/UserData/schwung/shared/menu_nav.mjs';
import { createMenuStack } from '/data/UserData/schwung/shared/menu_stack.mjs';
import { drawMenuList, drawMenuHeader, drawMenuFooter, menuLayoutDefaults } from '/data/UserData/schwung/shared/menu_layout.mjs';

const g_loop_names = [
    "action", "amen", "apache", "around", "boogiewoogie", "delight", "do", "eeloil", "fireeater", "funkydrummer", "groove", "hitormiss", "hotline", "hungup_0", "hungup_1", "impeach", "king", "kool", "marymary", "mechanicalman", "movement", "newday", "neworleans", "riffin", "rill", "ripple", "sesame", "sneakin", "sport", "squib", "swat", "think", "useme"
];

const length_options = ["0.25", "0.5", "1", "2", "4", "8"];

/* State */
let menuState;
let menuStack;
let needsRedraw = true;

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
    needsRedraw = true;
    console.log("Breakbeat UI ready");
};

/* Tick */
globalThis.tick = function() {
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
