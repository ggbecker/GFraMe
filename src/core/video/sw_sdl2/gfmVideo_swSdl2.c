/**
 * Software backend that uses SDL2 for rendering the SW backbuffer to the
 * screen. This is mostly for fun and in cases where nothing better may be used
 * (in which case, you are probably screwed, because this will certainly be
 ( quite slow)
 * 
 * @file src/core/video/sdl2/gfmVideo_swSdl2.c
 */
#include <GFraMe/gfmAssert.h>
#include <GFraMe/gfmError.h>
#include <GFraMe/gfmGenericArray.h>
#include <GFraMe/gfmLog.h>
#include <GFraMe/gfmUtils.h>

#include <GFraMe/core/gfmFile_bkend.h>

#include <GFraMe_int/core/gfmVideo_bkend.h>
#include <GFraMe_int/gfmVideo_bmp.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>

#include <stdlib.h>
#include <string.h>

/** Define a texture array type */
gfmGenArr_define(gfmTexture);

struct stGFMTexture {
    /** Texture data, 24 bits per color in RGB order */
    unsigned char *pData;
    /** Alpha transparency mask; 0xFF repesents transparent pixels and 0
     * represents opaque ones */
    unsigned char *pMask;
    /** Width of the texture in bytes (sometime referred as pitch). Useful to
     * skipping to the next line */
    int widthInBytes;
    /** Texture's width in pixels */
    int width;
    /** Texture's height in pixels */
    int height;
};

struct stGFMVideoSwSDL2 {
    gfmLog *pLog;
    /** Actual window (managed by SDL2) */
    SDL_Window *pSDLWindow;
    /** Intermediate context used to render things to the backbuffer and, then,
      to the screen */
    SDL_Renderer *pRenderer;
    /** Buffer used to render everything */
    SDL_Texture *pSDLBackbuffer;
    /** Backbuffer data array. Pixels are stored in 24 bits, RGB format */
    unsigned char *pBackbufferData;
    /** Input texture for rendering */
    gfmTexture *pCachedTexture;
    /** Every cached texture */
    gfmGenArr_var(gfmTexture, pTextures);
/* ==== WINDOW FIELDS ======================================================= */
    /** Device's width */
    int devWidth;
    /** Device's height */
    int devHeight;
    /** Window's width (useful only in windowed mode) */
    int wndWidth;
    /** Window's height (useful only in windowed mode) */
    int wndHeight;
    /** Current resolution (useful only in fullscreen) */
    int curResolution;
    /** Whether we are currently in full-screen mode */
    int isFullscreen;
    /** How many resolutions are supported by this device */
    int resCount;
/* ==== BACKBUFFER FIELDS =================================================== */
    /** Position of the backbuffer within the screen */
    SDL_Rect outRect;
    /** Backbuffer's width */
    int bbufWidth;
    /** Backbuffer's width in bytes */
    int bbufWidthInBytes;
    /** Backbuffer's height */
    int bbufHeight;
    /** Factor by which the (output) screen is bigger than the backbuffer */
    int scrZoom;
    /** Background red component */
    Uint8 bgRed;
    /** Background green component */
    Uint8 bgGreen;
    /** Background blue component */
    Uint8 bgBlue;
    /** Background alpha component */
    Uint8 bgAlpha;
    int totalNumObjects;
    int lastNumObjects;
};
typedef struct stGFMVideoSwSDL2 gfmVideoSwSDL2;

/**
 * Set the background color
 * 
 * NOTE: This color is used only when cleaning the backbuffer. If the
 * backbuffer has to be letter-boxed into the window, it will cleaned with
 * black.
 * 
 * @param  [ in]pVideo The video context
 * @param  [ in]color  The background color (in 0xAARRGGBB format)
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, ...
 */
static gfmRV gfmVideo_SWSDL2_setBackgroundColor(gfmVideo *pVideo, int color) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Setting BG color to 0x%X", color);
    ASSERT(rv == GFMRV_OK, rv);

    /* Set the color */
    pCtx->bgAlpha = (color >> 24) & 0xff;
    pCtx->bgRed   = (color >> 16) & 0xff;
    pCtx->bgGreen = (color >> 8) & 0xff;
    pCtx->bgBlue  = color & 0xff;

    rv = GFMRV_OK;
__ret:
    return rv;
}


/**
 * Frees and cleans a previously allocated texture
 * 
 * @param  ppCtx The alocated texture
 * @return       GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_ALLOC_FAILED
 */
static void gfmVideo_SWSDL2_freeTexture(gfmTexture **ppCtx) {
    /* Check if the object was actually alloc'ed */
    if (ppCtx && *ppCtx) {
        /* Check if the texture was created and destroy it */
        if ((*ppCtx)->pData) {
            free((*ppCtx)->pData);
        }
        if ((*ppCtx)->pMask) {
            free((*ppCtx)->pMask);
        }
        memset(*ppCtx, 0x0, sizeof(gfmTexture));
        /* Free the memory */
        free(*ppCtx);

        *ppCtx = 0;
    }
}


/**
 * Initializes a new gfmVideo
 * 
 * @param  [out]ppCtx The alloc'ed gfmVideo context
 * @param  [ in]pLog  The logger facility, so it's possible to log whatever
 *                    happens in this module
 * @return            GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_ALLOC_FAILED, ...
 */
static gfmRV gfmVideo_SWSDL2_init(gfmVideo **ppCtx, gfmLog *pLog) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int didInit, irv;
    SDL_DisplayMode sdlMode;

    didInit = 0;
    pCtx = 0;

    /* Sanitize arguments */
    ASSERT(ppCtx, GFMRV_ARGUMENTS_BAD);

    /* Alloc the video context */
    pCtx = (gfmVideoSwSDL2*)malloc(sizeof(gfmVideoSwSDL2));
    ASSERT(pCtx, GFMRV_ALLOC_FAILED);

    /* Clean the struct */
    memset(pCtx, 0x0, sizeof(gfmVideoSwSDL2));

    /* Store the log facility */
    pCtx->pLog = pLog;

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Initializing SDL2 video backend");
    ASSERT(rv == GFMRV_OK, rv);

    /* Initialize the SDL2 video subsystem */
    irv = SDL_InitSubSystem(SDL_INIT_VIDEO);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    /* Mark SDL2 as initialized, in case anything happens */
    didInit = 1;

    /* Get the device's default resolution */
    irv = SDL_GetDisplayMode(0 /*displayIndex*/, 0/*defResolution*/, &sdlMode);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    pCtx->devWidth = sdlMode.w;
    pCtx->devHeight = sdlMode.h;

    gfmLog_log(pCtx->pLog, gfmLog_info, "Main display dimensions: %i x %i",
            pCtx->devWidth, pCtx->devHeight);

    /* Retrieve the number of available resolutions */
    pCtx->resCount = SDL_GetNumDisplayModes(0);
    ASSERT_LOG(pCtx->resCount, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    gfmLog_log(pCtx->pLog, gfmLog_info, "Number of available resolutions: %i",
            pCtx->resCount);

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "SDL2 video context initialized");
    ASSERT(rv == GFMRV_OK, rv);

    /* Set the return variables */
    *ppCtx = (gfmVideo*)pCtx;
    rv = GFMRV_OK;
__ret:
    if (rv != GFMRV_OK) {
        /* On error, shutdown SDL and free the alloc'ed memory */
        if (didInit) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
        if (pCtx) {
            free(pCtx);
        }
    }

    return rv;
}

/**
 * Releases a previously alloc'ed/initialized gfmVideo
 * 
 * @param  [out]ppVideo The gfmVideo context
 * @return              GFMRV_OK, GFMRV_ARGUMENTS_BAD
 */
static gfmRV gfmVideo_SWSDL2_free(gfmVideo **ppVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;

    /* Sanitize arguments */
    ASSERT(ppVideo, GFMRV_ARGUMENTS_BAD);

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)*ppVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);

    /* Clean all textures */
    gfmGenArr_clean(pCtx->pTextures, gfmVideo_SWSDL2_freeTexture);

    /* Destroy the renderer and the backbuffer */
    if (pCtx->pBackbufferData) {
        free(pCtx->pBackbufferData);
    }
    if (pCtx->pSDLBackbuffer) {
        SDL_DestroyTexture(pCtx->pSDLBackbuffer);
    }
    if (pCtx->pRenderer) {
        SDL_DestroyRenderer(pCtx->pRenderer);
    }

    /* Destroy the window */
    if (pCtx->pSDLWindow) {
        /* TODO revert screen to it's original mode? (is it required?) */
        SDL_DestroyWindow(pCtx->pSDLWindow);
    }

    /* Release the video context */
    free(pCtx);

    *ppVideo = 0;
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Count how many resolution modes there are available when in fullscreen
 * 
 * @param  [out]pCount How many resolutions were found
 * @param  [ in]pVideo The video context (will store the resolutions list)
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD
 */
static gfmRV gfmVideo_SWSDL2_countResolutions(int *pCount, gfmVideo *pVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);

    /* Retrieve the number of resolutions */
    *pCount = pCtx->resCount;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Get one of the possibles window's resolution
 * 
 * If the resolutions hasn't been queried, this function will do so
 * 
 * @param  [out]pWidth   A possible window's width
 * @param  [out]pHeight  A possible window's height
 * @param  [out]pRefRate A possible window's refresh rate
 * @param  [ in]pVideo   The video context
 * @param  [ in]index    Resolution to be read (0 is the default resolution)
 * @return               GFMRV_OK, GFMRV_ARGUMENTS_BAD,
 *                       GFMRV_INTERNAL_ERROR, GFMRV_INVALID_INDEX
 */
static gfmRV gfmVideo_SWSDL2_getResolution(int *pWidth, int *pHeight,
        int *pRefRate, gfmVideo *pVideo, int index) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int irv;
    SDL_DisplayMode sdlMode;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(pWidth, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(pHeight, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(pRefRate, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(index >= 0, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that the resolution is valid */
    ASSERT_LOG(index < pCtx->resCount, GFMRV_INVALID_INDEX, pCtx->pLog);

    /* Retrieve the dimensions for the current resolution mode */
    irv = SDL_GetDisplayMode(0 /*displayIndex*/, index, &sdlMode);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Resolution %i: %i x %i @ %iHz",
            sdlMode.w, sdlMode.h, sdlMode.refresh_rate);
    ASSERT(rv == GFMRV_OK, rv);
    switch (sdlMode.format) {
#define LOG_PF(fmt) \
    case SDL_PIXELFORMAT_##fmt: { \
        rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Color format: "#fmt); \
    } break
        LOG_PF(UNKNOWN);
        LOG_PF(INDEX1LSB);
        LOG_PF(INDEX1MSB);
        LOG_PF(INDEX4LSB);
        LOG_PF(INDEX4MSB);
        LOG_PF(INDEX8);
        LOG_PF(RGB332);
        LOG_PF(RGB444);
        LOG_PF(RGB555);
        LOG_PF(BGR555);
        LOG_PF(ARGB4444);
        LOG_PF(RGBA4444);
        LOG_PF(ABGR4444);
        LOG_PF(BGRA4444);
        LOG_PF(ARGB1555);
        LOG_PF(RGBA5551);
        LOG_PF(ABGR1555);
        LOG_PF(BGRA5551);
        LOG_PF(RGB565);
        LOG_PF(BGR565);
        LOG_PF(RGB24);
        LOG_PF(BGR24);
        LOG_PF(RGB888);
        LOG_PF(RGBX8888);
        LOG_PF(BGR888);
        LOG_PF(BGRX8888);
        LOG_PF(ARGB8888);
        LOG_PF(RGBA8888);
        LOG_PF(ABGR8888);
        LOG_PF(BGRA8888);
        LOG_PF(ARGB2101010);
        LOG_PF(YV12);
        LOG_PF(IYUV);
        LOG_PF(YUY2);
        LOG_PF(UYVY);
        LOG_PF(YVYU);
    }
    ASSERT(rv == GFMRV_OK, rv);

    /* Set the return */
    *pWidth = sdlMode.w;
    *pHeight = sdlMode.h;
    *pRefRate = sdlMode.refresh_rate;
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Recalculate helper variables to render the backbuffer into a window
 *
 * @param  [ in]pCtx   The backbuffer context
 * @param  [ in]width  The window's width
 * @param  [ in]height The window's height
 * @return        GFMRV_OK, GFMRV_ARGUMENTS_BAD,
 *                GFMRV_BACKBUFFER_WINDOW_TOO_SMALL
 */
static gfmRV gfmVideoSwSDL2_cacheDimensions(gfmVideoSwSDL2 *pCtx, int width,
        int height) {
    gfmRV rv;
    int horRatio, verRatio;

    /* Check that the window's dimension is valid */
    ASSERT_LOG(width >= pCtx->bbufWidth, GFMRV_BACKBUFFER_WINDOW_TOO_SMALL,
            pCtx->pLog);
    ASSERT_LOG(height >= pCtx->bbufHeight, GFMRV_BACKBUFFER_WINDOW_TOO_SMALL,
            pCtx->pLog);

    /* Check each possible ratio */
    horRatio = (int)( (double)width / (double)pCtx->bbufWidth);
    verRatio = (int)( (double)height / (double)pCtx->bbufHeight);
    /* Get the smaller of the two */
    if (horRatio < verRatio)
        pCtx->scrZoom = horRatio;
    else
        pCtx->scrZoom = verRatio;
    ASSERT_LOG(pCtx->scrZoom > 0, GFMRV_BACKBUFFER_WINDOW_TOO_SMALL,
            pCtx->pLog);

    /* Center the output */
    pCtx->outRect.x = (width - pCtx->bbufWidth * pCtx->scrZoom) / 2;
    pCtx->outRect.y = (height - pCtx->bbufHeight * pCtx->scrZoom) / 2;
    pCtx->outRect.w = pCtx->bbufWidth * pCtx->scrZoom;
    pCtx->outRect.h = pCtx->bbufHeight * pCtx->scrZoom;

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Backbuffer position: %i x %i",
            pCtx->outRect.x, pCtx->outRect.y);
    ASSERT(rv == GFMRV_OK, rv);
    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Backbuffer resized dimensions: "
            "%i x %i", pCtx->outRect.w, pCtx->outRect.h);
    ASSERT(rv == GFMRV_OK, rv);
    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Backbuffer scalling ratio: %i "
            "times", pCtx->scrZoom);
    ASSERT(rv == GFMRV_OK, rv);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Change the fullscreen resolution of the window
 * 
 * NOTE 1: The resolution is the index to one of the previously queried
 * resolutions
 * 
 * NOTE 2: This modification will only take effect when switching to
 * fullscreen mode
 * 
 * @param  [ in]pVideo The video context
 * @param  [ in]index  The resolution's index
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_INTERNAL_ERROR,
 *                     GFMRV_INVALID_INDEX, GFMRV_WINDOW_NOT_INITIALIZED
 */
static gfmRV gfmVideo_SWSDL2_setResolution(gfmVideo *pVideo, int index) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int irv;
    SDL_DisplayMode sdlMode;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(index >= 0, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that the index is valid */
    ASSERT_LOG(index < pCtx->resCount, GFMRV_INVALID_INDEX, pCtx->pLog);
    /* Check that the window was already initialized */
    ASSERT_LOG(pCtx->pSDLWindow, GFMRV_WINDOW_NOT_INITIALIZED, pCtx->pLog);

    /* Retrieve the desired mode */
    irv = SDL_GetDisplayMode(0 /*displayIndex*/, index, &sdlMode);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    /* Check if the backbuffer fit into this new window */
    ASSERT_LOG(sdlMode.w >= pCtx->bbufWidth, GFMRV_BACKBUFFER_WINDOW_TOO_SMALL,
            pCtx->pLog);
    ASSERT_LOG(sdlMode.h >= pCtx->bbufHeight, GFMRV_BACKBUFFER_WINDOW_TOO_SMALL,
            pCtx->pLog);

    /* Switch the fullscreen resolution */
    irv = SDL_SetWindowDisplayMode(pCtx->pSDLWindow, &sdlMode);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Fullscreen resolution set to %i "
            "x %i @ %iHz", sdlMode.w, sdlMode.h, sdlMode.refresh_rate);
    ASSERT(rv == GFMRV_OK, rv);

    if (pCtx->isFullscreen) {
        /* Update helper variables */
        rv = gfmVideoSwSDL2_cacheDimensions(pCtx, sdlMode.w, sdlMode.h);
        ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);
    }

    /* Store the resolution */
    pCtx->curResolution = index;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Create the only window for the game
 * 
 * NOTE 1: The window may switch to fullscreen mode later
 * 
 * NOTE 2: The window's dimensions shall be clamped to the device's ones.
 * The resolution (i.e., width X height X refresh rate) may only take effect
 * when in fullscreen mode, so, in order to set all that on init, use
 * instead gfmVideo_SWSDL2_initFullscreen
 * 
 * NOTE 3: The argument 'isUserResizable' defines whether a user may
 * manually stretch/shrink, but doesn't control whether or not a window's
 * dimensions may be modified programatically
 * 
 * @param  [ in]pCtx       The video context
 * @param  [ in]width      The desired width
 * @param  [ in]height     The desired height
 * @param  [ in]bbufWidth  The backbuffer's width
 * @param  [ in]bbufHeight The backbuffer's height
 * @param  [ in]pName      The game's title
 * @param  [ in]flags      Whether the user can resize the window
 * @param  [ in]vsync      Whether vsync is enabled or not
 * @return                 GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_ALLOC_FAILED,
 *                         GFMRV_INTERNAL_ERROR
 */
static gfmRV gfmVideo_SWSDL2_createWindow(gfmVideoSwSDL2 *pCtx, int width,
        int height, int bbufWidth, int bbufHeight, char *pName,
        SDL_WindowFlags flags, int vsync) {
    gfmRV rv;
    SDL_RendererFlags rFlags;

    /* if pName is NULL, the window should have no title */
    if (!pName) {
        pName = "";
    }

    /* Clamp the dimensions to the device's */
    if (width > pCtx->devWidth) {
        width = pCtx->devWidth;
    }
    if (height > pCtx->devHeight) {
        height = pCtx->devHeight;
    }

    /* Check that the window is big enough to support the backbuffer */
    ASSERT_LOG(bbufWidth <= width, GFMRV_BACKBUFFER_WIDTH_INVALID, pCtx->pLog);
    ASSERT_LOG(bbufHeight <= height, GFMRV_BACKBUFFER_HEIGHT_INVALID,
            pCtx->pLog);

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Creating %i x %i window...",
            width, height);
    ASSERT(rv == GFMRV_OK, rv);

    /* Create the window */
    pCtx->pSDLWindow = SDL_CreateWindow(pName, SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED, width, height, flags);
    ASSERT_LOG(pCtx->pSDLWindow, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    /* Select the renderer flags */
    rFlags = SDL_RENDERER_ACCELERATED;
    if (vsync) {
        rFlags |= SDL_RENDERER_PRESENTVSYNC;

        rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Creating backbuffer with "
                "VSYNC...");
        ASSERT(rv == GFMRV_OK, rv);
    }
    else {
        rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Creating backbuffer...");
        ASSERT(rv == GFMRV_OK, rv);
    }

    /* Create the window's renderer */
    pCtx->pRenderer = SDL_CreateRenderer(pCtx->pSDLWindow, -1, rFlags);
    ASSERT_LOG(pCtx->pRenderer, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    /* Create the backbuffer */
    pCtx->pSDLBackbuffer = SDL_CreateTexture(pCtx->pRenderer,
            SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, bbufWidth,
            bbufHeight);
    ASSERT_LOG(pCtx->pSDLBackbuffer , GFMRV_INTERNAL_ERROR, pCtx->pLog);

    pCtx->pBackbufferData = (unsigned char*)malloc(sizeof(unsigned char) * 3 *
            bbufWidth * bbufHeight);
    ASSERT_LOG(pCtx->pBackbufferData , GFMRV_INTERNAL_ERROR, pCtx->pLog);

    /* Store the window (in windowed mode) dimensions */
    pCtx->wndWidth = width;
    pCtx->wndHeight = height;
    /* Store the backbbufer dimensions */
    pCtx->bbufWidth = bbufWidth;
    pCtx->bbufWidthInBytes = bbufWidth * 3;
    pCtx->bbufHeight = bbufHeight;
    /* Set it at the default resolution (since it's the default behaviour) */
    pCtx->curResolution = 0;

    /* Update helper variables */
    rv = gfmVideoSwSDL2_cacheDimensions(pCtx, width, height);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    /* Set the background color */
    rv = gfmVideo_SWSDL2_setBackgroundColor((gfmVideo*)pCtx, 0xff000000);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    rv = GFMRV_OK;
__ret:
    if (rv != GFMRV_OK) {
        if (pCtx->pSDLWindow) {
            SDL_DestroyWindow(pCtx->pSDLWindow);
        }
    }

    return rv;
}

/**
 * Create the only window for the game
 * 
 * NOTE 1: The window may switch to fullscreen mode later
 * 
 * NOTE 2: The window's dimensions shall be clamped to the device's ones.
 * The resolution (i.e., width X height X refresh rate) may only take effect
 * when in fullscreen mode, so, in order to set all that on init, use
 * instead gfmVideo_SWSDL2_initFullscreen
 * 
 * NOTE 3: The argument 'isUserResizable' defines whether a user may
 * manually stretch/shrink, but doesn't control whether or not a window's
 * dimensions may be modified programatically
 * 
 * @param  [ in]pVideo          The video context
 * @param  [ in]width           The desired width
 * @param  [ in]height          The desired height
 * @param  [ in]bbufWidth       The backbuffer's width
 * @param  [ in]bbufHeight      The backbuffer's height
 * @param  [ in]pName           The game's title (must be NULL terminated)
 * @param  [ in]isUserResizable Whether the user can resize the window
 * @param  [ in]vsync           Whether vsync is enabled or not
 * @return                      GFMRV_OK, GFMRV_ARGUMENTS_BAD,
 *                              GFMRV_ALLOC_FAILED, GFMRV_INTERNAL_ERROR
 */
static gfmRV gfmVideo_SWSDL2_initWindow(gfmVideo *pVideo, int width, int height,
        int bbufWidth, int bbufHeight, char *pName, int isUserResizable,
        int vsync) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    SDL_WindowFlags flags;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(width > 0, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(height > 0, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(width <= 16384, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(height <= 16384, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that it hasn't been initialized */
    ASSERT_LOG(!pCtx->pSDLWindow, GFMRV_WINDOW_ALREADY_INITIALIZED, pCtx->pLog);

    /* Set the SDL flag */
    flags = 0;
    if (isUserResizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Initializing game in windowed "
            "mode");
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    /* Actually create the window */
    rv = gfmVideo_SWSDL2_createWindow(pCtx, width, height, bbufWidth, bbufHeight,
            pName, flags, vsync);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    /* Set it as in windowed mode */
    pCtx->isFullscreen = 0;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Create the only window for the game in fullscreen mode
 * 
 * NOTE 1: The resolution is the index to one of the previously queried
 * resolutions
 * 
 * NOTE 2: The window may switch to windowed mode later
 * 
 * @param  [ in]pVideo          The video context
 * @param  [ in]resolution      The desired resolution
 * @param  [ in]bbufWidth       The backbuffer's width
 * @param  [ in]bbufHeight      The backbuffer's height
 * @param  [ in]pName           The game's title (must be NULL terminated)
 * @param  [ in]isUserResizable Whether the user can resize the window
 * @param  [ in]vsync           Whether vsync is enabled or not
 * @return                      GFMRV_OK, GFMRV_ARGUMENTS_BAD,
 *                              GFMRV_ALLOC_FAILED, GFMRV_INTERNAL_ERROR,
 *                              GFMRV_INVALID_INDEX
 */
static gfmRV gfmVideo_SWSDL2_initWindowFullscreen(gfmVideo *pVideo,
        int resolution, int bbufWidth, int bbufHeight, char *pName,
        int isUserResizable, int vsync) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    SDL_WindowFlags flags;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(resolution >= 0, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that the resolution is valid */
    ASSERT_LOG(resolution < pCtx->resCount, GFMRV_INVALID_INDEX, pCtx->pLog);
    /* Check that it hasn't been initialized */
    ASSERT_LOG(!pCtx->pSDLWindow, GFMRV_WINDOW_ALREADY_INITIALIZED, pCtx->pLog);

    /* Set the SDL flag */
    flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (isUserResizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Initializing game in fullscreen "
            "mode");
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    /* Actually create the window */
    rv = gfmVideo_SWSDL2_createWindow(pCtx, pCtx->devWidth, pCtx->devHeight,
            bbufWidth, bbufHeight, pName, flags, vsync);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    /* Set it as in fullscreen mode */
    pCtx->isFullscreen = 1;

    /* Set the current resolution */
    rv = gfmVideo_SWSDL2_setResolution(pCtx, resolution);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Set the window's dimensions
 * 
 * This modification will only take effect when in windowed mode. If the
 * window is currently in fullscreen mode, the modification will be delayed
 * until the switch is made
 * 
 * @param  [ in]pVideo The video context
 * @param  [ in]width  The desired width
 * @param  [ in]height The desired height
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_INTERNAL_ERROR,
 *                     GFMRV_WINDOW_NOT_INITIALIZED
 */
static gfmRV gfmVideo_SWSDL2_setDimensions(gfmVideo *pVideo, int width,
        int height) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(width > 0, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(height > 0, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that the window was initialized */
    ASSERT_LOG(pCtx->pSDLWindow, GFMRV_WINDOW_NOT_INITIALIZED, pCtx->pLog);

    /* Clamp the dimensions to the device's */
    if (width > pCtx->devWidth) {
        width = pCtx->devWidth;
    }
    if (height > pCtx->devHeight) {
        height = pCtx->devHeight;
    }

    /* Check if the backbuffer fit into this new window */
    ASSERT_LOG(width >= pCtx->bbufWidth, GFMRV_BACKBUFFER_WINDOW_TOO_SMALL,
            pCtx->pLog);
    ASSERT_LOG(height >= pCtx->bbufHeight, GFMRV_BACKBUFFER_WINDOW_TOO_SMALL,
            pCtx->pLog);

    /* Try to change the dimensions */
    SDL_SetWindowSize(pCtx->pSDLWindow, width, height);

    if (!pCtx->isFullscreen) {
        /* Update helper variables */
        rv = gfmVideoSwSDL2_cacheDimensions(pCtx, width, height);
        ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);
    }

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Window dimensions set to %i x %i",
            width, height);
    ASSERT(rv == GFMRV_OK, rv);

    /* Store the new  dimensions */
    pCtx->wndWidth = width;
    pCtx->wndHeight = height;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Retrieve the window's dimensions
 * 
 * If the window is in fullscreen mode, retrieve the dimensions for the
 * current resolution
 * 
 * @param  [out]pWidth  The current width
 * @param  [out]pHeight The current height
 * @param  [ in]pVideo  The video context
 * @return              GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_INTERNAL_ERROR,
 *                      GFMRV_WINDOW_NOT_INITIALIZED
 */
static gfmRV gfmVideo_SWSDL2_getDimensions(int *pWidth, int *pHeight,
        gfmVideo *pVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int irv;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(pWidth, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(pHeight, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that the window was initialized */
    ASSERT_LOG(pCtx->pSDLWindow, GFMRV_WINDOW_NOT_INITIALIZED, pCtx->pLog);

    if (!pCtx->isFullscreen) {
        /* Retrieve the window's dimensions */
        *pWidth = pCtx->wndWidth;
        *pHeight = pCtx->wndHeight;
    }
    else {
        SDL_DisplayMode sdlMode;

        /* Retrieve the dimensions for the current resolution mode */
        irv = SDL_GetDisplayMode(0 /*displayIndex*/, pCtx->curResolution,
                &sdlMode);
        ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);

        *pWidth = sdlMode.w;
        *pHeight = sdlMode.h;
    }

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Swith the current window mode to fullscreen
 * 
 * @param  [ in]pVideo The video context
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_INTERNAL_ERROR,
 *                     GFMRV_WINDOW_MODE_UNCHANGED, GFMRV_WINDOW_NOT_INITIALIZED
 */
static gfmRV gfmVideo_SWSDL2_setFullscreen(gfmVideo *pVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int irv;
    SDL_DisplayMode sdlMode;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that the window was initialized */
    ASSERT_LOG(pCtx->pSDLWindow, GFMRV_WINDOW_NOT_INITIALIZED, pCtx->pLog);

    /* Check that the window isn't in fullscreen mode */
    ASSERT_LOG(!pCtx->isFullscreen, GFMRV_WINDOW_MODE_UNCHANGED, pCtx->pLog);

    /* Retrieve the desired mode */
    irv = SDL_GetDisplayMode(0 /*displayIndex*/, pCtx->curResolution, &sdlMode);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    /* Try to make it fullscrren */
    irv = SDL_SetWindowFullscreen(pCtx->pSDLWindow,
            SDL_WINDOW_FULLSCREEN_DESKTOP);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    pCtx->isFullscreen = 1;

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Just switched to fullscreen "
            "mode");
    ASSERT(rv == GFMRV_OK, rv);

    /* Update helper variables */
    rv = gfmVideoSwSDL2_cacheDimensions(pCtx, sdlMode.w, sdlMode.h);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Swith the current window mode to windowed
 * 
 * @param  [ in]pVideo The video context
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_INTERNAL_ERROR,
 *                     GFMRV_WINDOW_MODE_UNCHANGED, GFMRV_WINDOW_NOT_INITIALIZED
 */
static gfmRV gfmVideo_SWSDL2_setWindowed(gfmVideo *pVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int irv;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that the window was initialized */
    ASSERT_LOG(pCtx->pSDLWindow, GFMRV_WINDOW_NOT_INITIALIZED, pCtx->pLog);

    /* Check that the window isn't in windowed mode */
    ASSERT_LOG(pCtx->isFullscreen, GFMRV_WINDOW_MODE_UNCHANGED, pCtx->pLog);

    /* Try to make it fullscrren */
    irv = SDL_SetWindowFullscreen(pCtx->pSDLWindow, 0);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    pCtx->isFullscreen = 0;

    rv = gfmLog_log(pCtx->pLog, gfmLog_info, "Just switched to windowed mode");
    ASSERT(rv == GFMRV_OK, rv);

    /* Update helper variables */
    rv = gfmVideoSwSDL2_cacheDimensions(pCtx, pCtx->wndWidth, pCtx->wndHeight);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Retrieve the backbuffer's dimensions
 * 
 * @param  [out]pWidth  The width
 * @param  [out]pHeight The height
 * @param  [ in]pVideo  The video context
 * @return              GFMRV_OK, GFMRV_ARGUMENTS_BAD, ...
 */
static gfmRV gfmVideo_SWSDL2_getBackbufferDimensions(int *pWidth, int *pHeight,
        gfmVideo *pVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(pWidth, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(pHeight, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that the window was initialized */
    ASSERT_LOG(pCtx->pSDLWindow, GFMRV_WINDOW_NOT_INITIALIZED, pCtx->pLog);

    /* Retrieve the backbuffer's dimensions */
    *pWidth = pCtx->bbufWidth;
    *pHeight = pCtx->bbufHeight;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Convert a point in window-space to backbuffer-space
 * 
 * NOTE: Both pX and pY must be initialized with the window-space point
 * 
 * @param  [out]pX   The horizontal position, in backbuffer-space
 * @param  [out]pY   The vertical position, in backbuffer-space
 * @param  [ in]pVideo The video context
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, ...
 */
static gfmRV gfmVideo_SWSDL2_windowToBackbuffer(int *pX, int *pY,
        gfmVideo *pVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(pX, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(pY, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that it was initialized */
    ASSERT_LOG(pCtx->pRenderer, GFMRV_BACKBUFFER_NOT_INITIALIZED, pCtx->pLog);

    /* Convert the space */
    *pX = (*pX - pCtx->outRect.x) / (float)pCtx->scrZoom;
    *pY = (*pY - pCtx->outRect.y) / (float)pCtx->scrZoom;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Initialize the rendering operation
 * 
 * @param  [ in]pVideo The video context
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, ...
 */
static gfmRV gfmVideo_SWSDL2_drawBegin(gfmVideo *pVideo) {
    gfmVideoSwSDL2 *pCtx;
    unsigned char *pData;
    gfmRV rv;
    int i;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that it was initialized */
    ASSERT_LOG(pCtx->pRenderer, GFMRV_BACKBUFFER_NOT_INITIALIZED, pCtx->pLog);

    /* Clear the previous frame */
    i = 0;
    pData = pCtx->pBackbufferData;
    while (i < pCtx->bbufWidth * pCtx->bbufHeight) {
        pData[0] = pCtx->bgRed;
        pData[1] = pCtx->bgGreen;
        pData[2] = pCtx->bgBlue;
        pData += 3;

        i++;
    }

    pCtx->lastNumObjects = pCtx->totalNumObjects;
    pCtx->totalNumObjects = 0;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Draw a tile into the backbuffer
 * 
 * @param  [ in]pVideo    The video context
 * @param  [ in]pSset     Spriteset containing the tile
 * @param  [ in]dstX      Horizontal (top-left) position in screen-space
 * @param  [ in]dstY      Vertical (top-left) position in screen-space
 * @param  [ in]tile      Index of the tile
 * @param  [ in]isFlipped Whether the tile should be flipped
 * @return                GFMRV_OK, GFMRV_ARGUMENTS_BAD, ...
 */
static gfmRV gfmVideo_SWSDL2_drawTile(gfmVideo *pVideo, gfmSpriteset *pSset,
        int dstX, int dstY, int tile, int isFlipped) {
    gfmTexture *pTex;
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int srcH, srcW, srcX, srcY;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(pSset, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(tile >= 0, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that it was initialized */
    ASSERT_LOG(pCtx->pRenderer, GFMRV_BACKBUFFER_NOT_INITIALIZED, pCtx->pLog);

    /* Retrieve the spriteset's texture */
    rv = gfmSpriteset_getTexture(&pTex, pSset);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    /* Get the tile's dimensions and position into the spriteset */
    rv = gfmSpriteset_getDimension(&srcW, &srcH, pSset);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);
    rv = gfmSpriteset_getPosition(&srcX, &srcY, pSset, tile);
    ASSERT_LOG(rv == GFMRV_OK, rv, pCtx->pLog);

    /* Clamp the sprite the the visible position */
    if (dstX < 0) {
        srcX -= dstX;
        srcW += dstX;
        dstX = 0;
    }
    if (dstX + srcW > pCtx->bbufWidth) {
        srcW = pCtx->bbufWidth - dstX;
    }
    if (dstY < 0) {
        srcY -= dstY;
        srcH += dstY;
        dstY = 0;
    }
    if (dstY + srcH > pCtx->bbufHeight) {
        srcH = pCtx->bbufHeight - dstY;
    }

    do {
        unsigned char *pDst, *pMask, *pSrc;
        int j, maskInc, pixelInc;

        /* Retrieve the initial position on the destination and source
         * buffers */
        if (isFlipped) {
            maskInc = -1;
            pixelInc = -3;

            pDst = pCtx->pBackbufferData + (dstX + srcW - 1) * 3 +
                    dstY * pCtx->bbufWidthInBytes;
            pSrc = pTex->pData + (srcX + srcW - 1) * 3 +
                    srcY * pTex->widthInBytes;
            pMask = pTex->pMask + srcX + srcW - 1 + srcY * pTex->width;
        }
        else {
            maskInc = 1;
            pixelInc = 3;

            pDst = pCtx->pBackbufferData + dstX * 3 + dstY * pCtx->bbufWidthInBytes;
            pSrc = pTex->pData + srcX * 3 + srcY * pTex->widthInBytes;
            pMask = pTex->pMask + srcX + srcY * pTex->width;
        }

        /* Blit the source into the destination */
        j = 0;
        while (j < srcH) {
            unsigned char *pTmpDst, *pTmpMask, *pTmpSrc;
            int i;

            pTmpDst = pDst;
            pTmpSrc = pSrc;
            pTmpMask = pMask;
            i = 0;
            while (i < srcW) {
                /* Clean the opaque pixels in the destination */
                pTmpDst[0] &= *pTmpMask;
                pTmpDst[1] &= *pTmpMask;
                pTmpDst[2] &= *pTmpMask;
                /* Put the source pixels */
                pTmpDst[0] |= pTmpSrc[0];
                pTmpDst[1] |= pTmpSrc[1];
                pTmpDst[2] |= pTmpSrc[2];

                pTmpDst += pixelInc;
                pTmpSrc += pixelInc;
                pTmpMask += maskInc;
                i++;
            } /* while (i < srcW) */

            pDst += pCtx->bbufWidthInBytes;
            pSrc += pTex->widthInBytes;
            pMask += pTex->width;
            j++;
        } /* while (j < srcH) */
    } while (0);

    pCtx->totalNumObjects++;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Draw the borders of a rectangle into the backbuffer
 * 
 * @param  [ in]pVideo The video context
 * @param  [ in]x      Horizontal (top-left) position in screen-space
 * @param  [ in]y      Vertical (top-left) position in screen-space
 * @param  [ in]width  The rectangle's width
 * @param  [ in]height The rectangle's height
 * @param  [ in]color  The background color (in 0xAARRGGBB format)
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, ...
 */
static gfmRV gfmVideo_SWSDL2_drawRectangle(gfmVideo *pVideo, int x, int y,
        int width, int height, int color) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int irv;
    SDL_Rect rect;
    unsigned char alpha, blue, green, red;

    return GFMRV_FUNCTION_NOT_IMPLEMENTED;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that it was initialized */
    ASSERT_LOG(pCtx->pRenderer, GFMRV_BACKBUFFER_NOT_INITIALIZED, pCtx->pLog);

    /* Check that the rectangle is inside the screen */
    if (x + width < 0) {
        return GFMRV_OK;
    }
    if (y + height < 0) {
        return GFMRV_OK;
    }
    if (x >= pCtx->bbufWidth) {
        return GFMRV_OK;
    }
    if (y >= pCtx->bbufHeight) {
        return GFMRV_OK;
    }

    /* Retrieve each color component */
    alpha = (color >> 24) & 0xff;
    red   = (color >> 16) & 0xff;
    green = (color >> 8) & 0xff;
    blue  = color & 0xff;

    /* Set the rect's dimensions */
    rect.x = x;
    rect.y = y;
    rect.w = width;
    rect.h = height;

    /* Set the color to render it */
    irv = SDL_SetRenderDrawColor(pCtx->pRenderer, red, green, blue, alpha);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    irv = SDL_RenderDrawRect(pCtx->pRenderer, &rect);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    pCtx->totalNumObjects++;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Draw a solid rectangle into the backbuffer
 * 
 * @param  [ in]pVideo The video context
 * @param  [ in]x      Horizontal (top-left) position in screen-space
 * @param  [ in]y      Vertical (top-left) position in screen-space
 * @param  [ in]width  The rectangle's width
 * @param  [ in]height The rectangle's height
 * @param  [ in]color  The background color (in 0xAARRGGBB format)
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, ...
 */
static gfmRV gfmVideo_SWSDL2_drawFillRectangle(gfmVideo *pVideo, int x, int y,
        int width, int height, int color) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int irv;
    SDL_Rect rect;
    unsigned char alpha, blue, green, red;

    return GFMRV_FUNCTION_NOT_IMPLEMENTED;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that it was initialized */
    ASSERT_LOG(pCtx->pRenderer, GFMRV_BACKBUFFER_NOT_INITIALIZED, pCtx->pLog);

    /* Check that the rectangle is inside the screen */
    if (x + width < 0) {
        return GFMRV_OK;
    }
    if (y + height < 0) {
        return GFMRV_OK;
    }
    if (x >= pCtx->bbufWidth) {
        return GFMRV_OK;
    }
    if (y >= pCtx->bbufHeight) {
        return GFMRV_OK;
    }

    /* Retrieve each color component */
    alpha = (color >> 24) & 0xff;
    red   = (color >> 16) & 0xff;
    green = (color >> 8) & 0xff;
    blue  = color & 0xff;

    /* Set the rect's dimensions */
    rect.x = x;
    rect.y = y;
    rect.w = width;
    rect.h = height;

    /* Set the color to render it */
    irv = SDL_SetRenderDrawColor(pCtx->pRenderer, red, green, blue, alpha);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    irv = SDL_RenderFillRect(pCtx->pRenderer, &rect);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);

    pCtx->totalNumObjects++;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Get the backbuffer's data (i.e., composite of everything rendered to it
 * since the last gfmBackbuffer_drawBegin)
 * 
 * NOTE 1: Data is returned as 24 bits colors, with 8 bits per color and
 *         RGB order
 * 
 * NOTE 2: This function must be called twice. If pData is NULL, pLen will
 *         return the necessary length for the buffer. If pData isn't NULL,
 *         then pLen must be the length of pData
 * 
 * @param  [out]pData  Buffer where the data should be retrieved (caller
 *                     allocated an freed)
 * @param  [out]pLen   Returns the buffer length, in bytes
 * @param  [ in]pVideo The video context
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD,
 *                     GFMRV_BACKBUFFER_NOT_INITIALIZED,
 *                     GFMRV_BUFFER_TOO_SMALL, GFMRV_INTERNAL_ERROR
 */
static gfmRV gfmVideo_SWSDL2_getBackbufferData(unsigned char *pData, int *pLen,
        gfmVideo *pVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;
    int len;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that it was initialized */
    ASSERT_LOG(pCtx->pRenderer, GFMRV_BACKBUFFER_NOT_INITIALIZED, pCtx->pLog);

    /* Calculate the required length */
    len = pCtx->bbufWidth * pCtx->bbufHeight * 3 * sizeof(unsigned char);

    /* Check that either the buffer is big enough or it's requesting the len */
    ASSERT_LOG(!pData || *pLen >= len, GFMRV_BUFFER_TOO_SMALL, pCtx->pLog);
    /* Store the return value */
    *pLen = len;
    /* If requested, return the required size */
    if (!pData) {
        return GFMRV_OK;
    }

    /* Retrieve the data */
    memcpy(pData, pCtx->pBackbufferData, len);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Finalize the rendering operation
 * 
 * @param  [ in]pVideo The video context
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_INTERNAL_ERROR
 */
static gfmRV gfmVideo_SWSDL2_drawEnd(gfmVideo *pVideo) {
    gfmVideoSwSDL2 *pCtx;
    unsigned char *pBbPixels;
    gfmRV rv;
    int irv, pitch;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that it was initialized */
    ASSERT_LOG(pCtx->pRenderer, GFMRV_BACKBUFFER_NOT_INITIALIZED, pCtx->pLog);

    /* Update the backbuffer */
    irv = SDL_LockTexture(pCtx->pSDLBackbuffer, 0, (void**)&pBbPixels, &pitch);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    do {
        unsigned char *pSrc;
        int j;

        j = 0;
        pSrc = pCtx->pBackbufferData;
        while (j < pCtx->bbufHeight) {
            memcpy(pBbPixels, pSrc, pCtx->bbufWidthInBytes);

            pBbPixels += pitch;
            pSrc += pCtx->bbufWidthInBytes;
            j++;
        }
    } while (0);
    SDL_UnlockTexture(pCtx->pSDLBackbuffer);

    /* Set the screen as rendering target */
    irv = SDL_SetRenderTarget(pCtx->pRenderer, 0);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    /* Clear the screen */
    irv = SDL_SetRenderDrawColor(pCtx->pRenderer, 0/*r*/, 0/*g*/, 0/*b*/,
            0/*a*/);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    irv = SDL_RenderClear(pCtx->pRenderer);
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    /* Render the backbuffer to the screen */
    irv = SDL_RenderCopy(pCtx->pRenderer, pCtx->pSDLBackbuffer, 0/*srcRect*/,
            &(pCtx->outRect));
    ASSERT_LOG(irv == 0, GFMRV_INTERNAL_ERROR, pCtx->pLog);
    SDL_RenderPresent(pCtx->pRenderer);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Retrieve information about the last frame
 * 
 * @param  [out]pBatched The number of batched draws
 * @param  [out]pNum     The number of sprites rendered
 * @param  [ in]pvideo   The video context
 * @return               GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_INTERNAL_ERROR
 */
gfmRV gfmVideo_SWSDL2_getDrawInfo(int *pBatched, int *pNum, gfmVideo *pVideo) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(pNum, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    ASSERT_LOG(pBatched, GFMRV_ARGUMENTS_BAD, pCtx->pLog);
    /* Check that it was initialized */
    ASSERT_LOG(pCtx->pRenderer, GFMRV_BACKBUFFER_NOT_INITIALIZED, pCtx->pLog);

    /* Retrieve the info */
    *pBatched = pCtx->lastNumObjects;
    *pNum = pCtx->lastNumObjects;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Alloc a new texture
 * 
 * @param  [out]ppCtx The alocated texture
 * @return            GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_ALLOC_FAILED
 */
static gfmRV gfmVideo_SWSDL2_getNewTexture(gfmTexture **ppCtx) {
    gfmRV rv;

    /* Alloc the texture */
    *ppCtx = (gfmTexture*)malloc(sizeof(gfmTexture));
    ASSERT(*ppCtx, GFMRV_ALLOC_FAILED);

    /* Initialize the object */
    memset(*ppCtx, 0x0, sizeof(gfmTexture));

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Initialize a texture
 * 
 * @param  [ in]pCtx   The alocated texture
 * @param  [ in]pVideo The video context
 * @param  [ in]width  The texture's width
 * @param  [ in]height The texture's height
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD
 */
static gfmRV gfmVideoSwSDL2_initTexture(gfmTexture *pCtx, gfmVideoSwSDL2 *pVideo,
        int width, int height) {
    gfmLog *pLog;
    gfmRV rv;

    pLog = pVideo->pLog;

    /* Sanitize arguments */
    ASSERT_LOG(width > 0, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(height > 0, GFMRV_ARGUMENTS_BAD, pLog);
    /* Check the dimensions */
    ASSERT_LOG(gfmUtils_isPow2(width) == GFMRV_TRUE,
            GFMRV_TEXTURE_INVALID_WIDTH, pLog);
    ASSERT_LOG(gfmUtils_isPow2(height) == GFMRV_TRUE,
            GFMRV_TEXTURE_INVALID_HEIGHT, pLog);

    /* Create the texture */
    pCtx->pData = (unsigned char*)malloc(sizeof(unsigned char) * 3 * width *
            height);
    ASSERT_LOG(pCtx->pData, GFMRV_ALLOC_FAILED, pLog);
    pCtx->pMask = (unsigned char*)malloc(sizeof(unsigned char) * width *
            height);
    ASSERT_LOG(pCtx->pMask, GFMRV_ALLOC_FAILED, pLog);
    pCtx->width = width;
    pCtx->widthInBytes = width * 3;
    pCtx->height = height;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Loads a 24 bits bitmap file into a texture
 * 
 * NOTE: The image's dimensions must be power of two (e.g., 256x256)
 * 
 * @param  [out]pTex     Handle to the loaded texture
 * @param  [ in]pVideo   The video context
 * @param  [ in]pData    The texture's data (encoded as as 24 bits 0xRRGGBB)
 * @param  [ in]width    The texture's width
 * @param  [ in]height   The texture's height
 */
static gfmRV gfmVideo_SWSDL2_loadTexture(int *pTex, gfmVideo *pVideo,
        char *pData, int width, int height) {
    gfmLog *pLog;
    gfmTexture *pTexture;
    gfmVideoSwSDL2 *pCtx;
    gfmRV rv;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Zero variable that must be cleaned on error */
    pTexture = 0;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);

    pLog = pCtx->pLog;

    ASSERT(pLog, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(pTex, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(pData, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT(gfmUtils_isPow2(width) == GFMRV_TRUE, GFMRV_TEXTURE_INVALID_WIDTH);
    ASSERT(gfmUtils_isPow2(height) == GFMRV_TRUE, GFMRV_TEXTURE_INVALID_HEIGHT);

    /* Initialize the texture  */
    gfmGenArr_getNextRef(gfmTexture, pCtx->pTextures, 1/*incRate*/, pTexture,
            gfmVideo_SWSDL2_getNewTexture);
    rv = gfmVideoSwSDL2_initTexture(pTexture, pCtx, width, height);
    ASSERT_LOG(rv == GFMRV_OK, rv, pLog);

    /* Load the data into texture */
    do {
        unsigned char *pTexData, *pTexMask, *pSrc;
        int j;

        pSrc = (unsigned char*)pData;
        pTexData = pTexture->pData;
        pTexMask = pTexture->pMask;
        j = 0;
        while (j < height) {
            int i;

            i = 0;
            while (i < width) {
                *pTexMask = ~pSrc[3];
                pTexData[0] = pSrc[0] & pSrc[3];
                pTexData[1] = pSrc[1] & pSrc[3];
                pTexData[2] = pSrc[2] & pSrc[3];

                pSrc += 4;
                pTexData += 3;
                pTexMask++;
                i++;
            }
            j++;
        }
    } while (0);

    /* Get the texture's index */
    *pTex = gfmGenArr_getUsed(pCtx->pTextures);
    /* Push the texture into the array */
    gfmGenArr_push(pCtx->pTextures);

    rv = GFMRV_OK;
__ret:
    if (rv != GFMRV_OK) {
        if (pTexture) {
            /* On error, clean the texture and remove it from the list */
            gfmVideo_SWSDL2_freeTexture(&pTexture);
            gfmGenArr_pop(pCtx->pTextures);
        }
    }

    return rv;
}

/**
 * Retrieve a texture's pointer from its index
 * 
 * @param  [out]pTexture The texture
 * @param  [ in]pVideo   The video context
 * @param  [ in]handle   The texture's handle
 * @param  [ in]pLog     The logger interface
 * @return               GFMRV_OK, GFMRV_ARUMENTS_BAD, GFMRV_INVALID_INDEX
 */
static gfmRV gfmVideo_SWSDL2_getTexture(gfmTexture **ppTexture, gfmVideo *pVideo,
        int handle, gfmLog *pLog) {
    gfmRV rv;
    gfmVideoSwSDL2 *pCtx;

    /* Retrieve the internal video context */
    pCtx = (gfmVideoSwSDL2*)pVideo;

    /* Sanitize arguments */
    ASSERT(pLog, GFMRV_ARGUMENTS_BAD);
    ASSERT_LOG(pCtx, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(ppTexture, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(handle >= 0, GFMRV_ARGUMENTS_BAD, pLog);
    /* Check that the texture is valid */
    ASSERT_LOG(handle < gfmGenArr_getUsed(pCtx->pTextures), GFMRV_INVALID_INDEX,
            pLog);

    /* Return the texture */
    *ppTexture = gfmGenArr_getObject(pCtx->pTextures, handle);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Retrieves a texture's dimensions
 * 
 * @param [out]pWidth  The texture's width
 * @param [out]pHeight The texture's height
 * @param [ in]pCtx    The texture
 * @return             GFMRV_OK, GFMRV_ARGUMENTS_BAD
 */
static gfmRV gfmVideo_SWSDL2_getTextureDimensions(int *pWidth, int *pHeight,
        gfmTexture *pCtx) {
    gfmRV rv;

    /* Sanitize arguments */
    ASSERT(pWidth, GFMRV_ARGUMENTS_BAD);
    ASSERT(pHeight, GFMRV_ARGUMENTS_BAD);
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);

    *pWidth = pCtx->width;
    *pHeight = pCtx->height;

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Load all software video functions into the struct
 *
 * NOTE: SDL2 is still used for rendering to the screen
 * 
 * @param  [ in]pCtx The video function context
 * @return           GFMRV_OK, GFMRV_ARGUMENTS_BAD
 */
gfmRV gfmVideo_SWSDL2_loadFunctions(gfmVideoFuncs *pCtx) {
    gfmRV rv;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);

    /* Simply copy all function pointer */
    pCtx->gfmVideo_init = gfmVideo_SWSDL2_init;
    pCtx->gfmVideo_free = gfmVideo_SWSDL2_free;
    pCtx->gfmVideo_countResolutions = gfmVideo_SWSDL2_countResolutions;
    pCtx->gfmVideo_getResolution = gfmVideo_SWSDL2_getResolution;
    pCtx->gfmVideo_initWindow = gfmVideo_SWSDL2_initWindow;
    pCtx->gfmVideo_initWindowFullscreen = gfmVideo_SWSDL2_initWindowFullscreen;
    pCtx->gfmVideo_setDimensions = gfmVideo_SWSDL2_setDimensions;
    pCtx->gfmVideo_getDimensions = gfmVideo_SWSDL2_getDimensions;
    pCtx->gfmVideo_setFullscreen = gfmVideo_SWSDL2_setFullscreen;
    pCtx->gfmVideo_setWindowed = gfmVideo_SWSDL2_setWindowed;
    pCtx->gfmVideo_setResolution = gfmVideo_SWSDL2_setResolution;
    pCtx->gfmVideo_getBackbufferDimensions = gfmVideo_SWSDL2_getBackbufferDimensions;
    pCtx->gfmVideo_windowToBackbuffer = gfmVideo_SWSDL2_windowToBackbuffer;
    pCtx->gfmVideo_setBackgroundColor = gfmVideo_SWSDL2_setBackgroundColor;
    pCtx->gfmVideo_loadTexture= gfmVideo_SWSDL2_loadTexture;
    pCtx->gfmVideo_drawBegin = gfmVideo_SWSDL2_drawBegin;
    pCtx->gfmVideo_drawTile = gfmVideo_SWSDL2_drawTile;
    pCtx->gfmVideo_drawRectangle = gfmVideo_SWSDL2_drawRectangle;
    pCtx->gfmVideo_drawFillRectangle = gfmVideo_SWSDL2_drawFillRectangle;
    pCtx->gfmVideo_getBackbufferData = gfmVideo_SWSDL2_getBackbufferData;
    pCtx->gfmVideo_drawEnd = gfmVideo_SWSDL2_drawEnd;
    pCtx->gfmVideo_getTexture = gfmVideo_SWSDL2_getTexture;
    pCtx->gfmVideo_getTextureDimensions = gfmVideo_SWSDL2_getTextureDimensions;
    pCtx->gfmVideo_getDrawInfo = gfmVideo_SWSDL2_getDrawInfo;

    rv = GFMRV_OK;
__ret:
    return rv;
}

