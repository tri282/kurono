/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_UI_LAYOUT_H
#define KURONO_UI_LAYOUT_H

#include <stddef.h>

/*
 * View ids, region roles and per-view layout variants (DESIGN.md VIII.4).
 * Header-only so tools/keymap_test can assert it; listgeom.h is the
 * precedent.
 */

#define TUI_MIN_W 60
#define TUI_MIN_H 15
#define LAYOUT_MAX_COLS 3

/* Append-only: ACT_VIEW_SET's arg is config-visible (design II.1). */
typedef enum {
  VIEW_LIBRARY = 0, VIEW_DIRECTORIES, VIEW_PLAYLISTS, VIEW_NOW_PLAYING, VIEW_SETTINGS,
  VIEW_COUNT
} UiView;

/* Append-only, like Action and StyleSlot. */
typedef enum {
  ROLE_PARENT = 0, ROLE_MAIN, ROLE_FACET, ROLE_INFO, ROLE_QUEUE, ROLE_COVER, ROLE_SOURCES,
  ROLE_COUNT
} RegionRole;

typedef struct { RegionRole role; int x, y, w, h; } Region;

typedef struct {
  int    w, h;
  int    too_small;
  int    body_y, body_h;
  int    status_y;
  int    nregions;
  Region region[ROLE_COUNT];
} TuiLayout;

typedef struct { RegionRole role; int pct; int min_w; } LayoutCol;
typedef struct { size_t ncols; LayoutCol col[LAYOUT_MAX_COLS]; } LayoutVariant;

/*
 * layout_variants: the ordered variants for view `v`, first match wins
 * (design II.3). The last is a single full-width column, used
 * unconditionally. Sets *n to the count; an out-of-range v is Library.
 */
static inline const LayoutVariant *
layout_variants(UiView v, size_t * n)
{
  /* Function-local: a file-scope static const unused by one includer
     trips -Wunused-const-variable under -Werror (design II.4). */
  static const LayoutVariant library[] = {
    { 3, { { ROLE_FACET, 20, 12 }, { ROLE_MAIN, 50, 20 }, { ROLE_INFO, 30, 16 } } },
    { 2, { { ROLE_MAIN,  70, 20 }, { ROLE_INFO, 30, 16 } } },
    { 1, { { ROLE_MAIN, 100,  1 } } },
  };
  static const LayoutVariant directories[] = {
    { 3, { { ROLE_PARENT, 25, 10 }, { ROLE_MAIN, 30, 16 }, { ROLE_INFO, 45, 16 } } },
    { 1, { { ROLE_MAIN, 100,  1 } } },
  };
  static const LayoutVariant playlists[] = {
    { 3, { { ROLE_SOURCES, 20, 10 }, { ROLE_QUEUE, 50, 20 }, { ROLE_INFO, 30, 16 } } },
    { 2, { { ROLE_QUEUE,   70, 20 }, { ROLE_INFO, 30, 16 } } },
    { 1, { { ROLE_QUEUE,  100,  1 } } },
  };
  static const LayoutVariant now_playing[] = {
    { 1, { { ROLE_INFO, 100, 1 } } },
  };
  static const LayoutVariant settings[] = {
    { 1, { { ROLE_MAIN, 100, 1 } } },
  };

  switch (v) {
  case VIEW_DIRECTORIES:
    *n = sizeof directories / sizeof directories[0];
    return directories;
  case VIEW_PLAYLISTS:
    *n = sizeof playlists / sizeof playlists[0];
    return playlists;
  case VIEW_NOW_PLAYING:
    *n = sizeof now_playing / sizeof now_playing[0];
    return now_playing;
  case VIEW_SETTINGS:
    *n = sizeof settings / sizeof settings[0];
    return settings;
  case VIEW_LIBRARY:
  case VIEW_COUNT:
  default:
    *n = sizeof library / sizeof library[0];
    return library;
  }
} /* layout_variants */

/*
 * layout_compute: lay view `v` out in a w x h terminal into *out. Takes
 * the view, the size, whether the Library facet is shown, and the
 * result. Pure arithmetic: below the TUI_MIN_W x TUI_MIN_H floor it sets
 * too_small and allocates nothing; otherwise the first variant whose
 * columns all meet their min_w wins (the last variant is exempt).
 */
static inline void
layout_compute(UiView v, int w, int h, int facet_on, TuiLayout * out)
{
  *out = (TuiLayout) { 0 };
  out->w = w;
  out->h = h;

  if (w < TUI_MIN_W || h < TUI_MIN_H) {
    out->too_small = 1;
    return;
  }

  out->body_y   = 1;
  out->body_h   = h - 2;
  out->status_y = h - 1;

  size_t               nvar = 0;
  const LayoutVariant * var = layout_variants(v, &nvar);

  for (size_t vi = 0; vi < nvar; vi++) {
    const LayoutVariant * lv   = &var[vi];
    int                   last = (vi + 1 == nvar);
    int                   skip = 0;
    int                   cw[LAYOUT_MAX_COLS];
    int                   avail = w - (int) (lv->ncols - 1);
    int                   used  = 0;

    for (size_t i = 0; i < lv->ncols; i++) {
      if (lv->col[i].role == ROLE_FACET && !facet_on) {
        skip = 1;
      }
      if (i + 1 < lv->ncols) {
        cw[i] = avail * lv->col[i].pct / 100;
        used += cw[i];
      } else {
        cw[i] = avail - used;
      }
      if (!last && cw[i] < lv->col[i].min_w) {
        skip = 1;
      }
    }
    if (skip) {
      continue;
    }

    int x = 0;
    for (size_t i = 0; i < lv->ncols; i++) {
      out->region[i] = (Region) { lv->col[i].role, x, out->body_y, cw[i], out->body_h };
      x += cw[i] + 1;
    }
    out->nregions = (int) lv->ncols;
    return;
  }
} /* layout_compute */

/*
 * layout_find: the region carrying `r` in *l, or NULL when the layout
 * did not allocate that role.
 */
static inline const Region *
layout_find(const TuiLayout * l, RegionRole r)
{
  for (int i = 0; i < l->nregions; i++) {
    if (l->region[i].role == r) {
      return &l->region[i];
    }
  }
  return NULL;
} /* layout_find */

/*
 * layout_list_top: rows reserved above a role's list in view `v` (the
 * Library filter line, design IV.5). Takes the view and role, returns
 * the row count.
 */
static inline int
layout_list_top(UiView v, RegionRole r)
{
  return (v == VIEW_LIBRARY && (r == ROLE_FACET || r == ROLE_MAIN)) ? 1 : 0;
} /* layout_list_top */

#endif // KURONO_UI_LAYOUT_H
