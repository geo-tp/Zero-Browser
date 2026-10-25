#pragma once
#include <stdint.h>
// Private SOCK_SEQPACKET channel inherited only by the supervised NetSurf.
// No pointers, titles, or locale-dependent strings cross the boundary.
#define ZERO_GTK_PROTOCOL 1u
#define ZERO_GTK_OPEN_SEARCHBAR 0x5a534243u // unsolicited GTK -> host command
enum ZeroGtkKind { ZERO_GTK_UNKNOWN, ZERO_GTK_MAIN, ZERO_GTK_DOWNLOADS, ZERO_GTK_HISTORY, ZERO_GTK_SETTINGS, ZERO_GTK_DOWNLOADS_HIDDEN };
struct ZeroGtkQuery { uint32_t version, serial; };
struct ZeroGtkState {
    uint32_t version, serial, kind, modal, popup;
    char app_id[96], main_id[96];
};
