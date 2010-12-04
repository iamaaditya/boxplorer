#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <assert.h>

#define NO_SDL_GLEXT
#include <SDL/SDL_opengl.h>
#include "shader_procs.h"
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#include "default_shaders.h"

#define DEFAULT_CONFIG_FILE  "boxplorer.cfg"
#define VERTEX_SHADER_FILE   "vertex.glsl"
#define FRAGMENT_SHADER_FILE "fragment.glsl"

#ifdef PI
  #undef PI
#endif
#define PI          3.14159265358979324
#define die(...)    ( fprintf(stderr, __VA_ARGS__), exit(-1), 1 )
#define lengthof(x) ( sizeof(x)/sizeof((x)[0]) )
#define sign(x)     ( (x)<0 ? -1 : 1 )

#define FPS_FRAMES_TO_AVERAGE 6

////////////////////////////////////////////////////////////////
// Helper functions

// Compute the dot product of two vectors.
float dot(float x[3], float y[3]) {
  return x[0]*y[0] + x[1]*y[1] + x[2]*y[2];
}

// Normalize a vector. If it was zero, return 0.
int normalize(float x[3]) {
  float len = dot(x, x); if (len == 0) return 0;
  len = 1/sqrt(len); x[0] *= len; x[1] *= len; x[2] *= len;
  return 1;
}

// Allocate a char[] and read a text file into it. Return 0 on error.
char* readFile(char const* name) {
  FILE* f;
  int len;
  char* s = 0;

  // open file an get its length
  if (!(f = fopen(name, "r"))) goto readFileError1;
  fseek(f, 0, SEEK_END);
  len = ftell(f);

  // read the file in an allocated buffer
  if (!(s = malloc(len+1))) goto readFileError2;
  rewind(f);
  len = fread(s, 1, len, f);
  s[len] = '\0';

  readFileError2: fclose(f);
  readFileError1: return s;
}

////////////////////////////////////////////////////////////////
// FPS tracking.

int framesToAverage;
Uint32* frameDurations; int frameDurationsIndex = 0;
Uint32 lastFrameTime;

// Initialize the FPS structure.
void initFPS(int framesToAverage_) {
  assert(framesToAverage_ > 1);
  framesToAverage = framesToAverage_;
  frameDurations = malloc(sizeof(Uint32) * framesToAverage_);
  frameDurations[0] = 0;
  lastFrameTime = SDL_GetTicks();
}

// Update the FPS structure after drawing a frame.
void updateFPS(void) {
  Uint32 time = SDL_GetTicks();
  frameDurations[frameDurationsIndex++ % framesToAverage] = time - lastFrameTime;
  lastFrameTime = time;
}

// Return the duration of the last frame.
Uint32 getLastFrameDuration(void) {
  return frameDurations[(frameDurationsIndex+framesToAverage-1) % framesToAverage];
}

// Return the average FPS over the last X frames.
float getFPS(void) {
  if (frameDurationsIndex < framesToAverage) return 0;  // not enough data

  int i; Uint32 sum;
  for (i=sum=0; i<framesToAverage; i++) sum += frameDurations[i];
  return framesToAverage * 1000. / sum;
}


////////////////////////////////////////////////////////////////
// Current logical state of the program.

// Camera.

float camera[16] = {
  0,0,0, 0,
  0,1,0, 0,
  0,0,1, 0,
  -1.85,1.85,-2.2, 1
};

#define rightDirection (&camera[0])
#define upDirection (&camera[4])
#define direction (&camera[8])
#define position (&camera[12])

// Set the OpenGL modelview matrix to the camera matrix.
void setCamera(void) {
  glLoadMatrixf(camera);
}

// Orthogonalize the camera matrix.
void orthogonalizeCamera(void) {
  int i; float l;

  if (!normalize(direction)) { direction[0]=direction[1]=0; direction[2]=1; }

  // Orthogonalize and normalize upDirection.
  l = dot(direction, upDirection);
  for (i=0; i<3; i++) upDirection[i] -= l*direction[i];
  if (!normalize(upDirection)) {  // Error? Make upDirection.z = 0.
    upDirection[2] = 0;
    if (fabs(direction[2]) == 1) { upDirection[0] = 0; upDirection[1] = 1; }
    else {
      upDirection[0] = -direction[1]; upDirection[1] = direction[0];
      normalize(upDirection);
    }
  }

  // Compute rightDirection as a cross product of upDirection and direction.
  for (i=0; i<3; i++) {
    int j = (i+1)%3, k = (i+2)%3;
    rightDirection[i] = upDirection[j]*direction[k] - upDirection[k]*direction[j];
  }
}

// Move camera in a direction relative to the view direction.
// Behaves like `glTranslate`.
void moveCamera(float x, float y, float z) {
  int i; for (i=0; i<3; i++) {
    position[i] += rightDirection[i]*x + upDirection[i]*y + direction[i]*z;
  }
}

// Move camera in the normalized absolute direction `dir` by `len` units.
void moveCameraAbsolute(float* dir, float len) {
  int i; for (i=0; i<3; i++) {
    position[i] += len * dir[i];
  }
}

// Rotate the camera by `deg` degrees around a normalized axis.
// Behaves like `glRotate` without normalizing the axis.
void rotateCamera(float deg, float x, float y, float z) {
  int i, j;
  float s = sin(deg*PI/180), c = cos(deg*PI/180), t = 1-c;
  float r[3][3] = {
    { x*x*t +   c, x*y*t + z*s, x*z*t - y*s },
    { y*x*t - z*s, y*y*t +   c, y*z*t + x*s },
    { z*x*t + y*s, z*y*t - x*s, z*z*t +   c }
  };
  for (i=0; i<3; i++) {
    float c[3];
    for (j=0; j<3; j++) c[j] = camera[i+j*4];
    for (j=0; j<3; j++) camera[i+j*4] = dot(c, r[j]);
  }
}


// User parameters and their names (default: par0x, par0y, par1x, ...).
// par0 is specialized for the Mandelbox
float par[10][2] = { {0.25, -1.77} };
char* parName[10][2];


// Simple configuration parameters.

#define PROCESS_CONFIG_PARAMS \
  PROCESS(int, width, "width") \
  PROCESS(int, height, "height") \
  PROCESS(int, fullscreen, "fullscreen") \
  PROCESS(int, multisamples, "multisamples") \
  PROCESS(float, fov_x, "fov_x") \
  PROCESS(float, fov_y, "fov_y") \
  PROCESS(float, speed, "speed") \
  PROCESS(float, keyb_rot_speed, "keyb_rot_speed") \
  PROCESS(float, mouse_rot_speed, "mouse_rot_speed") \
  PROCESS(float, min_dist, "min_dist") \
  PROCESS(int, max_steps, "max_steps") \
  PROCESS(int, iters, "iters") \
  PROCESS(int, color_iters, "color_iters") \
  PROCESS(float, ao_eps, "ao_eps") \
  PROCESS(float, ao_strength, "ao_strength") \
  PROCESS(float, glow_strength, "glow_strength") \
  PROCESS(float, dist_to_color, "dist_to_color")

// Non-simple: position[3], direction[3], upDirection[3], par[10][2]

// Define simple config parameters.

#define PROCESS(type, name, nameString) type name = 0;
PROCESS_CONFIG_PARAMS
#undef PROCESS



// Make sure parameters are OK.
void sanitizeParameters(void) {
  // Resolution: if only one coordinate is set, keep 4:3 aspect ratio.
  if (width < 1) {
    if (height < 1) { height = 480; }
    width = height*4/3;
  }
  if (height < 1) height = width*3/4;

  // FOV: keep pixels square unless stated otherwise.
  // Default FOV_y is 75 degrees.
  if (fov_x <= 0) {
    if (fov_y <= 0) { fov_y = 75; }
    fov_x = atan(tan(fov_y*PI/180/2)*width/height)/PI*180*2;
  }
  if (fov_y <= 0) fov_y = atan(tan(fov_x*PI/180/2)*height/width)/PI*180*2;

  // Fullscreen: 0=off, anything else=on.
  if (fullscreen != 0 && fullscreen != 1) fullscreen = 1;

  // The others are easy.
  if (multisamples < 1) multisamples = 1;
  if (speed <= 0) speed = 0.005;  // units/frame
  if (keyb_rot_speed <= 0) keyb_rot_speed = 5;  // degrees/frame
  if (mouse_rot_speed <= 0) mouse_rot_speed = 1;  // degrees/pixel
  if (max_steps < 1) max_steps = 128;
  if (min_dist <= 0) min_dist = 0.0001;
  if (iters < 1) iters = 13;
  if (color_iters < 1) color_iters = 9;
  if (ao_eps <= 0) ao_eps = 0.0005;
  if (ao_strength <= 0) ao_strength = 0.1;
  if (glow_strength <= 0) glow_strength = 0.5;
  if (dist_to_color <= 0) dist_to_color = 0.2;

  orthogonalizeCamera();

  // Don't do anything with user parameters - they must be
  // sanitized (clamped, ...) in the shader.
}


// Load configuration.
void loadConfig(char const* configFile) {
  FILE* f;
  if ((f = fopen(configFile, "r")) != 0) {
    int i;
    char s[32768];  // max line length
    while (fscanf(f, " %s", s) == 1) {  // read word
      double val;

      #define PROCESS(type, name, nameString) \
        if (!strcmp(s, nameString)) { fscanf(f, " %lf", &val); name = val; continue; }
      PROCESS_CONFIG_PARAMS
      #undef PROCESS

      if (!strcmp(s, "position")) { fscanf(f, " %f %f %f", &position[0], &position[1], &position[2]); continue; }
      if (!strcmp(s, "direction")) { fscanf(f, " %f %f %f", &direction[0], &direction[1], &direction[2]); continue; }
      if (!strcmp(s, "upDirection")) { fscanf(f, " %f %f %f", &upDirection[0], &upDirection[1], &upDirection[2]); continue; }
      for (i=0; i<lengthof(par); i++) {
        char p[256];
        sprintf(p, "par%d", i); if (!strcmp(s, p)) { fscanf(f, " %f %f", &par[i][0], &par[i][1]); break; }
        sprintf(p, "par%dx_name", i); if (!strcmp(s, p)) {
          fscanf(f, " %s", p);
          if (parName[i][0]) free(parName[i][0]);
          parName[i][0] = malloc(strlen(p)+1); strcpy(parName[i][0], p);
          break;
        }
        sprintf(p, "par%dy_name", i); if (!strcmp(s, p)) {
          fscanf(f, " %s", p);
          if (parName[i][1]) free(parName[i][1]);
          parName[i][1] = malloc(strlen(p)+1); strcpy(parName[i][1], p);
          break;
        }
      }
    }
  }
}


// Save configuration.
void saveConfig(char const* configFile) {
  int i;
  FILE* f;
  if ((f = fopen(configFile, "w")) != 0) {

    #define PROCESS(type, name, nameString) \
      fprintf(f, nameString " %g\n", (double)name);
    PROCESS_CONFIG_PARAMS
    #undef PROCESS

    fprintf(f, "position %.7g %.7g %.7g\n", position[0], position[1], position[2]);
    fprintf(f, "direction %.7g %.7g %.7g\n", direction[0], direction[1], direction[2]);
    fprintf(f, "upDirection %.7g %.7g %.7g\n", upDirection[0], upDirection[1], upDirection[2]);
    for (i=0; i<lengthof(par); i++) {
      fprintf(f, "par%d %g %g\n", i, par[i][0], par[i][1]);
      if (parName[i][0]) fprintf(f, "par%dx_name %s\n", i, parName[i][0]);
      if (parName[i][1]) fprintf(f, "par%dy_name %s\n", i, parName[i][1]);
    }
    fclose(f);
  }
}


////////////////////////////////////////////////////////////////
// Controllers.

typedef enum Controller {
  // user parameters: 0..9
  // other parameters:
  CTL_FOV = lengthof(par), CTL_RAY, CTL_ITER, CTL_AO, CTL_GLOW, CTL_CAM,
  CTL_LAST = CTL_CAM,
} Controller;


// Controller modifiers.
// They get a pointer to the modified vaule and a signed count of consecutive changes.
void m_mul(float* x, int d) { *x *= pow(10, sign(d)/20.); }
void m_mulSlow(float* x, int d) { *x *= pow(10, sign(d)/40.); }
void m_tan(float* x, int d) { *x = atan(tan(*x*PI/180/2) * pow(0.1, sign(d)/40.) ) /PI*180*2; }
void m_progressiveInc(int* x, int d) { *x += sign(d) * ((abs(d)+4) / 4); }
void m_progressiveAdd(float* x, int d) { *x += 0.001 * (sign(d) * ((abs(d)+4) / 4)); }
void m_singlePress(int* x, int d) { if (d==1 || d==-1) *x += d; }
void m_rotateX(int d) { rotateCamera(sign(d)*keyb_rot_speed, 0, 1, 0); }
void m_rotateY(int d) { rotateCamera(-sign(d)*keyb_rot_speed, 1, 0, 0); }


// Print controller values into a string.
char* printController(char* s, Controller c) {
  assert(c <= CTL_LAST);
  switch (c) {
    default: {
      char x[8],y[8]; sprintf(x, "par%dx", c); sprintf(y, "par%dy", c);
      sprintf(s, "%s %.3f %s %.3f",
        parName[c][1] ? parName[c][1] : y, par[c][1],
        parName[c][0] ? parName[c][0] : x, par[c][0]
      );
    } break;
    case CTL_FOV: sprintf(s, "Fov %.3g %.3g", fov_x, fov_y); break;
    case CTL_RAY: sprintf(s, "Ray %.2e steps %d", min_dist, max_steps); break;
    case CTL_ITER: sprintf(s, "It %d|%d", iters, color_iters); break;
    case CTL_AO: sprintf(s, "aO %.2e d %.2e", ao_strength, ao_eps); break;
    case CTL_GLOW: sprintf(s, "Glow %.3f bgd %.2e", glow_strength, dist_to_color); break;
    case CTL_CAM: {
      sprintf(s, "Look [%d %d %d]", (int)round(direction[0]*100), (int)round(direction[1]*100), (int)round(direction[2]*100));
    } break;
  }
  return s;
}


// Update controller.y by the signed count of consecutive changes.
void updateControllerY(Controller c, int d) {
  assert(c <= CTL_LAST);
  switch (c) {
    default: m_progressiveAdd(&par[c][1], d); break;
    case CTL_FOV: m_tan(&fov_x, d); m_tan(&fov_y, d); break;
    case CTL_RAY: m_mul(&min_dist, d); break;
    case CTL_ITER: m_singlePress(&iters, d); if ((iters&1)==(d>0)) m_singlePress(&color_iters, d); break;
    case CTL_AO: m_mulSlow(&ao_strength, d); break;
    case CTL_GLOW: m_progressiveAdd(&glow_strength, d); break;
    case CTL_CAM: m_rotateY(d); break;
  }
}


// Update controller.x by the signed count of consecutive changes.
void updateControllerX(Controller c, int d) {
  assert(c <= CTL_LAST);
  switch (c) {
    default: m_progressiveAdd(&par[c][0], d); break;
    case CTL_FOV: m_tan(&fov_x, d); break;
    case CTL_RAY: m_progressiveInc(&max_steps, d); break;
    case CTL_ITER: m_singlePress(&color_iters, d); break;
    case CTL_AO: m_mul(&ao_eps, d); break;
    case CTL_GLOW: m_mul(&dist_to_color, d); break;
    case CTL_CAM: m_rotateX(d); break;
  }
}


// Change the active controller with a keypress.
void changeController(SDLKey key, Controller* c) {
  switch (key) {
    case SDLK_0: case SDLK_KP0: *c = 0; break;
    case SDLK_1: case SDLK_KP1: *c = 1; break;
    case SDLK_2: case SDLK_KP2: *c = 2; break;
    case SDLK_3: case SDLK_KP3: *c = 3; break;
    case SDLK_4: case SDLK_KP4: *c = 4; break;
    case SDLK_5: case SDLK_KP5: *c = 5; break;
    case SDLK_6: case SDLK_KP6: *c = 6; break;
    case SDLK_7: case SDLK_KP7: *c = 7; break;
    case SDLK_8: case SDLK_KP8: *c = 8; break;
    case SDLK_9: case SDLK_KP9: *c = 9; break;
    case SDLK_f: *c = CTL_FOV; break;
    case SDLK_g: *c = CTL_GLOW; break;
    case SDLK_l: *c = CTL_CAM; break;
    case SDLK_i: *c = CTL_ITER; break;
    case SDLK_o: *c = CTL_AO; break;
    case SDLK_r: *c = CTL_RAY; break;
    default: break;  // no change
  }
}


////////////////////////////////////////////////////////////////
// Graphics.

// Position of the OpenGL window on the screen.
int viewportOffset[2];

// Is the mouse and keyboard input grabbed?
int grabbedInput = 1;

void saveScreenshot(char const* tgaFile) {
  FILE *f;

  if ((f = fopen(tgaFile, "wb")) != 0) {
    unsigned char header[18] = {
      0,0,2,0,0,0,0,0,0,0,0,0,width%256,width/256,height%256,height/256,24,0
    };
    unsigned char* img = malloc(width * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_FRONT);
    glReadPixels(viewportOffset[0], viewportOffset[1], width, height, GL_BGR, GL_UNSIGNED_BYTE, img);

    fwrite(header, 18, 1, f);
    fwrite(img, 3, width*height, f);

    free(img);
    fclose(f);
  }
}

// The shader program handle.
int program;

// Compile and activate shader programs. Return the program handle.
int setupShaders(void) {
  char const* vs;
  char const* fs;
  GLuint v,f,p;
  char log[2048]; int logLength;

  (vs = readFile(VERTEX_SHADER_FILE)) || ( vs = default_vs );
  (fs = readFile(FRAGMENT_SHADER_FILE)) || ( fs = default_fs );

  p = glCreateProgram();

  v = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(v, 1, &vs, 0);
  glCompileShader(v);
  glGetShaderInfoLog(v, sizeof(log), &logLength, log);
  if (logLength) fprintf(stderr, "[Vertex:]\n%s\n", log);

  f = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(f, 1, &fs, 0);
  glCompileShader(f);
  glGetShaderInfoLog(f, sizeof(log), &logLength, log);
  if (logLength) fprintf(stderr, "[Fragment:]\n%s\n", log);

  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);

  glGetProgramInfoLog(p, sizeof(log), &logLength, log);
  if (logLength) fprintf(stderr, "[Program:]\n%s\n", log);

  if (vs != default_vs) free((char*)vs);
  if (fs != default_fs) free((char*)fs);

  glUseProgram(p);
  return p;
}


// Update shader parameters to their current values.
#define glSetUniformf(name) \
  glUniform1f(glGetUniformLocation(program, #name), name);
#define glSetUniformv(name) \
  glUniform2fv(glGetUniformLocation(program, #name), lengthof(name), (float*)name);
#define glSetUniformi(name) \
  glUniform1i(glGetUniformLocation(program, #name), name);

void setUniforms(void) {
  glSetUniformv(par);
  glSetUniformf(fov_x); glSetUniformf(fov_y);
  glSetUniformi(max_steps); glSetUniformf(min_dist);
  glSetUniformi(iters); glSetUniformi(color_iters);
  glSetUniformf(ao_eps); glSetUniformf(ao_strength);
  glSetUniformf(glow_strength); glSetUniformf(dist_to_color);
}


// Initializes the video mode, OpenGL state, shaders, camera and shader parameters.
// Exits the program if an error occurs.
void initGraphics(void) {
  // If not fullscreen, use the color depth of the current video mode.
  int bpp = 24;  // FSAA works reliably only in 24bit modes
  if (!fullscreen) {
    SDL_VideoInfo const* info = SDL_GetVideoInfo();
    bpp = info->vfmt->BitsPerPixel;
  }

  // Set attributes for the OpenGL window.
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  if (multisamples == 1) {
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
  }
  else {
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, multisamples);
  }

  // Set the video mode, hide the mouse and grab keyboard and mouse input.
  SDL_putenv("SDL_VIDEO_CENTERED=center");
  SDL_Surface* screen;
  (screen = SDL_SetVideoMode(width, height, bpp, SDL_OPENGL | (fullscreen ? SDL_FULLSCREEN : 0)))
    || die("Video mode initialization failed: %s\n", SDL_GetError());

  if (multisamples > 1) glEnable(GL_MULTISAMPLE);  // redundant?

  if (grabbedInput) { SDL_ShowCursor(SDL_DISABLE); SDL_WM_GrabInput(SDL_GRAB_ON); }  // order is important
  else { SDL_ShowCursor(SDL_ENABLE); SDL_WM_GrabInput(SDL_GRAB_OFF); }

  // TODO: logging.
  //int samples = 0; glGetIntegerv(GL_SAMPLES, &samples); printf("%dx%d, %d bpp, FSAA %d\n", screen->w, screen->h, screen->format->BitsPerPixel, samples);

  // If we got higher resolution (which can happen in fullscreen mode),
  // use a centered viewport.
  viewportOffset[0] = (screen->w - width)/2;
  viewportOffset[1] = (screen->h - height)/2;
  glViewport(viewportOffset[0], viewportOffset[1], width, height);

  // Enable shader functions and compile shaders.
  // Needs to be done after setting the video mode.
  enableShaderProcs() || die("This program needs support for GLSL shaders.\n");
  (program = setupShaders()) || die("Error in GLSL shader compilation (see stderr.txt for details).\n");
}


////////////////////////////////////////////////////////////////
// Setup, input handling and drawing.

int main(int argc, char **argv) {
  // Load configuration.
  loadConfig(argc>=2 ? argv[1] : DEFAULT_CONFIG_FILE);
  sanitizeParameters();

  // Initialize SDL and OpenGL graphics.
  SDL_Init(SDL_INIT_VIDEO) == 0 || die("SDL initialization failed: %s\n", SDL_GetError());
  atexit(SDL_Quit);

  // Set up the video mode, OpenGL state, shaders and shader parameters.
  initGraphics();
  initFPS(FPS_FRAMES_TO_AVERAGE);

  // Locked movement direction for mouse button movement.
  int dirLocked = 0; float lockedDir[3];

  // Main loop.
  Controller ctl = CTL_CAM;  // the default controller is camera rotation
  int consecutiveChanges = 0;
  int done = 0;

  while (!done) {
    int ctlXChanged = 0, ctlYChanged = 0;

    // Raytrace a frame and tell it to the FPS structure.
    setCamera();
    setUniforms();

    glRects(-1,-1,1,1);

    SDL_GL_SwapBuffers();
    updateFPS();

    // Show position and fps in the caption.
    char caption[2048], controllerStr[256];

    sprintf(caption, "%s %.2ffps [%.3f %.3f %.3f] %dms",
      printController(controllerStr, ctl),
      getFPS(), position[0], position[1], position[2], getLastFrameDuration()
    );
    SDL_WM_SetCaption(caption, 0);

    // Process events.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_QUIT: done |= 1; break;

        case SDL_MOUSEBUTTONDOWN: {
          if (!grabbedInput) { grabbedInput = 1; SDL_ShowCursor(SDL_DISABLE); SDL_WM_GrabInput(SDL_GRAB_ON); }
        } break;

        case SDL_KEYDOWN: switch (event.key.keysym.sym) {
          case SDLK_ESCAPE: {
            if (grabbedInput && !fullscreen) { grabbedInput = 0; SDL_ShowCursor(SDL_ENABLE); SDL_WM_GrabInput(SDL_GRAB_OFF); }
            else done |= 1;
          } break;

          // Switch fullscreen mode (loses the whole OpenGL context in Windows).
          case SDLK_RETURN: case SDLK_KP_ENTER: {
            fullscreen ^= 1; grabbedInput = 1; initGraphics();
          } break;

          // Save config and screenshot (filename = current time).
          case SDLK_SPACE: {
            time_t t = time(0);
            struct tm* ptm = localtime(&t);
            char filename[256];
            strftime(filename, 256, "%Y%m%d_%H%M%S.cfg", ptm); saveConfig(filename);
            strftime(filename, 256, "%Y%m%d_%H%M%S.tga", ptm); saveScreenshot(filename);
          } break;

          // Change movement speed.
          case SDLK_LSHIFT: case SDLK_RSHIFT: speed *= 2; break;
          case SDLK_LCTRL:  case SDLK_RCTRL:  speed /= 2; break;

          // Resolve controller value changes that happened during rendering.
          case SDLK_LEFT:  ctlXChanged = 1; updateControllerX(ctl, -(consecutiveChanges=1)); break;
          case SDLK_RIGHT: ctlXChanged = 1; updateControllerX(ctl,  (consecutiveChanges=1)); break;
          case SDLK_DOWN:  ctlYChanged = 1; updateControllerY(ctl, -(consecutiveChanges=1)); break;
          case SDLK_UP:    ctlYChanged = 1; updateControllerY(ctl,  (consecutiveChanges=1)); break;

          // Otherwise see whether the active controller has changed.
          default: {
            Controller oldCtl = ctl;
            changeController(event.key.keysym.sym, &ctl);
            if (ctl != oldCtl) { consecutiveChanges = 0; }
          } break;
        }
        break;
      }
    }

    // Get keyboard and mouse state.
    Uint8* keystate = SDL_GetKeyState(0);
    int mouse_dx, mouse_dy;
    Uint8 mouse_buttons = SDL_GetRelativeMouseState(&mouse_dx, &mouse_dy);
    int mouse_button_left = mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT);
    int mouse_button_right = mouse_buttons & SDL_BUTTON(SDL_BUTTON_RIGHT);

    // Translate the camera.
    if (mouse_button_left || mouse_button_right) {
      if (!dirLocked) {
        int i;
        for (i=0; i<3; i++) lockedDir[i] = direction[i];
      }
      dirLocked = 1;
    }
    else dirLocked = 0;

    if (mouse_button_left) moveCameraAbsolute(lockedDir, speed);
    if (mouse_button_right) moveCameraAbsolute(lockedDir, -speed);
    if (mouse_button_left && mouse_button_right) moveCameraAbsolute(lockedDir, speed/4);

    if (keystate[SDLK_LALT]) moveCamera(0, 0, speed);

    if (keystate[SDLK_w]) moveCamera(0, speed, 0);
    if (keystate[SDLK_s]) moveCamera(0, -speed, 0);
    if (keystate[SDLK_w] && keystate[SDLK_s]) moveCamera(0, 0, speed);

    if (keystate[SDLK_a]) moveCamera(-speed, 0, 0);
    if (keystate[SDLK_d]) moveCamera( speed, 0, 0);
    if (keystate[SDLK_a] && keystate[SDLK_d]) moveCamera(0, 0, -speed);

    if ((keystate[SDLK_LALT] || (keystate[SDLK_w] && keystate[SDLK_s])) && (keystate[SDLK_a] && keystate[SDLK_d])) moveCamera(0, 0, speed/4);

    // Rotate the camera.
    if (grabbedInput && (mouse_dx != 0 || mouse_dy != 0)) {
      float len = sqrt(mouse_dx*mouse_dx + mouse_dy*mouse_dy);
      rotateCamera(mouse_rot_speed * len, mouse_dy/len, mouse_dx/len, 0);
    }
    if (keystate[SDLK_q]) rotateCamera( keyb_rot_speed, 0, 0, 1);
    if (keystate[SDLK_e]) rotateCamera(-keyb_rot_speed, 0, 0, 1);

    // Change the value of the active controller.
    if (!ctlXChanged) {
      if (keystate[SDLK_LEFT])  { ctlXChanged = 1; updateControllerX(ctl, -++consecutiveChanges); }
      if (keystate[SDLK_RIGHT]) { ctlXChanged = 1; updateControllerX(ctl,  ++consecutiveChanges); }
    }
    if (!ctlYChanged) {
      if (keystate[SDLK_DOWN])  { ctlYChanged = 1; updateControllerY(ctl, -++consecutiveChanges); }
      if (keystate[SDLK_UP])    { ctlYChanged = 1; updateControllerY(ctl,  ++consecutiveChanges); }
    }

    if (!(ctlXChanged || ctlYChanged)) consecutiveChanges = 0;
  }

  saveConfig("last.cfg");  // Save a config file on exit, just in case.
  return 0;
}
