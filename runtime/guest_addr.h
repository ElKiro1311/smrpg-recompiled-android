// (SMRPG Android) Dónde vive el juego en la memoria del proceso (las direcciones del juego son las del proceso: no hay
// traducción). Las direcciones del juego se escriben como siempre, las de la Switch tal como las carga este port
// (main en 0x7000000000, el sdk en 0x7005000000...), y GADDR() las lleva a donde está el juego de verdad:
// GUEST_SHIFT bytes más abajo. En el PC y en Android x86_64, 0: nada cambia. En Android arm64 el espacio de direcciones
// de un proceso es de 39 bits (512 GB) y la ventana de siempre, 0x70_0000_0000-0x80_0000_0000, es justo donde Android
// pone sus bibliotecas, sus pilas y su memoria: ahí el juego va 0x60_0000_0000 más abajo (el Makefile pasa
// -DGUEST_SHIFT=0x6000000000ull), a 0x10_0000_0000-0x20_0000_0000, que en Android está libre.
// El código generado se hace con las bases ya movidas (tools/gen.py <base>), y sus funciones conservan los nombres de
// siempre (gen.py --name-shift: f_<dirección de siempre>), así que los arreglos (tools/game_overrides.c) y el programa
// las siguen encontrando por su nombre.
#pragma once

#ifndef GUEST_SHIFT
#define GUEST_SHIFT 0ull
#endif
#define GADDR(a) ((unsigned long long)(a) - (unsigned long long)(GUEST_SHIFT))
