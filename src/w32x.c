/*
 * Copyright (c) 2015-2016 Devin Smith <devin@devinsmith.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>

#include <windows.h>
#include "w32x_priv.h"


Colormap colormap;
int blackpixel;
int whitepixel;
Display *disp;
XContext ctxt;

struct msgq_entry {
  MSG msg;
  TAILQ_ENTRY(msgq_entry) entries;
};

struct paint_entry {
  Wnd *wnd;

  TAILQ_ENTRY(paint_entry) entries;
};

TAILQ_HEAD(msg_queue, msgq_entry) g_msg_queue;
TAILQ_HEAD(paint_queue, paint_entry) g_paint_queue;

struct WndClass {
  struct WndClass *next;
  char *name;
  unsigned long border_pixel;
  unsigned long background_pixel;
  int (*proc)(HWND wnd, unsigned int msg, WPARAM wParam, LPARAM lParam);
  size_t wndExtra;
};

static WndClass *class_list = NULL;

extern WNDCLASS ButtonClass;

/* Special atom for deleted messages */
Atom WM_DELETE_WINDOW;

int w32x_init(const char *display_name)
{
  disp = XOpenDisplay(display_name);
  if (disp == NULL)
    return -1;

	colormap = DefaultColormap(disp, DefaultScreen(disp));

	/* Initialize color */
	blackpixel = BlackPixel(disp, DefaultScreen(disp));
	whitepixel = WhitePixel(disp, DefaultScreen(disp));

  /* Capture the WM_DELETE_WINDOW atom, if it exists. We'll use this later
   * to tell the window manager that we're interested in handling the
   * close/delete events for top level windows */
  WM_DELETE_WINDOW = XInternAtom(disp, "WM_DELETE_WINDOW", 0);

  ctxt = XUniqueContext();

  TAILQ_INIT(&g_msg_queue);
  TAILQ_INIT(&g_paint_queue);

  /* Register built in classes. */
  RegisterClass(&ButtonClass);

  return 0;
}

static WndClass *
get_class_by_name(const char *name)
{
  WndClass *wc;

  /* Iterate through list of window classes to see if this
   * class has already been registered. */
  for (wc = class_list; wc != NULL; wc = wc->next) {
    if (strcmp(wc->name, name) == 0) {
      return wc;
    }
  }

  return NULL;
}

int
RegisterClass(WNDCLASS *wndClass)
{
  WndClass *wc;
  XColor back_col;

  if (wndClass == NULL)
    return -1;

  wc = get_class_by_name(wndClass->Name);
  if (wc != NULL) {
    /* Already registered */
    return 1;
  }

  /* Lookup colors */
  XParseColor(disp, colormap, wndClass->BackgroundColor, &back_col);
  XAllocColor(disp, colormap, &back_col);

  /* Register a new class */
  wc = calloc(1, sizeof(WndClass));
  wc->name = strdup(wndClass->Name);
  wc->border_pixel = blackpixel;
  wc->background_pixel = back_col.pixel;
  wc->wndExtra = wndClass->wndExtra;
  wc->proc = wndClass->EventProc;
  wc->next = class_list;
  class_list = wc;

  return 0;
}

HWND
CreateWindow(const char *lpClassName, const char *lpWindowName,
    DWORD dwStyle, HWND parent, int x, int y, int width,
    int height)
{
  XClassHint class_hint;
  Window parent_win;
  Wnd *wnd;
  WndClass *wc = get_class_by_name(lpClassName);

  if (wc == NULL) {
    /* Can't create window, no class */
    return NULL;
  }

  wnd = calloc(1, sizeof(Wnd) + wc->wndExtra);

  if (parent != NULL) {
    parent_win = parent->window;
  } else {
    parent_win = DefaultRootWindow(disp);
  }

  /* Create our GC */
  wnd->hdc = w32x_CreateDC();

  /* parent window */
  wnd->window = XCreateSimpleWindow(disp, parent_win,
    x, y, width, height,
    dwStyle & WS_BORDER ? 1 : 0,
    wc->border_pixel, wc->background_pixel);
  wnd->label = strdup(lpWindowName);
  wnd->proc = wc->proc;
  wnd->parent = parent;
  class_hint.res_name = wnd->label;
  class_hint.res_class = wc->name;
  XSetClassHint(disp, wnd->window, &class_hint);

  XSaveContext(disp, wnd->window, ctxt, (XPointer)wnd);

  XSelectInput(disp, wnd->window,
    ExposureMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask |
    StructureNotifyMask);

  /* Parent will explicitly call ShowWindow when ready */
  if (parent != NULL)
    ShowWindow(wnd);

  /* For top level windows we want to do some extra special case
   * processing */
  if (parent == NULL) {
    SetWindowName(wnd, lpWindowName);

    /* Use the WM_DELETE_WINDOW atom to tell the window manager that we want
     * to handle when this window is closed/destroyed */
    XSetWMProtocols(disp, wnd->window, &WM_DELETE_WINDOW, 1);
  }

  return wnd;
}

void
ShowWindow(HWND wnd)
{
  XMapWindow(disp, wnd->window);
}

void
DestroyWindow(HWND wnd)
{
  SendMessage(wnd, WM_DESTROY, 0, 0);

  XDestroyWindow(disp, wnd->window);
}

void
SetWindowName(HWND wnd, const char *name)
{
  XTextProperty wname;

  if (XStringListToTextProperty((char **)&name, 1, &wname) == 0) {
    fprintf(stderr, "Can't allocate window name\n");
    return;
  }
  XSetWMIconName(disp, wnd->window, &wname);
  XSetWMName(disp, wnd->window, &wname);
  XFree(wname.value);
}

int
GetWindowText(HWND wnd, char *lpString, int nMaxCount)
{
  snprintf(lpString, nMaxCount, "%s", wnd->label);
  return strlen(lpString);
}

int
SendMessage(HWND wnd, unsigned int msg, WPARAM wParam, LPARAM lParam)
{
  if (wnd == NULL || wnd->proc == NULL)
    return 0;

  return wnd->proc(wnd, msg, wParam, lParam);
}

void *GetWindowLongPtr(HWND wnd, int nIndex)
{
  return wnd->wndExtra;
}

BOOL GetWindowRect(HWND wnd, LPRECT rect)
{
	Window root_return;
	int x_return, y_return;
	unsigned int width_return, height_return;
	unsigned int border_width_return;
	unsigned int depth_return;

	XGetGeometry(disp, wnd->window, &root_return, &x_return, &y_return,
	    &width_return, &height_return, &border_width_return, &depth_return);

  rect->left = x_return;
  rect->top = y_return;
  rect->right = width_return;
  rect->bottom = height_return;

  return TRUE;
}

void PostQuitMessage(int nExitCode)
{
  struct msgq_entry *q_msg;

  q_msg = malloc(sizeof(struct msgq_entry));
  q_msg->msg.message = WM_QUIT;

  TAILQ_INSERT_TAIL(&g_msg_queue, q_msg, entries);
}

static void translate_xevent_to_msg(XEvent *e, LPMSG msg)
{
  Wnd *win = NULL;

  XFindContext(e->xany.display, e->xany.window, ctxt, (XPointer *)&win);

  msg->hwnd = win;

  /* Translate XEvent structure to WM messages */
  switch (e->type) {
  case ReparentNotify:
  case MapNotify:
  case VisibilityNotify:
    // Nothing to do for this event.
    break;
  case ConfigureNotify:
    // What do we do for this?
    printf("ConfigureNotify\n");
    break;
  case ButtonPress:
    if (e->xbutton.button == 1) {
      msg->message = WM_LBUTTONDOWN;
      msg->wParam = 0;
      msg->lParam = 0;
    }
    break;
  case ButtonRelease:
    if (e->xbutton.button == 1) {
      msg->message = WM_LBUTTONUP;
      msg->wParam = 0;
      msg->lParam = 0;
    }
    break;
  case Expose:
    if (e->xexpose.count == 0) {
      /* Don't bubble this up, instead put it in a queue */
      msg->message = WM_PAINT;
      msg->wParam = 0;
      msg->lParam = 0;
    }
    break;
  case ClientMessage:
    if (e->xclient.format == 32 && e->xclient.data.l[0] == WM_DELETE_WINDOW) {
      msg->message = WM_CLOSE;
      msg->wParam = 0;
      msg->lParam = 0;
    } else {
      printf("Unhandled client message!\n");
    }
    break;
  default:
    printf("Unhandled event %d\n", e->type);
    break;
  }
}

BOOL GetMessage(LPMSG msg, HWND wnd)
{
  struct msgq_entry *q_msg;
  struct paint_entry *q_paint;
  XEvent event;

  msg->message = 0;

  if (!XPending(disp)) {
    /* Anything in the queue? */
    q_msg = TAILQ_FIRST(&g_msg_queue);
    if (q_msg != NULL) {
      if (q_msg->msg.message == WM_QUIT) {
        free(q_msg);
        return FALSE;
      }

      TAILQ_REMOVE(&g_msg_queue, q_msg, entries);
      msg->hwnd = q_msg->msg.hwnd;
      msg->message = q_msg->msg.message;
      msg->lParam = q_msg->msg.lParam;
      msg->wParam = q_msg->msg.wParam;
      free(q_msg);
      return TRUE;
    }

    /* Anything in the paint queue? */
    TAILQ_FOREACH(q_paint, &g_paint_queue, entries) {

    }
  }

  /* We loop here until we get a message we can handle */
  while (msg->message == 0) {
    /* Process any pending events directly */
    XNextEvent(disp, &event);
    /* Process events */
    translate_xevent_to_msg(&event, msg);
  }

  return TRUE;
}

int DispatchMessage(const MSG *msg)
{
  Wnd *wnd = msg->hwnd;

  wnd->proc(wnd, msg->message, 0, 0);

  return 0;
}