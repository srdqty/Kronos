/*  Copyright 2006 Guillaume Duhamel
    Copyright 2006 Fabien Coulon
    Copyright 2005 Joost Peters

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <assert.h>
#include <unistd.h>
#include <sys/time.h>

#if defined(_USEGLEW_)
#include <GL/glew.h>
#endif

#include <platform.h>

#include "../yabause.h"
#include "../gameinfo.h"
#include "../yui.h"
#include "../peripheral.h"
#include "../sh2core.h"
#include "../sh2int.h"
#if defined(HAVE_LIBGL) || defined(HAVE_LIBGLES)
#include "../vidogl.h"
#include "../ygl.h"
#endif
#include "../vidsoft.h"
#include "../cs0.h"
#include "../cs2.h"
#include "../cdbase.h"
#include "../scsp.h"
#include "../sndsdl.h"
#include "../sndal.h"
#include "../persdljoy.h"
#ifdef ARCH_IS_LINUX
#include "../perlinuxjoy.h"
#endif
#include "../debug.h"
#include "../m68kcore.h"
#include "../m68kc68k.h"
#include "../vdp1.h"
#include "../vdp2.h"
#include "../cdbase.h"
#include "../peripheral.h"

#define AR (4.0f/3.0f)
#define WINDOW_WIDTH 1200
#define WINDOW_HEIGHT ((int)((float)WINDOW_WIDTH/AR))

#define  WINDOW_WIDTH_LOW 600
#define WINDOW_HEIGHT_LOW ((int)((float)WINDOW_WIDTH_LOW/AR))


M68K_struct * M68KCoreList[] = {
&M68KDummy,
#ifdef HAVE_MUSASHI
&M68KMusashi,
#endif
#ifdef HAVE_C68K
&M68KC68K,
#endif
#ifdef HAVE_Q68
&M68KQ68,
#endif
NULL
};

SH2Interface_struct *SH2CoreList[] = {
&SH2Interpreter,
&SH2DebugInterpreter,
#ifdef TEST_PSP_SH2
&SH2PSP,
#endif
#ifdef SH2_DYNAREC
&SH2Dynarec,
#endif
NULL
};

PerInterface_struct *PERCoreList[] = {
&PERDummy,
#ifdef ARCH_IS_LINUX
&PERLinuxJoy,
#endif
NULL
};

CDInterface *CDCoreList[] = {
&DummyCD,
&ISOCD,
#ifndef UNKNOWN_ARCH
&ArchCD,
#endif
NULL
};

SoundInterface_struct *SNDCoreList[] = {
&SNDDummy,
#ifdef HAVE_LIBSDL
&SNDSDL,
#endif
#ifdef HAVE_LIBAL
&SNDAL,
#endif
NULL
};

VideoInterface_struct *VIDCoreList[] = {
&VIDDummy,
#if defined(HAVE_LIBGL) || defined(HAVE_LIBGLES)
&VIDOGL,
#endif
&VIDSoft,
NULL
};

#ifdef YAB_PORT_OSD
#include "nanovg/nanovg_osdcore.h"
OSD_struct *OSDCoreList[] = {
&OSDNnovg,
NULL
};
#endif

static int g_buf_width = -1;
static int g_buf_height = -1;
static GLuint g_FrameBuffer = 0;
static GLuint g_VertexBuffer = 0;
static GLuint programObject  = 0;
static GLuint positionLoc    = 0;
static GLuint texCoordLoc    = 0;
static GLuint samplerLoc     = 0;
static float swVertices [] = {
   -1.0f, 1.0f, 0, 0,
   1.0f, 1.0f, 1.0f, 0,
   1.0f, -1.0f, 1.0f, 1.0f,
   -1.0f,-1.0f, 0, 1.0f
};

static const float squareVertices [] = {
   -1.0f, 1.0f, 0, 0,
   1.0f, 1.0f, 1.0f, 0,
   1.0f, -1.0f, 1.0f, 1.0f,
   -1.0f,-1.0f, 0, 1.0f
};
static int fullscreen = 0;
static int lowres_mode = 0;

static char biospath[256] = "\0";
static char cdpath[256] = "\0";

yabauseinit_struct yinit;

void YuiErrorMsg(const char * string) {
    fprintf(stderr, "%s\n\r", string);
}

int YuiRevokeOGLOnThisThread(){
  platform_YuiRevokeOGLOnThisThread();
  return 0;
}

int YuiUseOGLOnThisThread(){
  platform_YuiUseOGLOnThisThread();
  return 0;
}

static unsigned long nextFrameTime = 0;
static unsigned long delayUs = 1000000/60;

static int frameskip = 1;

static unsigned long getCurrentTimeUs(unsigned long offset) {
    struct timeval s;

    gettimeofday(&s, NULL);

    return (s.tv_sec * 1000000 + s.tv_usec) - offset;
}

static unsigned long time_left(void)
{
    unsigned long now;

    now = getCurrentTimeUs(0);
    if(nextFrameTime <= now)
        return 0;
    else
        return nextFrameTime - now;
}

static GLuint LoadShader ( GLenum type, const char *shaderSrc )
{
   GLuint shader;
   GLint compiled;

   // Create the shader object
   shader = glCreateShader ( type );
   if ( shader == 0 )
    return 0;
   // Load the shader source
   glShaderSource ( shader, 1, &shaderSrc, NULL );
   // Compile the shader
   glCompileShader ( shader );
   // Check the compile status
   glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );
   if ( !compiled )
   {
      GLint infoLen = 0;
      glGetShaderiv ( shader, GL_INFO_LOG_LENGTH, &infoLen );
      if ( infoLen > 1 )
      {
         char* infoLog = malloc (sizeof(char) * infoLen );
         glGetShaderInfoLog ( shader, infoLen, NULL, infoLog );
         printf ( "Error compiling shader:\n%s\n", infoLog );
         free ( infoLog );
      }
      glDeleteShader ( shader );
      return 0;
   }
   return shader;

}

static int YuiInitProgramForSoftwareRendering()
{
   GLbyte vShaderStr[] =
#if defined (_OGLES3_)
      "#version 300 es \n"
#else
      "#version 330 \n"
#endif
      "in vec2 a_position;   \n"
      "in vec2 a_texCoord;   \n"
      "out vec2 v_texCoord;     \n"
      "void main()                  \n"
      "{                            \n"
      "   gl_Position = vec4(a_position.xy, 0.0, 1.0); \n"
      "   v_texCoord = a_texCoord;  \n"
      "}                            \n";

   GLbyte fShaderStr[] =
#if defined (_OGLES3_)
      "#version 300 es \n"
#else
      "#version 330 \n"
#endif
      "precision mediump float;                            \n"
      "in vec2 v_texCoord;                            \n"
      "uniform sampler2D s_texture;                        \n"
      "out vec4 fragColor;            \n"
      "void main()                                         \n"
      "{                                                   \n"
      "  fragColor = texture2D( s_texture, v_texCoord );\n"
      "}                                                   \n";

   GLuint vertexShader;
   GLuint fragmentShader;
   GLint linked;

   // Load the vertex/fragment shaders
   vertexShader = LoadShader ( GL_VERTEX_SHADER, vShaderStr );
   fragmentShader = LoadShader ( GL_FRAGMENT_SHADER, fShaderStr );

   // Create the program object
   programObject = glCreateProgram ( );

   if ( programObject == 0 )
      return 0;

   glAttachShader ( programObject, vertexShader );
   glAttachShader ( programObject, fragmentShader );

   // Link the program
   glLinkProgram ( programObject );

   // Check the link status
   glGetProgramiv ( programObject, GL_LINK_STATUS, &linked );

   if ( !linked )
   {
      GLint infoLen = 0;

      glGetProgramiv ( programObject, GL_INFO_LOG_LENGTH, &infoLen );

      if ( infoLen > 1 )
      {
        char* infoLog = malloc (sizeof(char) * infoLen );
        glGetProgramInfoLog ( programObject, infoLen, NULL, infoLog );
        printf ( "Error linking program:\n%s\n", infoLog );
        free ( infoLog );
         return;
      }

      glDeleteProgram ( programObject );
      return GL_FALSE;
   }


   // Get the attribute locations
   positionLoc = glGetAttribLocation ( programObject, "a_position" );
   texCoordLoc = glGetAttribLocation ( programObject, "a_texCoord" );

   // Get the sampler location
   samplerLoc = glGetUniformLocation ( programObject, "s_texture" );

   glUseProgram(programObject);


   return GL_TRUE;
}

static void YuidrawSoftwareBuffer() {

    int buf_width, buf_height;
    int error;

    glClearColor( 0.0f,0.0f,0.0f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if( g_FrameBuffer == 0 )
    {
       glGenTextures(1,&g_FrameBuffer);
       glActiveTexture ( GL_TEXTURE0 );
       glBindTexture(GL_TEXTURE_2D, g_FrameBuffer);
       //glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
       glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
       glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
       glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
       glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
       error = glGetError();
       if( error != GL_NO_ERROR )
       {
          printf("g_FrameBuffer gl error %04X\n", error );
          return;
       }
    }else{
       glBindTexture(GL_TEXTURE_2D, g_FrameBuffer);
    }

    VIDCore->GetGlSize(&buf_width, &buf_height);
    if ((buf_width == 0) || (buf_height == 0)) return;
    //glTexSubImage2D(GL_TEXTURE_2D, 0,0,0,512,256,GL_RGBA,GL_UNSIGNED_SHORT_1_5_5_5_REV_EXT,dispbuffer);
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, buf_width, buf_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, dispbuffer);
    if( buf_width != g_buf_width ||  buf_height != g_buf_width )
      {
          int i;
	  memcpy(swVertices, squareVertices, sizeof(swVertices));
          //swVertices[6]=swVertices[10]=(float)buf_width/1024.0f;
          //swVertices[11]=swVertices[15]=(float)buf_height/1024.0f;
          g_buf_width  = buf_width;
          g_buf_width = buf_height;
          //glViewport(0, 0,buf_width, buf_height);
       }

    if( g_VertexBuffer == 0 )
   {
      glGenBuffers(1, &g_VertexBuffer);
   }
   glBindBuffer(GL_ARRAY_BUFFER, g_VertexBuffer);
   glBufferData(GL_ARRAY_BUFFER, sizeof(squareVertices),swVertices,GL_STATIC_DRAW);
   glVertexAttribPointer ( positionLoc, 2, GL_FLOAT,  GL_FALSE, 4 * sizeof(GLfloat), 0 );
   glVertexAttribPointer ( texCoordLoc, 2, GL_FLOAT,  GL_FALSE, 4 * sizeof(GLfloat), (void*)(sizeof(float)*2) );
   glEnableVertexAttribArray ( positionLoc );
   glEnableVertexAttribArray ( texCoordLoc );
   glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}


void YuiSwapBuffers(void) {

   if( yinit.vidcoretype == VIDCORE_SOFT ){
       YuidrawSoftwareBuffer();
   }
   platform_swapBuffers();
   SetOSDToggle(1);

  if (frameskip == 1)
    usleep(time_left());

  nextFrameTime += delayUs;
}

void YuiInit() {
	yinit.m68kcoretype = M68KCORE_MUSASHI;
	yinit.percoretype = PERCORE_LINUXJOY;
	yinit.sh2coretype = 0;
#ifdef FORCE_CORE_SOFT
  yinit.vidcoretype = VIDCORE_SOFT;
#else
	yinit.vidcoretype = VIDCORE_OGL; //VIDCORE_SOFT  
#endif
#ifdef HAVE_LIBSDL
	yinit.sndcoretype = SNDCORE_SDL;
#else
	yinit.sndcoretype = 0;
#endif
	yinit.cdcoretype = CDCORE_DEFAULT;
	yinit.carttype = CART_DRAM32MBIT;
	yinit.regionid = REGION_EUROPE;
	yinit.biospath = NULL;
	yinit.cdpath = NULL;
	yinit.buppath = NULL;
	yinit.mpegpath = NULL;
	yinit.cartpath = "./backup32Mb.ram";
  yinit.videoformattype = VIDEOFORMATTYPE_NTSC;
	yinit.osdcoretype = OSDCORE_DEFAULT;
	yinit.skip_load = 0;

	yinit.usethreads = 1;
	yinit.numthreads = 4;
}

static int SetupOpenGL() {
  int w = (lowres_mode == 0)?WINDOW_WIDTH:WINDOW_WIDTH_LOW;
  int h = (lowres_mode == 0)?WINDOW_HEIGHT:WINDOW_HEIGHT_LOW;
#if defined(_USEGLEW_)
  glewExperimental=GL_TRUE;
  glewInit();
#endif
  if (!platform_SetupOpenGL(w,h)) {
    printf("Error during openGL setup\n");
    exit(EXIT_FAILURE);
  }
}

void displayGameInfo(char *filename) {
  GameInfo info;
  if (! GameInfoFromPath(filename, &info))
  {
    return;
  }

  printf("Game Info:\n\tSystem: %s\n\tCompany: %s\n\tItemNum:%s\n\tVersion:%s\n\tDate:%s\n\tCDInfo:%s\n\tRegion:%s\n\tPeripheral:%s\n\tGamename:%s\n", info.system, info.company, info.itemnum, info.version, info.date, info.cdinfo, info.region, info.peripheral, info.gamename);
}

int main(int argc, char *argv[]) {
	int i;

	LogStart();
	LogChangeOutput( DEBUG_STDERR, NULL );

	YuiInit();

//handle command line arguments
  for (i = 1; i < argc; ++i) {
    if (argv[i]) {
      //show usage
      if (0 == strcmp(argv[i], "-h") || 0 == strcmp(argv[i], "-?") || 0 == strcmp(argv[i], "--help")) {
        print_usage(argv[0]);
        return 0;
      }
			
      //set bios
      if (0 == strcmp(argv[i], "-b") && argv[i + 1]) {
        strncpy(biospath, argv[i + 1], 256);
        yinit.biospath = biospath;
      } else if (strstr(argv[i], "--bios=")) {
        strncpy(biospath, argv[i] + strlen("--bios="), 256);
        yinit.biospath = biospath;
      }
      //set iso
      else if (0 == strcmp(argv[i], "-i") && argv[i + 1]) {
        strncpy(cdpath, argv[i + 1], 256);
        yinit.cdcoretype = 1;
        yinit.cdpath = cdpath;
        displayGameInfo(cdpath);
      } else if (strstr(argv[i], "--iso=")) {
        strncpy(cdpath, argv[i] + strlen("--iso="), 256);
        yinit.cdcoretype = 1;
        yinit.cdpath = cdpath;
      }
      //set cdrom
      else if (0 == strcmp(argv[i], "-c") && argv[i + 1]) {
        strncpy(cdpath, argv[i + 1], 256);
        yinit.cdcoretype = 2;
        yinit.cdpath = cdpath;
      } else if (strstr(argv[i], "--cdrom=")) {
        strncpy(cdpath, argv[i] + strlen("--cdrom="), 256);
        yinit.cdcoretype = 2;
        yinit.cdpath = cdpath;
      }
      // Set sound
      else if (strcmp(argv[i], "-ns") == 0 || strcmp(argv[i], "--nosound") == 0) {
        yinit.sndcoretype = 0;
      }
      // Set sound
      else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
        fullscreen = 1;
      }
      // Low resolution mode
      else if (strcmp(argv[i], "-lr") == 0 || strcmp(argv[i], "--lowres") == 0) {
        lowres_mode = 1;
      }
      else if (strcmp(argv[i], "-sc") == 0 || strcmp(argv[i], "--softcore") == 0) {
        yinit.vidcoretype = VIDCORE_SOFT;
      }
      // Auto frame skip
      else if (strstr(argv[i], "--vsyncoff")) {
        frameskip = 0;
      }
      else if (strcmp(argv[i], "-dr") == 0) {
	#ifdef SH2_DYNAREC
	yinit.sh2coretype = 2;
	#endif
      }
      // Binary
      else if (strstr(argv[i], "--binary=")) {
        char binname[1024];
        unsigned int binaddress;
        int bincount;

        bincount = sscanf(argv[i] + strlen("--binary="), "%[^:]:%x", binname, &binaddress);
        if (bincount > 0) {
          if (bincount < 2) binaddress = 0x06004000;
          MappedMemoryLoadExec(binname, binaddress);
        }
      }
    }
  }
  SetupOpenGL();

  YabauseDeInit();

  if (YabauseInit(&yinit) != 0) printf("YabauseInit error \n\r");

  if (yinit.vidcoretype == VIDCORE_SOFT) {
    if( YuiInitProgramForSoftwareRendering() != GL_TRUE ){
      printf("Fail to YuiInitProgramForSoftwareRendering\n");
      return -1;
    }
  }

  if (yinit.vidcoretype == VIDCORE_OGL) {
    if (lowres_mode == 0){
      VIDCore->SetSettingValue(VDP_SETTING_RESOLUTION_MODE, RES_NATIVE);
      VIDCore->SetSettingValue(VDP_SETTING_FILTERMODE, AA_FXAA);
      VIDCore->Resize(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 1);
    } else {
      VIDCore->SetSettingValue(VDP_SETTING_RESOLUTION_MODE, RES_ORIGINAL);
      VIDCore->SetSettingValue(VDP_SETTING_FILTERMODE, AA_NONE);
      VIDCore->Resize(0, 0, WINDOW_WIDTH_LOW, WINDOW_HEIGHT_LOW, 1);
    }
  }

  nextFrameTime = getCurrentTimeUs(0) + delayUs;

  while (!platform_shouldClose())
  {
        if (PERCore->HandleEvents() == -1) platform_Close();
        platform_HandleEvent();
  }

	YabauseDeInit();
	LogStop();
  platform_Deinit();

	return 0;
}
