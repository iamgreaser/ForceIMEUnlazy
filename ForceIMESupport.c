// vim: set sts=2 sw=2 et :
//
// ForceIMESupport v002
// Written by GreaseMonkey, 2022-2023. I release this software into the public domain.
//
// This is an LD_PRELOAD injection library which forces poorly-written software to accept input from an IME
// In this case, said poorly-written software is Unity 2019.
// (How many bugs did you find? Scroll down with your mouse's scrollwheel to find out more.
//  ...oh whoops, it scrolled up instead.)
// More specifically, this is designed to force IME support into NeosVR.
//
// UPDATED KNOWLEDGE: Unity 2019 uses some version of SDL for its input on Linux.
//
// Here's what this does:
//
// - Prior to calling XOpenIM(), we set the locale up so that your IME knows it can be used.
//
// - We intercept XCreateIC() to ensure that it gives a "preedit nothing" context, which means that the IME can actually be used. (If you asked for PreeditNone, you probably can't handle preedit information.)
//
// - Some poorly-written software (e.g. Unity) calls Xutf8LookupString(), expecting it to return only one character, and then it proceeds to ignore the rest.
//   - We have a buffer for this which acts somewhat like a back-to-back pair of FIFO queue.
//   - We return 1 character, and then while this buffer still has stuff, we mess with other calls:
//     - XPending() returns True.
//     - XEventsQueued() returns 1 more than what's actually there.
//     - XFilterEvent() returns False if it's a KeyPress event with a keycode of None.
//     - XNextEvent() returns a dummy KeyPress with a keycode of None.
//
// I wouldn't call this particularly well-written at this point. But at least it does a better job of input than Unity does.
//
// KNOWN PROBLEMS:
// - If Unity has a low framerate, the event queue we present as XNextEvent() can get backlogged, even when a text field isn't focused. This means that a KeyRelease event can take many frames to actually arrive.
//   - We should probably be using something like XCheckMaskEvent() to grab KeyRelease events.
//   - NOTE: Unity handles mouse input via other means - that is, it doesn't use classic X11 for this (I'm guessing XInput2). TODO: Find out how, because Unity has plenty of bugs here, too! --GM
//

#define _GNU_SOURCE

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <locale.h>

//
// When calling Xutf8LookupString:
// Unity 2019 accepts as much data as it can, but then only uses the first character.
// This is NOT how an IME behaves on Linux - you get multiple characters in one call.
//
// We need to create a buffer to work around this.
//
#define MAX_BYTES_IN 4096
unsigned char text_string_buffer[MAX_BYTES_IN];
int text_string_used = 0;

//
// This is used as a dummy event to return from XNextEvent() when we need to pass more characters to Unity.
//
XEvent last_key_event;

//
// This is a helper function for dealing with UTF-8 data.
// It tells us the length of the character at &text_string_buffer[offset].
//
static int _buf_char_len(int offset) {
  if (text_string_buffer[offset] <= 0b01111111) {
    return 1;
  } else if (text_string_buffer[offset] <= 0b10111111) {
    // This is a broken character fragment
    text_string_buffer[offset] = '?';
    return 1;
  } else if (text_string_buffer[offset] <= 0b11011111) {
    return 2;
  } else if (text_string_buffer[offset] <= 0b11101111) {
    return 3;
  } else if (text_string_buffer[offset] <= 0b11110111) {
    return 4;
  } else {
    // This is a broken character fragment
    text_string_buffer[offset] = '?';
    return 1;
  }
}

//
// We wouldn't normally need to intercept Xutf8LookupString(), but Unity is a poorly-written piece of software.
// So, when the real function inevitably returns multiple UTF-8 characters, we have to do return one at a time.
// We also need to cooperate with our XNextEvent() shim so that said shim can report more events.
//
int Xutf8LookupString(XIC ic, XKeyPressedEvent *event, char *buffer_return, int bytes_buffer, KeySym *keysym_return, Status *status_return)
{
  // This is only designed to work for programs which support UTF-8.
  // If you give us a buffer of less than 4 bytes, we can't do much here!
  assert(bytes_buffer >= 4);

  static int (*real)(XIC ic, XKeyPressedEvent *event, char *buffer_return, int bytes_buffer, KeySym *keysym_return, Status *status_return) = NULL;
  if (real == NULL) { real = dlsym(RTLD_NEXT, "Xutf8LookupString"); }
  if (real == NULL) { abort(); };

  int added = 0;
  if (text_string_used == 0) {
    added = real(ic, event, (char *)&text_string_buffer[text_string_used], MAX_BYTES_IN - text_string_used, keysym_return, status_return);
  }
  //fprintf(stderr, "shimmed Xutf8LookupString! %d bytes, got %d\n", text_string_used, added); fflush(stderr);

  int new_used = text_string_used + added;
  if (new_used >= MAX_BYTES_IN) {
    // TODO: Handle overflow properly! (dropping for now) --GM
    fprintf(stderr, "FIXME: Xutf8LookupString overflowed! %d -> %d\n", text_string_used, new_used);
  } else {
    text_string_used = new_used;
  }

  int shimmed_result = 0;
  if (text_string_used >= 1) {
    int bytes_to_grab = _buf_char_len(0);

    //fprintf(stderr, "Xutf8LookupString grabbing %d bytes of %d\n", bytes_to_grab, text_string_used); fflush(stderr);

    // SANITY CHECK: Make sure this doesn't actually overflow!
    if (bytes_to_grab >= text_string_used) {
      bytes_to_grab = text_string_used;
    }

    // Copy this across
    assert(bytes_buffer >= bytes_to_grab);
    shimmed_result = bytes_to_grab;
    memmove(buffer_return, text_string_buffer, bytes_to_grab);

    // Remove from the original buffer
    text_string_used -= bytes_to_grab;
    if (text_string_used >= 0) {
      memmove(&text_string_buffer[0], &text_string_buffer[bytes_to_grab], text_string_used);
    }

    // Clear that part of the buffer
    memset(&text_string_buffer[text_string_used], 0, bytes_to_grab);
  }

  return shimmed_result;
}

//
// XOpenIM needs some things done to the environment before it is called.
//
XIM XOpenIM(Display *display, XrmDatabase db, char *res_name, char *res_class) {
  static XIM (*real)(Display *display, XrmDatabase db, char *res_name, char *res_class) = NULL;
  if (real == NULL) { real = dlsym(RTLD_NEXT, "XOpenIM"); }
  if (real == NULL) { abort(); };

  // For the IME to work, we need to set a valid locale and valid locale modifiers.
  if (setlocale(LC_ALL, "") != NULL) {
    if (XSupportsLocale()) {
      (void)XSetLocaleModifiers("");
    }
  }

  XIM result = real(display, db, res_name, res_class);
  fprintf(stderr, "shimmed XOpenIM!\n"); fflush(stderr);
  return result;
}

//
// XCreateIC is another place where we need to ensure certain things are set for an IME to work.
//
// XCreateIC takes a variable number of arguments.
// Passing this along correctly is currently more trouble than it's worth.
//
// So instead, we're going to read what we can from here, and send only what we want...
//
// XNInputStyle:
// Using XIMPreeditNothing | XIMStatusNothing.
// Not to be confused with XIMPreeditNone | XIMStatusNone, which prevents the IME from working.
// UPDATE: I accidentally said that Unity 2019 uses None. It uses Nothing instead, which is what we currently want.
//
// XNClientWindow:
// A compulsory argument. We need to grab this from the program.
//
// XNFocusWindow:
// If the program uses this argument explicitly, we need to grab it. Probably.
//
XIC XCreateIC(XIM im, ...) {
  static XIC (*real)(XIM im, ...) = NULL;
  if (real == NULL) { real = dlsym(RTLD_NEXT, "XCreateIC"); }
  if (real == NULL) { abort(); };

  fprintf(stderr, "shimming XCreateIC and I want to cry\n"); fflush(stderr);

  Window client_window = 0;
  Window focus_window = 0;
  va_list ap;
  va_start(ap, im);
  for (;;) {
    const char *k = va_arg(ap, const char *);
    if (k == NULL) { break; } // End of list

    if (!strcmp(k, XNInputStyle)) {
      int v = va_arg(ap, int);
      fprintf(stderr, "shimmed arg \"%s\": %d\n", k, v); fflush(stderr);
    } else if (!strcmp(k, XNClientWindow)) {
      Window v = va_arg(ap, Window);
      client_window = v;
      fprintf(stderr, "captured arg \"%s\": %lu\n", k, v); fflush(stderr);
    } else if (!strcmp(k, XNFocusWindow)) {
      Window v = va_arg(ap, Window);
      focus_window = v;
      fprintf(stderr, "captured arg \"%s\": %lu\n", k, v); fflush(stderr);
    } else if (!strcmp(k, XNPreeditAttributes)) {
      // TODO: If someone actually runs this in a program which supports preedit, we will probably want to at least not break things. --GM
      XVaNestedList v = va_arg(ap, XVaNestedList);
      fprintf(stderr, "misc arg \"%s\": %p\n", k, v); fflush(stderr);
    } else {
      void *v = va_arg(ap, void *);
      fprintf(stderr, "misc arg \"%s\": %p\n", k, v); fflush(stderr);
    }
  }
  va_end(ap);

  XIC result = real(im,
    XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
    XNClientWindow, client_window,
    XNFocusWindow, focus_window,
    NULL);
  fprintf(stderr, "shimmed XCreateIC!\n"); fflush(stderr);
  return result;
}

//
// We may need to force our fake events through the system.
//
Bool XFilterEvent(XEvent *event, Window w) {
  int (*real)(XEvent *event, Window w) = NULL;
  if (real == NULL) { real = dlsym(RTLD_NEXT, "XFilterEvent"); }
  if (real == NULL) { abort(); };

  // Do not filter the fake events.
  if (text_string_used > 0 && event->type == KeyPress && event->xkey.keycode == None) {
    return False;
  }

  return real(event, w);
}

int XPending(Display *display) {
  int (*real)(Display *display) = NULL;
  if (real == NULL) { real = dlsym(RTLD_NEXT, "XPending"); }
  if (real == NULL) { abort(); };

  // Announce our fake events.
  if (text_string_used > 0) {
    return True;
  }

  return real(display);
}

int XEventsQueued(Display *display, int mode) {
  int (*real)(Display *display, int mode) = NULL;
  if (real == NULL) { real = dlsym(RTLD_NEXT, "XEventsQueued"); }
  if (real == NULL) { abort(); };

  // Announce our fake events.
  int result = real(display, mode);
  if (text_string_used > 0) {
    return result + 1;
  }
  return result;
}

//
// Any event which comes out of XNextEvent() *MUST* be fed through XFilterEvent()!
// So, that's what we do...
// ... unless we need to tell the program that there's still more text to accept.
//
int XNextEvent(Display *display, XEvent *event_return) {
  static int last_result = 0; // FIXME: The return value of this doesn't seem to be defined...? Grab it from a valid call to XNextEvent anyway. --GM

  static int (*real)(Display *display, XEvent *event_return) = NULL;
  if (real == NULL) { real = dlsym(RTLD_NEXT, "XNextEvent"); }
  if (real == NULL) { abort(); };

  // If we have more characters to pass through,
  // synthesise some KeyPress events in order to pass them through.
  if (text_string_used > 0) {
    *event_return = last_key_event;
    event_return->xkey.type = KeyPress;
    event_return->xkey.keycode = None;
    return last_result;
  }

  int result = real(display, event_return);
  if (event_return->type == KeyPress) {
    last_key_event = *event_return;
  }

  return result;
}

//
// UnityPlayer.so grabs its X11 symbols via dlsym().
// This causes it to bypass the functions provided in this library.
//
// If you do a 1-byte hex edit so that the only instance of "dlsym" in the file becomes "Dlsym",
// then it will end up using the functions we want it to.
//
void *Dlsym(void *restrict handle, const char *restrict symbol)
{
  //fprintf(stderr, "Dlsym -> dlsym shim: %p \"%s\"\n", handle, symbol); fflush(stderr);

  if (!strcmp(symbol, "XCreateIC")) { return XCreateIC; }
  if (!strcmp(symbol, "XEventsQueued")) { return XEventsQueued; }
  if (!strcmp(symbol, "XFilterEvent")) { return XFilterEvent; }
  if (!strcmp(symbol, "XNextEvent")) { return XNextEvent; }
  if (!strcmp(symbol, "XOpenIM")) { return XOpenIM; }
  if (!strcmp(symbol, "XPending")) { return XPending; }
  if (!strcmp(symbol, "Xutf8LookupString")) { return Xutf8LookupString; }

  return dlsym(handle, symbol);
}
