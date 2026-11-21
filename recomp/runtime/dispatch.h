// Finds the code for a guest address: translated if there is any, interpreted if not.
#pragma once

void fill_code_slots(void);
// A module function's slot, kept with the module's number so dispatch can check the module
// is still where it was bound before running its code (modules.c).
void dispatch_register_module(uint32_t address, GuestFunction run, int module);
void dispatch_forget_module(uint32_t address, int module);  // Only if that module still owns it.
int modules_still_bound(int module);  // 0 once another module has taken its place; it's then forgotten.
