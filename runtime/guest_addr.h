// (SMRPG Android) Where the game lives in the process's memory (the game's addresses are the process's: there is no
// translation). The game's addresses are written as usual, the Switch's as this port loads them (main at 0x7000000000,
// the sdk at 0x7005000000...), and GADDR() takes them to where the game really is: GUEST_SHIFT bytes lower. On PC and
// on Android x86_64, 0: nothing changes. On Android arm64 a process's address space is 39 bits (512 GB) and the usual
// window, 0x70_0000_0000-0x80_0000_0000, is exactly where Android puts its libraries, its stacks and its memory: there
// the game goes 0x60_0000_0000 lower (the Makefile passes -DGUEST_SHIFT=0x6000000000ull), to
// 0x10_0000_0000-0x20_0000_0000, which is free on Android.
// The generated code is made with the bases already moved (tools/gen.py <base>), and its functions keep their usual
// names (gen.py --name-shift: f_<usual address>), so the fixes (tools/game_overrides.c) and the program still find them
// by name.
#pragma once

#ifndef GUEST_SHIFT
#define GUEST_SHIFT 0ull
#endif
#define GADDR(a) ((unsigned long long)(a) - (unsigned long long)(GUEST_SHIFT))
