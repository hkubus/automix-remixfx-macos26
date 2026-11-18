// planner27.h — macOS 27 selection rules for 26's beat-matched planner (see planner27.c, RULES27.md)
#pragma once
// option (a): slot (8, 9 or 12) of the served catalog -> the macOS 27 style it carries ("... [27:NN]")
void p27_set_slot_style(int slot, int style);
// option (b): the served catalog has macOS 27 style `id` (15-20) at its real ID; 26's BMFCF then evaluates it
void p27_add_style(int id);
// patch 26's _SonicKit_MusicKit_Packages in this process. Returns the number of hooks (>0) or <0 if nothing was
// patched (unknown build, call-site mismatch, protection failure). verbose: 0 errors, 1 decisions, 2 inputs,
// 3 every candidate score.
int  p27_install(int verbose);
