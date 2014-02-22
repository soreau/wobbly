/**************************************************************************
 *
 * Copyright 2014 Scott Moreau <oreaus@gmail.com>
 * All Rights Reserved.
 *
 **************************************************************************/

#include <stdio.h>

#include <GLES2/gl2.h>

#define WOBBLY_FRICTION 3
#define WOBBLY_SPRING_K 8

struct surface {
   void *ww;
   int x, y, width, height;
   int x_cells, y_cells;
   int grabbed, synced;
   int vertex_count;
   GLfloat *v;
   struct {
      void *data;
      void *uv;
      int width;
      int height;
   } tex;
};

struct window {
   int width, height;
};

int
wobbly_init(struct surface *surface);
void
wobbly_fini(struct surface *surface);
void
wobbly_grab_notify(struct surface *surface, int x, int y);
void
wobbly_ungrab_notify(struct surface *surface);
void
wobbly_resize_notify(struct surface *surface);
void
wobbly_move_notify(struct surface *surface, int dx, int dy);
void
wobbly_prepare_paint(struct surface *surface, int msSinceLastPaint);
void
wobbly_done_paint(struct surface *surface);
void
wobbly_add_geometry(struct surface *surface);
