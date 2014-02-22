/**************************************************************************
 *
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Copyright 2014 Scott Moreau <oreaus@gmail.com>
 * All Rights Reserved.
 *
 **************************************************************************/

/*
 * Render a wobbly surface with X/EGL and OpenGL ES 2.x
 */

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <EGL/egl.h>

#include "wobbly.h"
#include "image-loader.h"

struct shared_context {
   Display *x_dpy;
   Window x_win;
   EGLDisplay egl_dpy;
   EGLSurface egl_surf;
   struct window window;
   struct surface surface;
   struct timeval t1;
};

static int last_x = 0, last_y = 0, redraw = 0, running = 1, render_mode = 0, pointer[2];
static GLint u_matrix = -1;
static GLint attr_pos = 0, attr_color = 1, attr_texture = 2;


static void
make_identity_matrix(GLfloat *m)
{
   memset(m, 0, 16 * sizeof (GLfloat));
   m[0] = 1.0;
   m[5] = 1.0;
   m[10] = 1.0;
   m[15] = 1.0;
}

static void
make_translation_matrix(GLfloat x, GLfloat y, GLfloat *m)
{
   memset(m, 0, 16 * sizeof (GLfloat));

   m[0] = 1.0f;
   m[5] = 1.0f;
   m[10] = 1.0f;
   m[12] = x;
   m[13] = y;
   m[15] = 1.0f;
}

static void
make_scale_matrix(GLfloat xs, GLfloat ys, GLfloat zs, GLfloat *m)
{
   int i;
   for (i = 0; i < 16; i++)
      m[i] = 0.0;
   m[0] = xs;
   m[5] = ys;
   m[10] = zs;
   m[15] = 1.0;
}

static void
mul_matrix(GLfloat *prod, const GLfloat *a, const GLfloat *b)
{
#define A(row,col)  a[(col<<2)+row]
#define B(row,col)  b[(col<<2)+row]
#define P(row,col)  p[(col<<2)+row]
   GLfloat p[16];
   GLint i;
   for (i = 0; i < 4; i++) {
      const GLfloat ai0=A(i,0),  ai1=A(i,1),  ai2=A(i,2),  ai3=A(i,3);
      P(i,0) = ai0 * B(0,0) + ai1 * B(1,0) + ai2 * B(2,0) + ai3 * B(3,0);
      P(i,1) = ai0 * B(0,1) + ai1 * B(1,1) + ai2 * B(2,1) + ai3 * B(3,1);
      P(i,2) = ai0 * B(0,2) + ai1 * B(1,2) + ai2 * B(2,2) + ai3 * B(3,2);
      P(i,3) = ai0 * B(0,3) + ai1 * B(1,3) + ai2 * B(2,3) + ai3 * B(3,3);
   }
   memcpy(prod, p, sizeof(p));
#undef A
#undef B
#undef PROD
}

static void
draw_elements(struct shared_context *context)
{
   GLuint VBO, indexBuffer, textureID, textureUV;
   GLfloat mat[16], trans[16], scale[16], y_flip[16], cursor[2], *verts, *uv, cell_w, cell_h, w, h;
   GLushort *indices, x_pts, y_pts, num_pts;
   struct window *window;
   struct surface *surface;
   int x_cells, y_cells, i, x, y;

   window = &context->window;
   surface = &context->surface;

   /* Viewport needs to be set in our rendering thread */
   glViewport(0, 0, window->width, window->height);

   /* Set modelview/projection matrix */
   make_identity_matrix(mat);
   make_identity_matrix(y_flip);
   y_flip[5] = -1;
   make_translation_matrix(-1.0f, -1.0f, trans);
   make_scale_matrix(2.0f / window->width, 2.0f / window->height, 1.0, scale);
   mul_matrix(mat, mat, y_flip);
   mul_matrix(mat, mat, trans);
   mul_matrix(mat, mat, scale);
   glUniformMatrix4fv(u_matrix, 1, GL_FALSE, mat);

   /* Variable assignment */
   w = surface->width;
   h = surface->height;

   x_cells = surface->x_cells;
   y_cells = surface->y_cells;

   cell_w = w / x_cells;
   cell_h = h / y_cells;

   x_pts = x_cells + 1;
   y_pts = y_cells + 1;
   num_pts = x_pts * y_pts;

   /* Memory allocation */
   if (surface->synced) {
      verts = malloc(sizeof (GLfloat) * num_pts * 2);
      if (!verts)
         return;
      
      uv = malloc(sizeof (GLfloat) * num_pts * 2);
      if (!uv)
         return;

      /* Compute vertices and texture coordinates */
      for (y = 0, i = 0; y < y_pts; y++) {
         float y1 = y * cell_h;
         for (x = 0; x < x_pts; x++) {
            float x1 = x * cell_w;
            *(verts + i++) = x1 + surface->x;
            *(verts + i++) = y1 + surface->y;
            *(uv + (i - 2)) = x1 / w;
            *(uv + (i - 1)) = 1.0 - (y1 / h);
         }
      }
   } else {
      verts = uv = NULL;
   }

   indices = malloc(sizeof (GLushort) * x_cells * y_cells * 6);
   if (!indices)
      return;

   /* Compute indices */
   for (y = 0, i = 0; y < y_cells; y++)
      for (x = 0; x < x_cells; x++) {
         *(indices + i++) = y * x_pts + x;
         *(indices + i++) = y * x_pts + x + 1;
         *(indices + i++) = (y + 1) * x_pts + x;

         *(indices + i++) = y * x_pts + x + 1;
         *(indices + i++) = (y + 1) * x_pts + x + 1;
         *(indices + i++) = (y + 1) * x_pts + x;
      }

   /* Setup buffers */
   glGenBuffers(1, &VBO);
   glGenBuffers(1, &indexBuffer);
   glGenBuffers(1, &textureUV);
   glGenTextures(1, &textureID);

   glEnableVertexAttribArray(attr_pos);
   glEnableVertexAttribArray(attr_texture);

   glBindBuffer(GL_ARRAY_BUFFER, VBO);
   glBufferData(GL_ARRAY_BUFFER, sizeof (GLfloat) * num_pts * 2, surface->synced ? verts : surface->v, GL_STATIC_DRAW);
   glVertexAttribPointer(attr_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);

   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, textureID);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                  surface->tex.width, surface->tex.height,
                  0, GL_RGB, GL_UNSIGNED_BYTE, surface->tex.data);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

   glBindBuffer(GL_ARRAY_BUFFER, textureUV);
   glBufferData(GL_ARRAY_BUFFER, sizeof (GLfloat) * num_pts * 2, surface->synced ? uv : surface->tex.uv, GL_STATIC_DRAW);
   glVertexAttribPointer(attr_texture, 2, GL_FLOAT, GL_FALSE, 0, 0);

   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof (GLushort) * x_cells * y_cells * 6, indices, GL_STATIC_DRAW);

   /* Clear buffers */
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   /* Draw surface */
   for (i = 0; i < x_cells * y_cells * 2; i++) {
      GLint mode;
      switch (render_mode) {
         case 0:
            mode = GL_TRIANGLES;
            break;
         case 1:
            mode = GL_LINE_LOOP;
            break;
         case 2:
            mode = GL_POINTS;
            break;
         default:
            break;
      }
      glDrawElements(mode, 3, GL_UNSIGNED_SHORT, (const GLvoid*) (i * 3 * sizeof(GLushort)));
   }

   /* Draw point at cursor hotspot */
   cursor[0] = ((float) (pointer[0]));
   cursor[1] = ((float) (pointer[1]));

   glBindBuffer(GL_ARRAY_BUFFER, VBO);
   glBufferData(GL_ARRAY_BUFFER, sizeof (GLfloat) * 2, cursor, GL_STATIC_DRAW);

   glDrawArrays(GL_POINTS, 0, 1);

   /* Clean up */
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
   glBindTexture(GL_TEXTURE_2D, 0);

   glDisableVertexAttribArray(attr_pos);
   glDisableVertexAttribArray(attr_texture);

   glDeleteBuffers(1, &VBO);
   glDeleteBuffers(1, &indexBuffer);
   glDeleteBuffers(1, &textureUV);
   glDeleteTextures(1, &textureID);

   free(uv);
   free(verts);
   free(indices);
}

static void
prepare_paint(struct surface *surface, int msSinceLastPaint)
{
   wobbly_prepare_paint(surface, msSinceLastPaint);
}

static void
done_paint(struct surface *surface)
{
   wobbly_done_paint(surface);
}

static void
add_geometry(struct surface *surface)
{
   wobbly_add_geometry(surface);
}

static void
draw(struct shared_context *context)
{
   struct timeval *t1, t2;
   double elapsedTime;

   t1 = &context->t1;
   gettimeofday(&t2, NULL);

   elapsedTime = (t2.tv_sec - t1->tv_sec) * 1000.0;      // sec to ms
   elapsedTime += (t2.tv_usec - t1->tv_usec) / 1000.0;   // us to ms

   prepare_paint(&context->surface, (int) elapsedTime);

   gettimeofday(t1, NULL);

   add_geometry(&context->surface);

   draw_elements(context);

   done_paint(&context->surface);
}

/* new window size or exposure */
static void
reshape(struct shared_context *context, int width, int height)
{
   context->window.width = width;
   context->window.height = height;
   redraw = 1;
}


static void
create_shaders(void)
{
   static const char *fragShaderText =
      "precision mediump float;\n"
      "varying vec4 v_color;\n"
      "varying vec2 v_texcoord;\n"
      "uniform sampler2D tex;\n"
      "void main() {\n"
      "   gl_FragColor = texture2D(tex, v_texcoord);\n"
      "}\n";
   static const char *vertShaderText =
      "uniform mat4 modelviewProjection;\n"
      "attribute vec4 pos;\n"
      "attribute vec4 color;\n"
      "varying vec4 v_color;\n"
      "attribute vec2 texcoord;\n"
      "varying vec2 v_texcoord;\n"
      "void main() {\n"
      "   gl_Position = modelviewProjection * pos;\n"
      "   gl_PointSize = 4.0;\n"
      "   v_texcoord = texcoord;\n"
      "   v_color = color;\n"
      "}\n";

   GLuint fragShader, vertShader, program;
   GLint stat;

   fragShader = glCreateShader(GL_FRAGMENT_SHADER);
   glShaderSource(fragShader, 1, (const char **) &fragShaderText, NULL);
   glCompileShader(fragShader);
   glGetShaderiv(fragShader, GL_COMPILE_STATUS, &stat);
   if (!stat) {
      printf("Error: fragment shader did not compile!\n");
      exit(1);
   }

   vertShader = glCreateShader(GL_VERTEX_SHADER);
   glShaderSource(vertShader, 1, (const char **) &vertShaderText, NULL);
   glCompileShader(vertShader);
   glGetShaderiv(vertShader, GL_COMPILE_STATUS, &stat);
   if (!stat) {
      printf("Error: vertex shader did not compile!\n");
      exit(1);
   }

   program = glCreateProgram();
   glAttachShader(program, fragShader);
   glAttachShader(program, vertShader);
   glLinkProgram(program);

   glGetProgramiv(program, GL_LINK_STATUS, &stat);
   if (!stat) {
      char log[1000];
      GLsizei len;
      glGetProgramInfoLog(program, 1000, &len, log);
      printf("Error: linking:\n%s\n", log);
      exit(1);
   }

   glUseProgram(program);
   
   glBindAttribLocation(program, attr_pos, "pos");
   glBindAttribLocation(program, attr_color, "color");
   glBindAttribLocation(program, attr_texture, "texcoord");
   glLinkProgram(program);

   u_matrix = glGetUniformLocation(program, "modelviewProjection");
}

static int
init(struct shared_context *context)
{
   glClearColor(0.4, 0.4, 0.4, 0.0);

   create_shaders();

   if (!wobbly_init(&context->surface))
	return 0;

   return 1;
}

/*
 * Create an RGB, double-buffered X window.
 * Return the window and context handles.
 */
static void
make_x_window(Display *x_dpy, EGLDisplay egl_dpy,
              const char *name,
              int x, int y, int width, int height,
              Window *winRet,
              EGLContext *ctxRet,
              EGLSurface *surfRet)
{
   static const EGLint attribs[] = {
      EGL_RED_SIZE, 1,
      EGL_GREEN_SIZE, 1,
      EGL_BLUE_SIZE, 1,
      EGL_DEPTH_SIZE, 1,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_NONE
   };
   static const EGLint ctx_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE
   };

   int scrnum;
   XSetWindowAttributes attr;
   unsigned long mask;
   Window root;
   Window win;
   XVisualInfo *visInfo, visTemplate;
   int num_visuals;
   EGLContext ctx;
   EGLConfig config;
   EGLint num_configs;
   EGLint vid;

   scrnum = DefaultScreen( x_dpy );
   root = RootWindow( x_dpy, scrnum );

   if (!eglChooseConfig( egl_dpy, attribs, &config, 1, &num_configs)) {
      printf("Error: couldn't get an EGL visual config\n");
      exit(1);
   }

   assert(config);
   assert(num_configs > 0);

   if (!eglGetConfigAttrib(egl_dpy, config, EGL_NATIVE_VISUAL_ID, &vid)) {
      printf("Error: eglGetConfigAttrib() failed\n");
      exit(1);
   }

   /* The X window visual must match the EGL config */
   visTemplate.visualid = vid;
   visInfo = XGetVisualInfo(x_dpy, VisualIDMask, &visTemplate, &num_visuals);
   if (!visInfo) {
      printf("Error: couldn't get X visual\n");
      exit(1);
   }

   /* window attributes */
   attr.background_pixel = 0;
   attr.border_pixel = 0;
   attr.colormap = XCreateColormap( x_dpy, root, visInfo->visual, AllocNone);
   attr.event_mask = StructureNotifyMask | SubstructureNotifyMask | ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
   mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

   win = XCreateWindow( x_dpy, root, 0, 0, width, height,
		        0, visInfo->depth, InputOutput,
		        visInfo->visual, mask, &attr );

   /* set hints and properties */
   {
      XSizeHints sizehints;
      sizehints.x = x;
      sizehints.y = y;
      sizehints.width  = width;
      sizehints.height = height;
      sizehints.flags = USSize | USPosition;
      XSetNormalHints(x_dpy, win, &sizehints);
      XSetStandardProperties(x_dpy, win, name, name,
                              None, (char **)NULL, 0, &sizehints);
   }

   Atom wmDelete = XInternAtom(x_dpy, "WM_DELETE_WINDOW", 1);
   XSetWMProtocols(x_dpy, win, &wmDelete, 1);

   eglBindAPI(EGL_OPENGL_ES_API);

   ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, ctx_attribs);
   if (!ctx) {
      printf("Error: eglCreateContext failed\n");
      exit(1);
   }

   *surfRet = eglCreateWindowSurface(egl_dpy, config, win, NULL);
   if (!*surfRet) {
      printf("Error: eglCreateWindowSurface failed\n");
      exit(1);
   }

   /* sanity checks */
   {
      EGLint val;
      eglQuerySurface(egl_dpy, *surfRet, EGL_WIDTH, &val);
      assert(val == width);
      eglQuerySurface(egl_dpy, *surfRet, EGL_HEIGHT, &val);
      assert(val == height);
      assert(eglGetConfigAttrib(egl_dpy, config, EGL_SURFACE_TYPE, &val));
      assert(val & EGL_WINDOW_BIT);
   }

   XFree(visInfo);

   *winRet = win;
   *ctxRet = ctx;
}

static int
point_on_surface(struct shared_context *context, GLint x, GLint y)
{
   if (x > context->surface.x &&
       x < context->surface.x + context->surface.width &&
       y > context->surface.y &&
       y < context->surface.y + context->surface.height)
      return 1;

   return 0;
}

static void*
event_loop(void *data)
{
   struct shared_context *context = data;
   struct surface *surface = &context->surface;

   while (running) {
      XEvent event;

      XNextEvent(context->x_dpy, &event);

      switch (event.type) {
      case ButtonPress:
         if (point_on_surface(context, event.xcrossing.x, event.xcrossing.y)) {
            last_x = event.xcrossing.x;
            last_y = event.xcrossing.y;
            surface->grabbed = 1;
            surface->synced = 0;
            wobbly_grab_notify(surface, last_x, last_y);
         }
         break;
      case ButtonRelease:
         surface->grabbed = 0;
         redraw = 1;
         wobbly_ungrab_notify(surface);
         break;
      case KeyPress:
         if (XLookupKeysym(&event.xkey, 0) == XK_Escape) {
            running = 0;
            continue;
         }
         break;
      case ClientMessage:
         running = 0;
         break;
      }

      if (redraw)
         continue;

      switch (event.type) {
      case ConfigureNotify:
         reshape(context, event.xconfigure.width, event.xconfigure.height);
         break;
      case KeyPress:
         {
            char buffer[10];
            int code;
            code = XLookupKeysym(&event.xkey, 0);
            if (code == XK_Right) {
               surface->width += 10;
               wobbly_resize_notify(surface);
            }
            else if (code == XK_Left) {
               surface->width -= 10;
               if (surface->width < 10.0)
                   surface->width = 10.0;
               wobbly_resize_notify(surface);
            }
            else if (code == XK_Up) {
               surface->height -= 10;
               if (surface->height < 10.0)
                   surface->height = 10.0;
               wobbly_resize_notify(surface);
            }
            else if (code == XK_Down) {
               surface->height += 10;
               wobbly_resize_notify(surface);
            } else if (code == XK_d) {
               surface->x_cells += 1;
            } else if (code == XK_a) {
               surface->x_cells -= 1;
               if (surface->x_cells < 1)
                   surface->x_cells = 1;
            } else if (code == XK_w) {
               surface->y_cells += 1;
            } else if (code == XK_s) {
               surface->y_cells -= 1;
               if (surface->y_cells < 1)
                   surface->y_cells = 1;
	    } else if (code == XK_m) {
               if (++render_mode > 2)
                   render_mode = 0;
            } else {
               XLookupString(&event.xkey, buffer, sizeof(buffer),
                                 NULL, NULL);
               if (buffer[0] == 43) {
		  /* plus */
                  surface->y_cells = ++surface->x_cells;
               }
               else if (buffer[0] == 45) {
		  /* minus */
		  if (surface->y_cells > 1 && surface->x_cells > 1)
                     surface->y_cells = --surface->x_cells;
               }
            }
         }
         redraw = 1;
         break;
      case MotionNotify:
         {
            pointer[0] = event.xmotion.x;
            pointer[1] = event.xmotion.y;
            if (surface->grabbed) {
               int dx = (pointer[0] - last_x);
               int dy = (pointer[1] - last_y);
               last_x = pointer[0];
               last_y = pointer[1];
               wobbly_move_notify(surface, dx, dy);
            }
            redraw = 1;
	 }
         break;
      default:
         ; /* process next event */
      }
   }
   pthread_exit(NULL);
}

static void
usage(void)
{
   printf("Usage:\n");
   printf("  -display <displayname>  set the display to run on\n");
   printf("  -texture texture.png    set the image to use\n");
   printf("  -info                   display OpenGL renderer info\n\n");
   printf("Hotkeys:\n");
   printf("   a/d/w/s:               adjust surface x/y cells\n");
   printf("   +/-:                   adjust surface x/y cells in sync\n");
   printf("   arrow keys:            adjust surface width/height\n");
   printf("   m:                     cycle through primitive modes\n");
}


int
main(int argc, char *argv[])
{
   const int winWidth = 1000, winHeight = 500;
   pthread_t threads[1];
   struct shared_context *context;
   struct surface *surface;
   Window win;
   EGLContext egl_ctx;
   char *dpyName = NULL;
   char *texFile = NULL;
   GLboolean printInfo = GL_FALSE;
   EGLint egl_major, egl_minor;
   int i;
   const char *s;

   for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-display") == 0) {
         dpyName = argv[i+1];
         i++;
      }
      if (strcmp(argv[i], "-texture") == 0) {
         texFile = argv[i+1];
         i++;
      }
      else if (strcmp(argv[i], "-info") == 0) {
         printInfo = GL_TRUE;
      }
      else {
         usage();
         return -1;
      }
   }

   context = malloc(sizeof (*context));

   if (!context)
      return -1;

   XInitThreads();
   context->x_dpy = XOpenDisplay(dpyName);
   if (!context->x_dpy) {
      printf("Error: couldn't open display %s\n",
	     dpyName ? dpyName : getenv("DISPLAY"));
      return -1;
   }

   context->egl_dpy = eglGetDisplay(context->x_dpy);
   if (!context->egl_dpy) {
      printf("Error: eglGetDisplay() failed\n");
      return -1;
   }

   if (!eglInitialize(context->egl_dpy, &egl_major, &egl_minor)) {
      printf("Error: eglInitialize() failed\n");
      return -1;
   }

   s = eglQueryString(context->egl_dpy, EGL_VERSION);
   if (printInfo)
      printf("EGL_VERSION = %s\n", s);

   s = eglQueryString(context->egl_dpy, EGL_VENDOR);
   if (printInfo)
      printf("EGL_VENDOR = %s\n", s);

   s = eglQueryString(context->egl_dpy, EGL_EXTENSIONS);
   if (printInfo)
      printf("EGL_EXTENSIONS = %s\n", s);

   s = eglQueryString(context->egl_dpy, EGL_CLIENT_APIS);
   if (printInfo)
      printf("EGL_CLIENT_APIS = %s\n", s);

   make_x_window(context->x_dpy, context->egl_dpy,
                 "OpenGL ES 2.x wobbly", 0, 0, winWidth, winHeight,
                 &win, &egl_ctx, &context->egl_surf);

   context->x_win = win;
   XMapWindow(context->x_dpy, win);
   if (!eglMakeCurrent(context->egl_dpy, context->egl_surf, context->egl_surf, egl_ctx)) {
      printf("Error: eglMakeCurrent() failed\n");
      return -1;
   }

   if (printInfo) {
      printf("GL_RENDERER   = %s\n", (char *) glGetString(GL_RENDERER));
      printf("GL_VERSION    = %s\n", (char *) glGetString(GL_VERSION));
      printf("GL_VENDOR     = %s\n", (char *) glGetString(GL_VENDOR));
      printf("GL_EXTENSIONS = %s\n", (char *) glGetString(GL_EXTENSIONS));
   }

   surface = &context->surface;

   surface->width = 400;
   surface->height = 200;
   surface->x = winWidth / 2 - surface->width / 2;
   surface->y = winHeight / 2 - surface->height / 2;
   surface->grabbed = 0;
   surface->synced = 1;
   surface->x_cells = 8;
   surface->y_cells = 8;
   surface->v = NULL;
   surface->tex.uv = NULL;

   i = loadPngImage(texFile ? texFile : "texture.png",
		&surface->tex.width, &surface->tex.height, &surface->tex.data);

   if (!i) {
      surface->tex.data = NULL;
      surface->tex.width = 0;
      surface->tex.height = 0;
   }

   if (!init(context))
      goto cleanup;

   /* Set initial projection/viewing transformation.
    * We can't be sure we'll get a ConfigureNotify event when the window
    * first appears.
    */
   reshape(context, winWidth, winHeight);

   /* init reference timer */
   gettimeofday(&context->t1, NULL);

   pthread_create(threads, NULL, event_loop, context);

   while(running) {
      redraw = 1;
      draw(context);
      eglSwapBuffers(context->egl_dpy, context->egl_surf);
      redraw = 0;
      usleep(16000);
   }

   pthread_join(threads[0], NULL);

   wobbly_fini(&context->surface);

cleanup:
   eglDestroyContext(context->egl_dpy, egl_ctx);
   eglDestroySurface(context->egl_dpy, context->egl_surf);
   eglTerminate(context->egl_dpy);

   XDestroyWindow(context->x_dpy, context->x_win);
   XCloseDisplay(context->x_dpy);

   free(surface->tex.data);
   free(context);

   return 0;
}
