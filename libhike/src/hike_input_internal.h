/* The input parser, separated from the file descriptor that feeds it.
 *
 * Reading bytes and understanding bytes are two different jobs, and only one
 * of them can be tested. A terminal cannot be driven from a test suite: there
 * is no controlled way to make a real tty deliver "ESC [ 1 ; 5 A" split after
 * the semicolon at a chosen moment. So the state machine below takes bytes
 * from anywhere, and hike_poll is a thin loop that gets bytes from a
 * descriptor and hands them over. Everything interesting is on this side of
 * the line, where a test can feed one byte at a time.
 *
 * This header is internal. It is not installed, and nothing in hike.h refers
 * to it. */
#ifndef HIKE_INPUT_INTERNAL_H
#define HIKE_INPUT_INTERNAL_H

#include "hike/hike.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned char* buf;     /* bytes seen but not yet resolved into events */
    size_t len, cap;

    char* paste;            /* accumulated body of a bracketed paste */
    size_t paste_len, paste_cap;
    bool in_paste;
} hike_input_parser;

void hike_input_init(hike_input_parser* p);
void hike_input_free(hike_input_parser* p);

/* Adds bytes to the stream. Any number, at any boundary: a sequence split
 * across two calls parses the same as one delivered whole. */
void hike_input_feed(hike_input_parser* p, const void* bytes, size_t n);

/* Takes the next complete event, returning false when the buffered bytes do
 * not yet make one.
 *
 * `flush` says that no more bytes are coming soon -- the caller waited out the
 * escape timeout and nothing arrived. It is what turns a pending, ambiguous
 * ESC into a real Escape key; see the discussion in hike_input.c. */
bool hike_input_next(hike_input_parser* p, hike_event* out, bool flush);

/* How long hike_poll waits for the rest of a sequence after a bare ESC. */
#define HIKE_ESC_TIMEOUT_MS 50

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HIKE_INPUT_INTERNAL_H */
