/*

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

*/

#include <argp.h>
#include <errno.h>
#include <ft2build.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>
#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_renderutil.h>
#include FT_FREETYPE_H
#include "helper.h"

/**
 * Part of a keysym lookup, used to translate between keysyms and characters.
 *
 * character: the ASCII character
 * keysym: the Xorg keysym to interpret the character as
 */
typedef struct keysyms_lookup_t
{
  char character;
  xcb_keysym_t keysym;
} keysyms_lookup_t;

/**
 * A recursive structure holding data about windows, used to track the windows
 * we care about.  Exactly one of `window` (paired with `overlay_*`) and
 * `children` is non-NULL.
 *
 * ?overlay_window: the window we created over the top of the tracked window
 * ?overlay_font_gc: for drawing the text on `overlay_window`
 * ?overlay_bg_gc: for drawing the background on `overlay_window`
 * ?overlay_rect: the on-screen area covered by `overlay_window`
 * ?window: the pre-existing tracked window
 * character: the character that must be typed to select `window`, or to
 * descend into `children` ?children: the continuation of the structure
 * children_size: size of `children` (0 if `children` is NULL)
 */
typedef struct window_setup_t
{
  xcb_window_t* overlay_window;
  xcb_gcontext_t* overlay_font_gc;
  xcb_gcontext_t* overlay_bg_gc;
  xcb_rectangle_t* overlay_rect;
  xcb_window_t* window;
  char character;
  struct window_setup_t* children;
  int children_size;
} window_setup_t;

/**
 * Data generated from initial user input to the program.
 *
 * ksl: keys available for use
 * blacklist: windows which should be ignored
 * whitelist: windows which should be included
 * format: FORMAT_DEC or FORMAT_HEX
 */
typedef struct xcw_input_t
{
  keysyms_lookup_t* ksl;
  int ksl_size;
  xcb_window_t* blacklist;
  int blacklist_size;
  xcb_window_t* whitelist;
  int whitelist_size;
  short format;
  int font_size;
  char* font_path;
} xcw_input_t;

/**
 * Collection of data needed throughout the runtime of the program.
 *
 * xcon: the connection to the X server
 * xroot: the root window
 * ewmh: the state for `xcb_ewmh`
 * ksymbols: cached key symbols
 * overlay_font: font used to render text on overlays
 * input: data generated from initial user input to the program
 * wsetups: array of setup structures
 */
typedef struct xcw_state_t
{
  xcb_connection_t* xcon;
  xcb_window_t xroot;
  xcb_ewmh_connection_t ewmh;
  xcb_key_symbols_t* ksymbols;
  xcb_font_t overlay_font;
  xcw_input_t* input;
  window_setup_t* wsetups;
  int wsetups_size;
} xcw_state_t;

// -- constants

/**
 * Text colour for overlay windows.
 */
int FG_COLOUR = 0xffffffff;
/**
 * Name of the font used to render text on overlay windows.
 */
char* OVERLAY_FONT_NAME = "fixed";
/**
 * Background colour for overlay windows.
 */
int BG_COLOUR = 0xff333333;
/**
 * Window class set on overlay windows.
 */
char* OVERLAY_WINDOW_CLASS = "overlay\0xorg-choose-window";
/**
 * Number of windows requested from _NET_CLIENT_LIST.
 */
int MAX_WINDOWS = 1024;
/**
 * Printed version string (used internally by `argp`).
 */
const char* argp_program_version = "xorg-choose-window 0.2.0-next";
/*
 * Output format options.
 */
short FORMAT_DEC = 0;
short FORMAT_HEX = 1;

/**
 * Keysyms with an obvious 1-character representation.  Only these characters
 * may be used as input.
 */
keysyms_lookup_t ALL_KEYSYMS_LOOKUP[] = {
  { '0', 0x0030 }, { '1', 0x0031 }, { '2', 0x0032 }, { '3', 0x0033 },
  { '4', 0x0034 }, { '5', 0x0035 }, { '6', 0x0036 }, { '7', 0x0037 },
  { '8', 0x0038 }, { '9', 0x0039 }, { 'a', 0x0061 }, { 'b', 0x0062 },
  { 'c', 0x0063 }, { 'd', 0x0064 }, { 'e', 0x0065 }, { 'f', 0x0066 },
  { 'g', 0x0067 }, { 'h', 0x0068 }, { 'i', 0x0069 }, { 'j', 0x006a },
  { 'k', 0x006b }, { 'l', 0x006c }, { 'm', 0x006d }, { 'n', 0x006e },
  { 'o', 0x006f }, { 'p', 0x0070 }, { 'q', 0x0071 }, { 'r', 0x0072 },
  { 's', 0x0073 }, { 't', 0x0074 }, { 'u', 0x0075 }, { 'v', 0x0076 },
  { 'w', 0x0077 }, { 'x', 0x0078 }, { 'y', 0x0079 }, { 'z', 0x007a }
};
/**
 * Size of `ALL_KEYSYMS_LOOKUP`.
 */
int ALL_KEYSYMS_LOOKUP_SIZE =
  (sizeof(ALL_KEYSYMS_LOOKUP) / sizeof(*ALL_KEYSYMS_LOOKUP));

// -- utilities

/**
 * Compute the smaller of two numbers.
 */
int
min(int a, int b)
{
  return a < b ? a : b;
}

/**
 * Compute the larger of two numbers.
 */
int
max(int a, int b)
{
  return a < b ? b : a;
}

/**
 * Print an error message to stderr and exit the process with the given status.
 *
 * code: exit code
 * fmt, ...: arguments as taken by `*printf`
 */
void
xcw_fail(int code, char* fmt, ...)
{
  va_list args;
  fprintf(stderr, "error: ");
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  exit(code);
}

/**
 * Print an error message to stderr and exit the process with a failure status.
 *
 * fmt, ...: arguments as taken by `*printf`
 */
void
xcw_die(char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  xcw_fail(EX_SOFTWARE, fmt, args);
  va_end(args);
}

/**
 * Print a warning message to stderr.  Takes arguments like `*printf`.
 */
void
xcw_warn(char* fmt, ...)
{
  va_list args;
  fprintf(stderr, "warning: ");
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

/**
 * Exit the process with a status indicating a window was chosen.
 */
void
xcw_exit_match()
{
  exit(0);
}

/**
 * Exit the process with a status indicating no window was chosen.
 */
void
xcw_exit_no_match()
{
  exit(0);
}

/**
 * Print the chosen window to stdout and exit the process.
 */
void
choose_window(xcw_input_t* input, xcb_window_t window)
{
  if (input->format == FORMAT_DEC)
    printf("%d\n", window);
  else if (input->format == FORMAT_HEX)
    printf("0x%x\n", window);
  xcw_exit_match();
}

// -- xorg utilities

/**
 * Perform the check for a checked, reply-less xcb request.
 *
 * cookie: corresponding to the request
 * msg: to print in case of error
 */
void
xorg_check_request(xcb_connection_t* xcon, xcb_void_cookie_t cookie, char* msg)
{
  xcb_generic_error_t* error = xcb_request_check(xcon, cookie);
  if (error)
    xcw_die("%s (%d)\n", msg, error->error_code);
}

/**
 * Move and resize a window.
 *
 * x, y: new absolute screen location
 */
void
xorg_window_move_resize(
  xcb_connection_t* xcon,
  xcb_window_t window,
  int x,
  int y,
  int w,
  int h)
{
  int mask =
    (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
     XCB_CONFIG_WINDOW_HEIGHT);
  uint32_t values[] = { x, y, w, h };
  xcb_configure_window(xcon, window, mask, values);
}

/**
 * Determine whether a window has a property defined.
 *
 * prop: property ID
 */
int
xorg_window_has_property(
  xcb_connection_t* xcon,
  xcb_window_t window,
  xcb_atom_t prop)
{
  xcb_list_properties_cookie_t lpc = xcb_list_properties(xcon, window);
  xcb_list_properties_reply_t* lpr;
  if (!(lpr = xcb_list_properties_reply(xcon, lpc, NULL))) {
    xcw_die("list_properties");
  }

  xcb_atom_t* props = xcb_list_properties_atoms(lpr);
  int result = 0;
  for (int i = 0; i < xcb_list_properties_atoms_length(lpr); i++) {
    if (props[i] == prop) {
      result = 1;
      break;
    }
  }

  free(lpr);
  return result;
}

/**
 * Construct a 2-byte character string from a 1-byte character string.
 */
xcb_char2b_t*
xorg_str_to_2b(char* text, int text_size)
{
  xcb_char2b_t* text2b = malloc(text_size * sizeof(xcb_char2b_t));
  xcb_char2b_t c = { 0, 0 };
  for (int i = 0; i < text_size; i++) {
    c.byte2 = text[i];
    text2b[i] = c;
  }
  return text2b;
}

static void
load_glyph(
  xcb_connection_t* c,
  xcb_render_glyphset_t gs,
  FT_Face face,
  int charcode)
{
  uint32_t gid;
  xcb_render_glyphinfo_t ginfo;

  FT_Select_Charmap(face, ft_encoding_unicode);
  int glyph_index = FT_Get_Char_Index(face, charcode);
  if (glyph_index == 0) {
    // TODO use fallback font
    // http://www.unicode.org/policies/lastresortfont_eula.html
    printf("character %d not found\n", charcode);
  }
  FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT);

  FT_Bitmap* bitmap = &face->glyph->bitmap;
  ginfo.x = -face->glyph->bitmap_left;
  ginfo.y = face->glyph->bitmap_top;
  ginfo.width = bitmap->width;
  ginfo.height = bitmap->rows;
  ginfo.x_off = face->glyph->advance.x / 64;
  ginfo.y_off = face->glyph->advance.y / 64;

  // glyph_id = charcode;

  gid = charcode;

  int stride = (ginfo.width + 3) & ~3;
  uint8_t tmpbitmap[stride * ginfo.height];
  int y;
  for (y = 0; y < ginfo.height; y++)
    memcpy(
      tmpbitmap + y * stride, bitmap->buffer + y * ginfo.width, ginfo.width);

  xcb_render_add_glyphs_checked(
    c, gs, 1, &gid, &ginfo, stride * ginfo.height, tmpbitmap);

  xcb_flush(c);
}

static xcb_render_picture_t
create_pen(xcb_connection_t* c, int red, int green, int blue, int alpha)
{
  xcb_render_color_t color = {
    .red = red, .green = green, .blue = blue, .alpha = alpha
  };

  xcb_render_pictforminfo_t* fmt;
  const xcb_render_query_pict_formats_reply_t* fmt_rep =
    xcb_render_util_query_formats(c);
  // alpha can only be used with a picture containing a pixmap
  fmt =
    xcb_render_util_find_standard_format(fmt_rep, XCB_PICT_STANDARD_ARGB_32);

  xcb_drawable_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;

  xcb_pixmap_t pm = xcb_generate_id(c);
  xcb_create_pixmap(c, 32, pm, root, 1, 1);

  uint32_t values[1];
  values[0] = XCB_RENDER_REPEAT_NORMAL;

  xcb_render_picture_t picture = xcb_generate_id(c);
  xcb_render_create_picture(
    c, picture, pm, fmt->id, XCB_RENDER_CP_REPEAT, values);

  xcb_rectangle_t rect = { .x = 0, .y = 0, .width = 1, .height = 1 };

  xcb_render_fill_rectangles(
    c, XCB_RENDER_PICT_OP_OVER, picture, color, 1, &rect);

  xcb_free_pixmap(c, pm);
  return picture;
}

static xcb_render_glyphset_t
load_glyphset(
  xcb_connection_t* c,
  char* filename,
  int size,
  struct utf_holder holder)
{
  static int ft_lib_initialized = 0;
  static FT_Library library;
  int n;

  xcb_render_pictforminfo_t* fmt_a8;
  const xcb_render_query_pict_formats_reply_t* fmt_rep =
    xcb_render_util_query_formats(c);
  fmt_a8 = xcb_render_util_find_standard_format(fmt_rep, XCB_PICT_STANDARD_A_8);

  xcb_render_glyphset_t gs = xcb_generate_id(c);
  xcb_render_create_glyph_set(c, gs, fmt_a8->id);

  if (!ft_lib_initialized)
    FT_Init_FreeType(&library);

  // TODO: leave that aside from now and do a simple loading
  FT_Face face;

  FT_New_Face(library, filename, 0, &face);

  FT_Set_Char_Size(face, 0, size * 64, 90, 90);

  for (n = 0; n < holder.length; n++)
    load_glyph(c, gs, face, holder.str[n]);

  FT_Done_Face(face);

  return gs;
}

/**
 * Render text centred on a window.
 *
 * win_rect: rectangle covering window with ID `win`
 * gc: graphics context for rendering the text
 * text: text to render
 */
void
xorg_draw_text_centred(
  xcb_connection_t* xcon,
  xcb_window_t win,
  xcb_rectangle_t* win_rect,
  xcb_gcontext_t gc,
  char* text,
  int font_size,
  char* font_path)
{
  int size = min(strlen(text), 255);

  // check expected rendered size
  xcb_char2b_t* text_2b = xorg_str_to_2b(text, size);
  xcb_query_text_extents_cookie_t qtec =
    (xcb_query_text_extents(xcon, gc, size, text_2b));
  free(text_2b);
  xcb_query_text_extents_reply_t* qter;
  if (!(qter = xcb_query_text_extents_reply(xcon, qtec, NULL))) {
    xcw_die("query_text_extents\n");
  }

  int x = (win_rect->width - qter->overall_width) / 2;
  int y = (win_rect->height - qter->font_ascent - qter->font_descent) / 2;

  xcb_screen_t* screen;
  screen = xcb_setup_roots_iterator(xcb_get_setup(xcon)).data;
  xcb_pixmap_t pmap = xcb_generate_id(xcon);

  xcb_create_pixmap(
    xcon,
    screen->root_depth,
    pmap,
    screen->root, // doesn't matter
    win_rect->width,
    win_rect->height);

  xcb_render_pictforminfo_t* fmt;
  const xcb_render_query_pict_formats_reply_t* fmt_rep =
    xcb_render_util_query_formats(xcon);

  fmt = xcb_render_util_find_standard_format(fmt_rep, XCB_PICT_STANDARD_RGB_24);

  uint32_t values[2];
  xcb_void_cookie_t cookie;
  xcb_render_picture_t picture = xcb_generate_id(xcon);
  values[0] = XCB_RENDER_POLY_MODE_IMPRECISE;
  values[1] = XCB_RENDER_POLY_EDGE_SMOOTH;
  cookie = xcb_render_create_picture_checked(
    xcon,
    picture, // pid
    pmap,    // drawable
    fmt->id, // format
    XCB_RENDER_CP_POLY_MODE | XCB_RENDER_CP_POLY_EDGE,
    values); // make it smooth

  /*xcb_render_picture_t fg_pen = create_pen(xcon, 0xff0f, 0, 0xff00, 0xf000);*/
  xcb_render_picture_t fg_pen =
    create_pen(xcon, 0x0f00, 0xff00, 0x0f00, 0xf000);

  char* temp = malloc(strlen(text));
  strcpy(temp, text);
  struct utf_holder holder;
  holder = char_to_uint32(temp);
  /*int font_size = max(x, y) / 100 * 30;*/
  xcb_render_glyphset_t font =
    load_glyphset(xcon, font_path, font_size, holder);
  xcb_render_util_composite_text_stream_t* ts =
    xcb_render_util_composite_text_stream(font, holder.length, 0);

  // draw the text (in holder) at certain positions
  xcb_render_util_glyphs_32(
    ts,
    x,             // dx
    y,             // dy
    holder.length, //
    holder.str);

  xcb_render_util_composite_text(
    xcon,                    // connection
    XCB_RENDER_PICT_OP_OVER, // op
    fg_pen,                  // src
    picture,                 // dst
    0,                       // fmt
    0,                       // src x
    0,                       // src y
    ts);                     // txt stream

  xcb_copy_area(
    xcon,            /* xcb_connection_t */
    pmap,            /* The Drawable we want to paste */
    win,             /* The Drawable on which we copy the previous Drawable */
    gc,              /* A Graphic Context */
    0,               /* Top left x coordinate of the region to copy */
    0,               /* Top left y coordinate of the region to copy */
    0,               /* Top left x coordinate of the region where to copy */
    0,               /* Top left y coordinate of the region where to copy */
    win_rect->width, /* Width of the region to copy */
    win_rect->height /* Height of the region to copy */
  );

  /*xcb_image_text_8(xcon, size, win, gc, x, y + qter->font_ascent, text);*/
}

/**
 * Determine whether a window is 'normal' and visible according to the base
 * Xorg specification.
 */
int
xorg_window_normal(xcb_connection_t* xcon, xcb_window_t window)
{
  xcb_get_window_attributes_cookie_t gwac =
    (xcb_get_window_attributes(xcon, window));
  xcb_get_window_attributes_reply_t* gwar;
  if (!(gwar = xcb_get_window_attributes_reply(xcon, gwac, NULL))) {
    xcw_die("get_window_attributes\n");
  }

  return (
    gwar->map_state == XCB_MAP_STATE_VIEWABLE && gwar->override_redirect == 0);
}

/**
 * Determine whether a window is a persistent application window according
 * EWMH.
 */
int
ewmh_window_normal(xcw_state_t* state, xcb_window_t window)
{
  xcb_get_property_cookie_t gpc = (xcb_get_property(
    state->xcon,
    0,
    window,
    state->ewmh._NET_WM_WINDOW_TYPE,
    XCB_ATOM_ATOM,
    0,
    1));
  xcb_get_property_reply_t* gpr;
  if (!(gpr = xcb_get_property_reply(state->xcon, gpc, NULL))) {
    xcw_die("get_property _NET_WM_WINDOW_TYPE\n");
  }
  uint32_t* window_type = (uint32_t*)xcb_get_property_value(gpr);
  int prop_len = xcb_get_property_value_length(gpr);
  free(gpr);

  // if reply length is 0, window type isn't defined, so treat it as normal
  return (
    prop_len == 0 ||
    window_type[0] == state->ewmh._NET_WM_WINDOW_TYPE_TOOLBAR ||
    window_type[0] == state->ewmh._NET_WM_WINDOW_TYPE_MENU ||
    window_type[0] == state->ewmh._NET_WM_WINDOW_TYPE_UTILITY ||
    window_type[0] == state->ewmh._NET_WM_WINDOW_TYPE_SPLASH ||
    window_type[0] == state->ewmh._NET_WM_WINDOW_TYPE_DIALOG ||
    window_type[0] == state->ewmh._NET_WM_WINDOW_TYPE_NORMAL);
}

/**
 * Get all windows from the X server.
 *
 * windows (output): window IDs
 * windows_size (output): size of `windows`
 */
void
xorg_get_windows(xcw_state_t* state, xcb_window_t** windows, int* windows_size)
{
  xcb_query_tree_cookie_t qtc = xcb_query_tree(state->xcon, state->xroot);
  xcb_query_tree_reply_t* qtr;
  if (!(qtr = xcb_query_tree_reply(state->xcon, qtc, NULL))) {
    xcw_die("query_tree\n");
  }
  xcb_window_t* referenced_windows = xcb_query_tree_children(qtr);
  int size = xcb_query_tree_children_length(qtr);

  // copy for easier usage
  *windows = calloc(size, sizeof(xcb_window_t));
  for (int i = 0; i < size; i++)
    (*windows)[i] = referenced_windows[i];
  free(qtr);

  *windows_size = size;
}

/**
 * Get windows managed by the window manager.
 *
 * is_defined (output): whether the window manager defines the windows it
 *     tracks; if 0, `windows` and `windows_size` will not be set
 * windows (output): window IDs
 * windows_size (output): size of `windows`
 */
void
xorg_get_managed_windows(
  xcw_state_t* state,
  int* is_defined,
  xcb_window_t** windows,
  int* windows_size)
{
  *is_defined = xorg_window_has_property(
    state->xcon, state->xroot, state->ewmh._NET_CLIENT_LIST);

  if (*is_defined) {
    xcb_get_property_cookie_t gpc = (xcb_get_property(
      state->xcon,
      0,
      state->xroot,
      state->ewmh._NET_CLIENT_LIST,
      XCB_ATOM_WINDOW,
      0,
      MAX_WINDOWS));
    xcb_get_property_reply_t* gpr;
    if (!(gpr = xcb_get_property_reply(state->xcon, gpc, NULL))) {
      xcw_die("get_property _NET_CLIENT_LIST\n");
    }
    xcb_window_t* referenced_windows =
      ((xcb_window_t*)xcb_get_property_value(gpr));
    // each window ID is 4 bytes
    int size = xcb_get_property_value_length(gpr) / 4;

    // copy for easier usage
    *windows = calloc(size, sizeof(xcb_window_t));
    for (int i = 0; i < size; i++)
      (*windows)[i] = referenced_windows[i];
    free(gpr);

    *windows_size = size;
  }
}

/**
 * Determine whether a window is managed by the window manager.
 *
 * managed_windows: windows that are managed by the window manager
 */
int
xorg_window_managed(
  xcb_window_t window,
  xcb_window_t* managed_windows,
  int managed_windows_size)
{
  int found = 0;
  for (int i = 0; i < managed_windows_size; i++) {
    if (managed_windows[i] == window) {
      found = 1;
      break;
    }
  }
  return found;
}

/**
 * Determine whether a window is in the list of windows.
 */
int
xorg_contains_window(
  xcb_window_t* windows,
  int windows_size,
  xcb_window_t window)
{
  int found = 0;
  for (int i = 0; i < windows_size; i++) {
    if (windows[i] == window) {
      found = 1;
      break;
    }
  }
  return found;
}

/**
 * Initialise the connection to the X server.
 *
 * state (output): pointer to program state; `input` and `wsetups` are not
 *     initialised
 */
void
initialise_xorg(xcw_state_t** state)
{
  int default_screen; // unused
  xcb_screen_t* screen;
  xcb_connection_t* xcon = xcb_connect(NULL, &default_screen);
  if (xcb_connection_has_error(xcon))
    xcw_die("connect\n");

  screen = xcb_setup_roots_iterator(xcb_get_setup(xcon)).data;
  if (screen == NULL)
    xcw_die("no screens\n");
  xcb_window_t xroot = screen->root;

  xcb_ewmh_connection_t ewmh;
  xcb_intern_atom_cookie_t* ewmhc = xcb_ewmh_init_atoms(xcon, &ewmh);
  if (!xcb_ewmh_init_atoms_replies(&ewmh, ewmhc, NULL)) {
    xcw_die("ewmh init\n");
  }

  xcb_key_symbols_t* ksymbols;
  ksymbols = xcb_key_symbols_alloc(xcon);
  if (ksymbols == NULL)
    xcw_die("key_symbols_alloc\n");

  xcb_font_t overlay_font = xcb_generate_id(xcon);
  xcb_void_cookie_t ofc = xcb_open_font_checked(
    xcon, overlay_font, strlen(OVERLAY_FONT_NAME), OVERLAY_FONT_NAME);
  xorg_check_request(xcon, ofc, "open_font");

  *state = malloc(sizeof(xcw_state_t));
  xcw_state_t local_state = { xcon,         xroot, ewmh, ksymbols,
                              overlay_font, NULL,  NULL, 0 };
  **state = local_state;
}

// -- input handling

/**
 * Find an item in a keysym lookup given its character.
 *
 * ksl: keysym lookup to search
 * c: character to search for
 *
 * returns: found item, NULL if no matching items were found
 */
keysyms_lookup_t*
keysyms_lookup_find_char(keysyms_lookup_t* ksl, int ksl_size, char c)
{
  for (int i = 0; i < ksl_size; i++) {
    if (ksl[i].character == c) {
      return &(ksl[i]);
    }
  }
  return NULL;
}

/**
 * Find an item in a keysym lookup given its keysym.
 *
 * ksl: keysym lookup to search
 * ksym: keysym to search for
 *
 * returns: found item, NULL if no matching items were found
 */
keysyms_lookup_t*
keysyms_lookup_find_keysym(
  keysyms_lookup_t* ksl,
  int ksl_size,
  xcb_keysym_t ksym)
{
  for (int i = 0; i < ksl_size; i++) {
    if (ksl[i].keysym == ksym) {
      return &(ksl[i]);
    }
  }
  return NULL;
}

/**
 * Acquire a Xorg keyboard grab on the root window.
 */
void
initialise_input(xcw_state_t* state)
{
  // wait a little for other programs to release the keyboard
  // since this program is likely to be launched from a hotkey daemon
  struct timespec ts = { 0, 1000000 }; // 1ms
  int status;
  for (int s = 0; s < 1000; s++) { // up to 1s total
    xcb_grab_keyboard_cookie_t gkc = xcb_grab_keyboard(
      state->xcon,
      0,
      state->xroot,
      XCB_CURRENT_TIME,
      XCB_GRAB_MODE_ASYNC,
      XCB_GRAB_MODE_ASYNC);
    xcb_grab_keyboard_reply_t* gkr;

    if ((gkr = xcb_grab_keyboard_reply(state->xcon, gkc, NULL))) {
      status = gkr->status;
      free(gkr);
      if (status == XCB_GRAB_STATUS_ALREADY_GRABBED) {
        nanosleep(&ts, NULL);
        continue;
      } else if (status == XCB_GRAB_STATUS_SUCCESS) {
        break;
      } else {
        xcw_die("grab_keyboard: %d\n", status);
      }
    } else {
      xcw_die("grab_keyboard\n");
    }
  }

  if (status == XCB_GRAB_STATUS_ALREADY_GRABBED) {
    xcw_die("grab_keyboard: already grabbed\n");
  }
}

// -- overlay windows

/**
 * Create an overlay window.
 *
 * x, y: absolute screen location
 * w, h: window size
 */
xcb_window_t*
overlay_create(xcw_state_t* state, int x, int y, int w, int h)
{
  xcb_window_t* win = malloc(sizeof(xcb_window_t));
  *win = xcb_generate_id(state->xcon);
  uint32_t mask =
    (XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_SAVE_UNDER |
     XCB_CW_EVENT_MASK);
  uint32_t values[] = {
    BG_COLOUR, 1, 1, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
  };
  xcb_void_cookie_t cwc = xcb_create_window_checked(
    state->xcon,
    XCB_COPY_FROM_PARENT,
    *win,
    state->xroot,
    0,
    0,
    1,
    1,
    0,
    XCB_WINDOW_CLASS_INPUT_OUTPUT,
    XCB_COPY_FROM_PARENT,
    mask,
    values);
  xorg_check_request(state->xcon, cwc, "create_window");

  xcb_icccm_set_wm_class(
    state->xcon, *win, sizeof(OVERLAY_WINDOW_CLASS), OVERLAY_WINDOW_CLASS);
  xorg_window_move_resize(state->xcon, *win, x, y, w, h);
  xcb_void_cookie_t mwc = xcb_map_window_checked(state->xcon, *win);
  xorg_check_request(state->xcon, mwc, "map_window");
  return win;
}

/**
 * Create a graphics context for drawing the background of an overlay window.
 *
 * win: the overlay window
 */
xcb_gcontext_t*
overlay_get_bg_gc(xcb_connection_t* xcon, xcb_window_t win)
{
  xcb_gcontext_t* gc = malloc(sizeof(xcb_gcontext_t));
  *gc = xcb_generate_id(xcon);
  uint32_t mask = XCB_GC_FOREGROUND;
  uint32_t value_list[] = { BG_COLOUR };
  xcb_void_cookie_t cgc =
    (xcb_create_gc_checked(xcon, *gc, win, mask, value_list));
  xorg_check_request(xcon, cgc, "create_gc");
  return gc;
}

/**
 * Create a graphics context for drawing the text of an overlay window.
 *
 * win: the overlay window
 */
xcb_gcontext_t*
overlay_get_font_gc(xcw_state_t* state, xcb_window_t win)
{
  xcb_gcontext_t* gc = malloc(sizeof(xcb_gcontext_t));
  *gc = xcb_generate_id(state->xcon);
  uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
  uint32_t value_list[] = { FG_COLOUR, BG_COLOUR, state->overlay_font };
  xcb_void_cookie_t cgc =
    (xcb_create_gc_checked(state->xcon, *gc, win, mask, value_list));
  xorg_check_request(state->xcon, cgc, "create_gc");
  return gc;
}

/**
 * Set the text on an overlay window.  `xcb_flush` should be called after
 * calling this function.
 *
 * wsetup: containing the overlay window (if there is no overlay window, this
 *     function does nothing)
 * text: text to render (null-terminated, must be at most 255 characters)
 */
void
overlay_set_text(xcw_state_t* state, window_setup_t* wsetup, char* text)
{
  if (wsetup->overlay_window == NULL)
    return;
  xcb_window_t win = *(wsetup->overlay_window);

  if (wsetup->overlay_bg_gc == NULL) {
    wsetup->overlay_bg_gc = overlay_get_bg_gc(state->xcon, win);
  }
  if (wsetup->overlay_font_gc == NULL) {
    wsetup->overlay_font_gc = overlay_get_font_gc(state, win);
  }

  xcb_poly_fill_rectangle(
    state->xcon, win, *(wsetup->overlay_bg_gc), 1, wsetup->overlay_rect);
  xorg_draw_text_centred(
    state->xcon,
    win,
    wsetup->overlay_rect,
    *(wsetup->overlay_font_gc),
    text,
    state->input->font_size,
    state->input->font_path);
}

/**
 * See `overlays_set_text`.  `xcb_flush` should be called after calling * this
 * function.
 *
 * wsetups: array of setup structures containing overlay windows to render text
 *     on (recursively)
 * text: prefix to text rendered to every overlay window (null-terminated)
 */
void
_overlays_set_text(
  xcw_state_t* state,
  window_setup_t* wsetups,
  int wsetups_size,
  char* text)
{
  int text_size = strlen(text);
  // there's no way we're every going to reach 255 characters with the
  // current setup; this is just in case extra static text gets added
  if (text_size + 1 > 255) {
    xcw_warn("refusing to render text longer than 255 characters\n");
  } else
    for (int i = 0; i < wsetups_size; i++) {
      window_setup_t* wsetup = &(wsetups[i]);
      // next level down is 1 character longer, plus 1 for null
      char* new_text = calloc(text_size + 2, sizeof(char));
      strcpy(new_text, text);
      new_text[text_size] = wsetup->character;
      new_text[text_size + 1] = '\0';

      overlay_set_text(state, wsetup, new_text);
      if (wsetup->children != NULL) {
        _overlays_set_text(
          state, wsetup->children, wsetup->children_size, new_text);
      }

      free(new_text);
    }
}

/**
 * Update text on all overlay windows.
 */
void
overlays_set_text(xcw_state_t* state)
{
  char* text = "";
  _overlays_set_text(state, state->wsetups, state->wsetups_size, text);
  xcb_flush(state->xcon);
}

// -- wsetup utilities

/**
 * Create a bottom-level `wsetup_t`.
 *
 * window: the window to track
 * character: bottom-level character in the window label
 */
window_setup_t
initialise_window_setup(xcw_state_t* state, xcb_window_t window, char character)
{
  // an xcb_window_t is an xcb_drawable_t
  xcb_get_geometry_cookie_t ggc = xcb_get_geometry(state->xcon, window);
  xcb_get_geometry_reply_t* ggr;
  if (!(ggr = xcb_get_geometry_reply(state->xcon, ggc, NULL))) {
    xcw_die("get_geometry\n");
  }

  xcb_rectangle_t rect = { 0, 0, ggr->width, ggr->height };
  xcb_rectangle_t* rectp = malloc(sizeof(xcb_rectangle_t));
  *rectp = rect;
  // guaranteed that ksl_size <= windows_size
  xcb_window_t* overlay_window = overlay_create(
    state,
    ggr->border_width + ggr->x,
    ggr->border_width + ggr->y,
    rect.width,
    rect.height);
  xcb_window_t* window_p = malloc(sizeof(xcb_window_t));
  *window_p = window;

  window_setup_t wsetup = { overlay_window, NULL,      NULL, rectp,
                            window_p,       character, NULL, 0 };
  return wsetup;
}

/**
 * See `initialise_window_tracking`.
 *
 * remain_depth: number of nested levels remaining (we choose a character in
 * the string to type at every level; if 0, we choose the last character)
 */
void
_initialise_window_tracking(
  xcw_state_t* state,
  int remain_depth,
  xcb_window_t* windows,
  int windows_size,
  window_setup_t** wsetups,
  int* wsetups_size)
{
  if (remain_depth == 0) {
    *wsetups = calloc(windows_size, sizeof(window_setup_t));
    *wsetups_size = windows_size;
    for (int i = 0; i < windows_size; i++) {
      // guaranteed that ksl_size <= windows_size
      (*wsetups)[i] = initialise_window_setup(
        state, windows[i], state->input->ksl[i].character);
    }
  } else {
    // base number of windows 'used up' per iteration
    int p = windows_size / state->input->ksl_size;
    // number of iterations to use one extra window
    int r = windows_size % state->input->ksl_size;
    // required number of iterations to use all windows
    int n = p > 0 ? state->input->ksl_size : r;
    *wsetups = calloc(n, sizeof(window_setup_t));
    *wsetups_size = n;
    xcb_window_t* remain_windows = windows;

    for (int i = 0; i < n; i++) {
      window_setup_t* children = NULL;
      int children_size;
      int children_windows_size = i < r ? p + 1 : p;

      if (children_windows_size == 1) {
        (*wsetups)[i] = initialise_window_setup(
          state, *remain_windows, state->input->ksl[i].character);
      } else {
        _initialise_window_tracking(
          state,
          remain_depth - 1,
          remain_windows,
          children_windows_size,
          &children,
          &children_size);
        window_setup_t wsetup = { NULL,     NULL,
                                  NULL,     NULL,
                                  NULL,     state->input->ksl[i].character,
                                  children, children_size };
        (*wsetups)[i] = wsetup;
      }

      remain_windows += children_windows_size;
    }
  }
}

/**
 * Construct data for tracked windows in a nested structure matching the
 * characters that need to be typed to choose them.
 *
 * state: the result is stored in here
 * windows: tracked windows
 */
void
initialise_window_tracking(
  xcw_state_t* state,
  xcb_window_t* windows,
  int windows_size)
{
  _initialise_window_tracking(
    state,
    // the length of each tracking string
    (int)(log(max(windows_size - 1, 1)) / log(state->input->ksl_size)),
    windows,
    windows_size,
    &(state->wsetups),
    &(state->wsetups_size));
}

/**
 * See `wsetup_debug_print`.
 *
 * depth: current depth in the structure, starting at 0, used for indentation
 */
void
_wsetup_debug_print(window_setup_t* wsetup, int depth)
{
  printf("[wsetup] ");
  for (int i = 0; i < depth; i++)
    printf("  ");
  printf("%c", wsetup->character);
  if (wsetup->window == NULL)
    printf("\n");
  else
    printf(" %x\n", *(wsetup->window));

  if (wsetup->children != NULL) {
    for (int i = 0; i < wsetup->children_size; i++) {
      _wsetup_debug_print(&(wsetup->children[i]), depth + 1);
    }
  }
}

/**
 * Print a setup structure to stdout.
 */
void
wsetup_debug_print(window_setup_t* wsetup)
{
  _wsetup_debug_print(wsetup, 0);
}

/**
 * Destroy all overlay windows in a setup structure and free the structure's
 * used memory.
 */
void
wsetup_free(xcb_connection_t* xcon, window_setup_t* wsetup)
{
  xcb_window_t* w = wsetup->overlay_window;
  if (w != NULL) {
    xcb_destroy_window_checked(xcon, *w);
    free(wsetup->overlay_rect);
  }
  if (wsetup->overlay_bg_gc != NULL) {
    xcb_free_gc(xcon, *(wsetup->overlay_bg_gc));
    free(wsetup->overlay_bg_gc);
  }
  if (wsetup->overlay_font_gc != NULL) {
    xcb_free_gc(xcon, *(wsetup->overlay_font_gc));
    free(wsetup->overlay_font_gc);
  }

  if (wsetup->children != NULL) {
    for (int i = 0; i < wsetup->children_size; i++) {
      window_setup_t* child = &(wsetup->children[i]);
      wsetup_free(xcon, child);
    }
    free(wsetup->children);
  }

  xcb_flush(xcon);
}

/**
 * Choose the window in a setup structure or replace the current array of setup
 * structures with its children.  Updates text rendered on overlay windows.
 * Exits the process if a window is chosen.
 */
void
wsetup_choose(xcw_state_t* state, window_setup_t* wsetup)
{
  if (wsetup->window != NULL && wsetup->children_size == 0) {
    choose_window(state->input, *(wsetup->window));
  } else {
    state->wsetups = wsetup->children;
    state->wsetups_size = wsetup->children_size;
    overlays_set_text(state);
  }
}

/**
 * Reduce a setup structure by choosing an item.  Frees removed parts of the
 * structure.
 *
 * index: array index in `wsetups` to choose
 */
void
wsetups_descend_by_index(xcw_state_t* state, int index)
{
  // state changes during iteration
  window_setup_t* wsetups = state->wsetups;
  int wsetups_size = state->wsetups_size;
  for (int i = 0; i < wsetups_size; i++) {
    if (i == index)
      wsetup_choose(state, &(wsetups[i]));
    else
      wsetup_free(state->xcon, &(wsetups[i]));
  }
}

/**
 * Reduce a setup structure by choosing a character, then reduce recursively
 * like `wsetups_descend_by_index`.  Exits the process if the character doesn't
 * correspond to any options.  Frees removed parts of the structure.
 *
 * c: character to choose
 */
void
wsetups_descend_by_char(xcw_state_t* state, char c)
{
  int index = -1;
  for (int i = 0; i < state->wsetups_size; i++) {
    if (state->wsetups[i].character == c) {
      index = i;
      break;
    }
  }

  if (index == -1)
    xcw_exit_no_match();
  else
    wsetups_descend_by_index(state, index);
}

// -- program

/**
 * Get the windows to track.
 *
 * windows (output): window IDs
 * windows_size (output): size of `windows`
 */
void
initialise_tracked_windows(
  xcw_state_t* state,
  xcb_window_t** windows,
  int* windows_size)
{
  xcb_window_t* all_windows;
  int all_windows_size;
  xorg_get_windows(state, &all_windows, &all_windows_size);
  int managed_windows_defined;
  xcb_window_t* managed_windows;
  int managed_windows_size;
  xorg_get_managed_windows(
    state, &managed_windows_defined, &managed_windows, &managed_windows_size);

  *windows = calloc(all_windows_size, sizeof(xcb_window_t));
  int size = 0;
  for (int i = 0; i < all_windows_size; i++) {
    if (
      // ignore if not managed by the window manager
      !(managed_windows_defined &&
        !xorg_window_managed(
          all_windows[i], managed_windows, managed_windows_size)) &&

      // only include if whitelisted
      (state->input->whitelist_size == 0 || xorg_contains_window(
                                              state->input->whitelist,
                                              state->input->whitelist_size,
                                              all_windows[i])) &&

      // ignore if blacklisted
      !xorg_contains_window(
        state->input->blacklist,
        state->input->blacklist_size,
        all_windows[i]) &&

      xorg_window_normal(state->xcon, all_windows[i]) &&

      ewmh_window_normal(state, all_windows[i])) {
      (*windows)[size] = all_windows[i];
      size += 1;
    }
  }
  *windows = realloc(*windows, size * sizeof(xcb_window_t));
  *windows_size = size;

  if (managed_windows_defined)
    free(managed_windows);
  free(all_windows);
}

/**
 * Make adjustments to tracking windows based on a keypress event.  Exits the
 * process if this chooses a window.
 */
void
handle_keypress(xcw_state_t* state, xcb_key_press_event_t* kp)
{
  xcb_keysym_t ksym = xcb_key_press_lookup_keysym(state->ksymbols, kp, 0);
  keysyms_lookup_t* ksl_item = (keysyms_lookup_find_keysym(
    state->input->ksl, state->input->ksl_size, ksym));

  if (ksl_item == NULL) {
    xcw_exit_no_match();
  } else {
    wsetups_descend_by_char(state, ksl_item->character);
  }
}

/**
 * Parse the `CHARACTERS` argument.  May call `argp_error`.
 *
 * char_pool: value passed for the argument
 * input: result is placed in here
 */
void
parse_arg_characters(
  char* char_pool,
  struct argp_state* state,
  xcw_input_t* input)
{
  // we won't put more than `char_pool` items in `ksl`
  int pool_size = strlen(char_pool);
  input->ksl = calloc(pool_size, sizeof(keysyms_lookup_t));
  int size = 0;

  // check validity of characters, compile lookup
  for (int i = 0; i < pool_size; i++) {
    char c = char_pool[i];
    keysyms_lookup_t* ksl_item =
      keysyms_lookup_find_char(ALL_KEYSYMS_LOOKUP, ALL_KEYSYMS_LOOKUP_SIZE, c);
    if (ksl_item == NULL) {
      argp_error(state, "CHARACTERS argument: unknown character: %c", c);
    }
    // don't allow duplicates in lookup
    if (keysyms_lookup_find_char(input->ksl, size, c) == NULL) {
      input->ksl[size] = *ksl_item;
      size += 1;
    }
  }

  if (size < 2) {
    argp_error(state, "CHARACTERS argument: expected at least two characters");
  }
  input->ksl = realloc(input->ksl, sizeof(keysyms_lookup_t) * size);
  input->ksl_size = size;
}

/**
 * Parse the `--blacklist` or `--whitelist` option.  May call `argp_error`.
 *
 * window_id: value passed to the option
 * windows: result is added to this list, and its size updated
 */
void
parse_arg_window_list(
  char* window_id,
  struct argp_state* state,
  xcb_window_t** windows,
  int* windows_size)
{
  errno = 0;
  long int window = strtol(window_id, NULL, 0);
  // Xorg window IDs are 32-bit unsigned
  if (errno != 0 || window <= 0 || window > 0xffffffff) {
    argp_error(state, "invalid value for window ID: %s", window_id);
  }

  *windows = realloc(*windows, sizeof(xcb_window_t) * (*windows_size + 1));
  (*windows)[*windows_size] = window;
  *windows_size += 1;
}

/**
 * Parse the `--format` option.  May call `argp_error`.
 *
 * format: value passed to the option
 * input: result is placed in here
 */
void
parse_arg_format(char* format, struct argp_state* state, xcw_input_t* input)
{
  if (strcmp(format, "decimal") == 0) {
    input->format = FORMAT_DEC;
  } else if (strcmp(format, "hexadecimal") == 0) {
    input->format = FORMAT_HEX;
  } else {
    argp_error(state, "invalid value for output format: %s", format);
  }
}

/**
 * Parse the `--font-size` option.  May call `argp_error`.
 *
 * font_size: value passed to the option
 * input: result is placed in here
 */

void
parse_arg_font_size(
  char* font_size,
  struct argp_state* state,
  xcw_input_t* input)
{
  input->font_size = atoi(font_size);
}

/**
 * Parse the `--font-path` option.  May call `argp_error`.
 *
 * font_path: value passed to the option
 * input: result is placed in here
 */

void
parse_arg_font_path(
  char* font_path,
  struct argp_state* state,
  xcw_input_t* input)
{
  input->font_path = font_path;
}

/**
 * Argument parsing function for use with `argp`.
 *
 * state: `input` is `xcw_input_t*`, which points to an allocated instance that
 *     gets populated by calls to this function
 */
error_t
parse_arg(int key, char* value, struct argp_state* state)
{
  xcw_input_t* input = (xcw_input_t*)(state->input);

  if (key == 'b') {
    parse_arg_window_list(
      value, state, &(input->blacklist), &(input->blacklist_size));
    return 0;
  } else if (key == 'w') {
    parse_arg_window_list(
      value, state, &(input->whitelist), &(input->whitelist_size));
    return 0;
  } else if (key == 'f') {
    parse_arg_format(value, state, input);
    return 0;
  } else if (key == 's') {
    parse_arg_font_size(value, state, input);
    return 0;
  } else if (key == 't') {
    parse_arg_font_path(value, state, input);
    return 0;
  } else if (key == ARGP_KEY_ARG) {
    if (state->arg_num == 0) {
      parse_arg_characters(value, state, input);
      return 0;
    } else {
      return ARGP_ERR_UNKNOWN;
    }
  } else {
    return ARGP_ERR_UNKNOWN;
  }
}

/**
 * Parse command-line arguments.
 *
 * argc, argv: as passed to `main`
 */
xcw_input_t*
parse_args(int argc, char** argv)
{
  struct argp_option options[] = {
    { "blacklist",
      'b',
      "WINDOWID",
      0,
      "IDs of windows to ignore (specify this option multiple times)" },
    { "whitelist",
      'w',
      "WINDOWID",
      0,
      "IDs of windows to include (include all if none specified) \
(specify this option multiple times)" },
    { "format", 'f', "FORMAT", 0, "Output format: 'decimal' or 'hexadecimal'" },
    { "font-size",
      's',
      "FONT-SIZE",
      0,
      "size of text font that will be displayed in your window" },
    { "font-path",
      't',
      "FONT-PATH",
      0,
      "which font you want to use, please give the font(ttf) absolute path" },
    { 0 }
  };

  struct argp parser = { options, parse_arg, "CHARACTERS", "\n\
Running the program draws a string of characters over each visible window.  \
Typing one of those strings causes the program to print the corresponding \
window ID to standard output and exit.  If any non-matching keys are pressed, \
the program exits without printing anything.\n\
\n\
CHARACTERS defines the characters available for use in the displayed strings; \
eg. 'asdfjkl' is a good choice for a QWERTY keyboard layout.  Allowed \
characters are the numbers 0-9 and the letters a-z.\n\
\n\
The program exits with status 0 on success, 64 on invalid arguments, and 70 if \
an unexpected error occurs.",
                         NULL,    NULL,      NULL };

  xcw_input_t input = { NULL, 0, NULL, 0 };
  xcw_input_t* inputp = malloc(sizeof(xcw_input_t));
  *inputp = input;
  argp_parse(&parser, argc, argv, 0, NULL, inputp);
  if (inputp->ksl == NULL) {
    xcw_fail(EX_USAGE, "missing CHARACTERS argument\n");
  }
  return inputp;
}

int
main(int argc, char** argv)
{
  xcw_input_t* input = parse_args(argc, argv);
  xcw_state_t* state;
  initialise_xorg(&state);
  state->input = input;
  initialise_input(state);

  xcb_window_t* windows;
  int windows_size;
  initialise_tracked_windows(state, &windows, &windows_size);
  initialise_window_tracking(state, windows, windows_size);
  free(windows);

  if (state->wsetups_size == 0) {
    xcw_exit_no_match();
  } else if (state->wsetups_size == 1) {
    wsetup_choose(state, &(state->wsetups[0]));
  } else {
    overlays_set_text(state);
  }

  xcb_generic_event_t* event;
  while (1)
    if ((event = xcb_poll_for_event(state->xcon))) {
      switch (event->response_type & ~0x80) {
        case 0: {
          xcb_generic_error_t* evterr = (xcb_generic_error_t*)event;
          xcw_die("event loop error: %d\n", evterr->error_code);
          break;
        }
        case XCB_EXPOSE: {
          overlays_set_text(state);
          break;
        }
        case XCB_KEY_PRESS: {
          handle_keypress(state, (xcb_key_press_event_t*)event);
          break;
        }
      }

      free(event);
    }

  return 0;
}
