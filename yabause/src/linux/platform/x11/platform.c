#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#define LOGD
#define LOGE printf


static int quit = 0;

#if defined(__arm__) && defined(__linux__)
  static fbdev_window *window;
#elif defined(__linux__)
  static Window window;
#endif  

static Display *x11disp;
static XVisualInfo *visual;
static Colormap colormap;

static EGLDisplay eglDisp;
static EGLContext context;
static EGLContext bg_context;
static EGLSurface surface;
static EGLSurface g_Pbuffer;
static EGLConfig config;

static EGLint configAttributes[] =
{
  /* DO NOT MODIFY. */
  /* These attributes are in a known order and may be re-written at initialization according to application requests. */
  EGL_SAMPLES, 4,

  EGL_ALPHA_SIZE,  0,
  #if defined(__arm__)
  EGL_RED_SIZE,8,
  EGL_GREEN_SIZE,  8,
  EGL_BLUE_SIZE,   8,
  EGL_BUFFER_SIZE, 32,
  #else
  EGL_RED_SIZE,8,
  EGL_GREEN_SIZE,  8,
  EGL_BLUE_SIZE,   8,
  EGL_BUFFER_SIZE, 32,
  #endif
  EGL_STENCIL_SIZE,0,
  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,/* This field will be completed according to application request. */

  EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT ,

  EGL_DEPTH_SIZE,  16,

  /* MODIFY BELOW HERE. */
  /* If you need to add or override EGL attributes from above, do it below here. */

  EGL_NONE
};

static EGLint pbuffer_attribs[] = {
  EGL_WIDTH, 8,
  EGL_HEIGHT, 8,
  //EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
  //EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
  //EGL_LARGEST_PBUFFER, EGL_TRUE,
  EGL_NONE
};

static EGLint contextAttributes[] =
{
  EGL_CONTEXT_CLIENT_VERSION, 3, /* This field will be completed according to application request. */
  EGL_NONE, EGL_NONE,/* If we use OpenGL ES 3.1, we will have to use EGL_CONTEXT_MINOR_VERSION,
  and we overwrite these fields. */
  EGL_NONE
};

/**
 * Using the defaults (EGL_RENDER_BUFFER = EGL_BACK_BUFFER).
 */
static EGLint eglwindowAttributes[] =
{
  EGL_NONE
  /*
   * Uncomment and remove EGL_NONE above to specify EGL_RENDER_BUFFER value to eglCreateWindowSurface.
   * EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
   * EGL_NONE
   */
};

static EGLConfig findConfig(EGLDisplay eglDisp, int strictMatch)
{
  EGLConfig configToReturn;
  EGLConfig *configsArray = NULL;
  EGLint numberOfConfigs = 0;
  EGLBoolean success = EGL_FALSE;
  int matchFound = 0;
  int matchingConfig = -1;

  /* Enumerate available EGL configurations which match or exceed our required attribute list. */
  success = eglChooseConfig(eglDisp, configAttributes, NULL, 0, &numberOfConfigs);
  if(success != EGL_TRUE)
  {
    EGLint error = eglGetError();
    printf("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
    printf("Failed to enumerate EGL configs at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }

  LOGD("Number of configs found is %d\n", numberOfConfigs);

  if (numberOfConfigs == 0)
  {
    LOGD("Disabling AntiAliasing to try and find a config.\n");
    configAttributes[1] = EGL_DONT_CARE;
    success = eglChooseConfig(eglDisp, configAttributes, NULL, 0, &numberOfConfigs);
    if(success != EGL_TRUE)
    {
      EGLint error = eglGetError();
      printf("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
      printf("Failed to enumerate EGL configs at %s:%i\n", __FILE__, __LINE__);
      exit(1);
    }

    if (numberOfConfigs == 0)
    {
      printf("No configs found with the requested attributes.\n");
      exit(1);
    }
    else
    {
      LOGD("Configs found when antialiasing disabled.\n ");
    }
  }

  /* Allocate space for all EGL configs available and get them. */
  configsArray = (EGLConfig *)calloc(numberOfConfigs, sizeof(EGLConfig));
  if(configsArray == NULL)
  {
    printf("Out of memory at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }
  success = eglChooseConfig(eglDisp, configAttributes, configsArray, numberOfConfigs, &numberOfConfigs);
  if(success != EGL_TRUE)
  {
    EGLint error = eglGetError();
    printf("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
    printf("Failed to enumerate EGL configs at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }

  if (strictMatch)
  {
    /*
     * Loop through the EGL configs to find a color depth match.
     * Note: This is necessary, since EGL considers a higher color depth than requested to be 'better'
     * even though this may force the driver to use a slow color conversion blitting routine. 
     */
    EGLint redSize = configAttributes[5];
    EGLint greenSize = configAttributes[7];
    EGLint blueSize = configAttributes[9];

    for(int configsIndex = 0; (configsIndex < numberOfConfigs) && !matchFound; configsIndex++)
    {
      EGLint attributeValue = 0;

      success = eglGetConfigAttrib(eglDisp, configsArray[configsIndex], EGL_RED_SIZE, &attributeValue);
      if(success != EGL_TRUE)
      {
        EGLint error = eglGetError();
        printf("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
        printf("Failed to get EGL attribute at %s:%i\n", __FILE__, __LINE__);
        exit(1);
      }

      if(attributeValue == redSize)
      {
        success = eglGetConfigAttrib(eglDisp, configsArray[configsIndex], EGL_GREEN_SIZE, &attributeValue);
        if(success != EGL_TRUE)
        {
          EGLint error = eglGetError();
          printf("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
          printf("Failed to get EGL attribute at %s:%i\n", __FILE__, __LINE__);
          exit(1);
        }

        if(attributeValue == greenSize)
        {
          success = eglGetConfigAttrib(eglDisp, configsArray[configsIndex], EGL_BLUE_SIZE, &attributeValue);
          if(success != EGL_TRUE)
          {
            EGLint error = eglGetError();
            printf("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
            printf("Failed to get EGL attribute at %s:%i\n", __FILE__, __LINE__);
            exit(1);
          }

          if(attributeValue == blueSize) 
          {
            matchFound = 1;
            matchingConfig = configsIndex;
          }
        }
      }
    }
  }
  else
  {
    /* If not strict we can use any matching config. */
    matchingConfig = 0;
    matchFound = 1;
  }

  if(!matchFound)
  {
    printf("Failed to find matching EGL config at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }

  /* Return the matching config. */
  configToReturn = configsArray[matchingConfig];

  free(configsArray);
  configsArray = NULL;

  return configToReturn;
}


static int wait_for_map(Display *display, XEvent *event, char *windowPointer) 
{
  return (event->type == MapNotify && event->xmap.window == (*((Window*)windowPointer)));
}

int setupX11(int w, int h) {
  XSizeHints sizeHints;
  XEvent event;
  XVisualInfo visualInfoTemplate;
  XSetWindowAttributes windowAttributes;

  unsigned long mask;
  long screen;
  int visualID, numberOfVisuals;

  x11disp = XOpenDisplay(NULL);
  screen = DefaultScreen(x11disp);

  eglGetConfigAttrib(eglDisp, config, EGL_NATIVE_VISUAL_ID, &visualID);
  visualInfoTemplate.visualid = visualID;
  visual = XGetVisualInfo(x11disp, VisualIDMask, &visualInfoTemplate, &numberOfVisuals);
  if (visual == NULL)
  {
    LOGE("Couldn't get X visual info\n");
    return 0;
  }
  colormap = XCreateColormap(x11disp, RootWindow(x11disp, screen), visual->visual, AllocNone);

  windowAttributes.colormap = colormap;
  windowAttributes.background_pixel = 0xFFFFFFFF;
  windowAttributes.border_pixel = 0;
  windowAttributes.event_mask = StructureNotifyMask | ExposureMask;

  mask = CWBackPixel | CWBorderPixel | CWEventMask | CWColormap;

  window = XCreateWindow(x11disp, RootWindow(x11disp, screen), 0, 0, w, h,
            0, visual->depth, InputOutput, visual->visual, mask, &windowAttributes);
  sizeHints.flags = USPosition;
  sizeHints.x = 10;
  sizeHints.y = 10;

  XSetStandardProperties(x11disp, window, "uoYabause", "", None, 0, 0, &sizeHints);
  XMapWindow(x11disp, window);
  XIfEvent(x11disp, &event, wait_for_map, (char*)&window);
  XSetWMColormapWindows(x11disp, window, &window, 1);
  XFlush(x11disp);

  XSelectInput(x11disp, window, KeyPressMask | ExposureMask | EnterWindowMask
                     | LeaveWindowMask | PointerMotionMask | VisibilityChangeMask | ButtonPressMask
                     | ButtonReleaseMask | StructureNotifyMask);
  return 1;
}

int setupEgl(int w, int h) {
  int success;

#if defined(__arm__)
  /* Linux on ARM */
  eglDisp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
  /* Desktop Linux */
  x11disp = XOpenDisplay(NULL);
  eglDisp = eglGetDisplay(x11disp);
#endif
  if(eglDisp == EGL_NO_DISPLAY)
  {
    EGLint error = eglGetError();
    LOGE("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
    LOGE("No EGL Display available at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }

  /* Initialize EGL. */
  success = eglInitialize(eglDisp, NULL, NULL);
  if(success != EGL_TRUE)
  {
    EGLint error = eglGetError();
    LOGE("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
    LOGE("Failed to initialize EGL at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }

  /* 
  * Find a matching config and store it in our static variable.
  * On ARM devices perform a strict match to ensure we get the best performance.
  * On desktop devices perform a loose match to ensure greatest compatability.
  */
#if defined(__arm__)
  config = findConfig(eglDisp, 1);
#else
  config = findConfig(eglDisp, 0);
#endif

#if defined(__linux__) && !defined(__arm__)
  setupX11(w, h);
#endif
  /* Create a surface. */
  surface = eglCreateWindowSurface(eglDisp, config, (EGLNativeWindowType)(window), eglwindowAttributes);
  if(surface == EGL_NO_SURFACE)
  {
    EGLint error = eglGetError();
    LOGE("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
    LOGE("Failed to create EGL surface at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }
  /* Unconditionally bind to OpenGL ES API as we exit this function, since it's the default. */
  eglBindAPI(EGL_OPENGL_ES_API);

  context = eglCreateContext(eglDisp, config, EGL_NO_CONTEXT, contextAttributes);
  if(context == EGL_NO_CONTEXT)
  {
    EGLint error = eglGetError();
    LOGE("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
    LOGE("Failed to create EGL context at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }

  bg_context = eglCreateContext(eglDisp, config, context, contextAttributes);
  if(bg_context == EGL_NO_CONTEXT)
  {
    EGLint error = eglGetError();
    LOGE("eglGetError(): %i (0x%.4x)\n", (int)error, (int)error);
    LOGE("Failed to create EGL shared context at %s:%i\n", __FILE__, __LINE__);
    exit(1);
  }

  pbuffer_attribs[1] = w;
  pbuffer_attribs[3] = h;

  g_Pbuffer = eglCreatePbufferSurface(eglDisp, config, pbuffer_attribs );
  if(g_Pbuffer == EGL_NO_SURFACE)
  {
    LOGE("eglCreatePbufferSurface() returned error %X", eglGetError());
    exit(1);
  }
  eglMakeCurrent(eglDisp, surface, surface, context);
  eglSwapInterval(eglDisp, 0);

  return 1;
}

int platform_SetupOpenGL(int w, int h) {
  return setupEgl(w, h);
}

int platform_YuiRevokeOGLOnThisThread(){
#if defined(YAB_ASYNC_RENDERING)
  eglMakeCurrent(eglDisp, g_Pbuffer, g_Pbuffer, bg_context);
#endif
  return 0;
}

int platform_YuiUseOGLOnThisThread(){
#if defined(YAB_ASYNC_RENDERING)
  eglMakeCurrent(eglDisp, surface, surface, context);
#endif
  return 0;
}

void platform_swapBuffers(void){
  eglSwapBuffers(eglDisp, surface);
  return;
}
int platform_shouldClose(){
  return quit;
}
void platform_Close(){
  quit = 1;
  return;
}
int platform_Deinit(void){
  eglMakeCurrent(eglDisp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(eglDisp, bg_context);
  eglDestroyContext(eglDisp, context);
  eglDestroySurface(eglDisp, surface);
  eglDestroySurface(eglDisp, g_Pbuffer);
  eglTerminate(eglDisp);

  eglDisp = EGL_NO_DISPLAY;
  bg_context = EGL_NO_CONTEXT;
  context = EGL_NO_CONTEXT;
  surface = EGL_NO_SURFACE;
  g_Pbuffer = EGL_NO_SURFACE;

  XCloseDisplay(x11disp);
  
  return 0;
}
void platform_HandleEvent(){
  return;
}



