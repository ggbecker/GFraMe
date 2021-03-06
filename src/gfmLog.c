/**
 * @file src/gfmLog.c
 * 
 * Module for logging stuff
 */
#include <GFraMe/gframe.h>
#include <GFraMe/gfmAssert.h>
#include <GFraMe/gfmError.h>
#include <GFraMe/gfmLog.h>
#ifdef EMCC
#  include <stdio.h>
#else 
#  include <GFraMe/core/gfmFile_bkend.h>
#endif

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** The gfmLog struct */
struct stGFMLog {
#ifndef EMCC
    /** The current log file */
    gfmFile *pFile;
#endif
    /** The minimum level for logging */
    gfmLogLevel minLevel;
};

/** States for the log parser */
enum enGFMLogParser {
    gfmLogParser_waiting = 0,
    gfmLogParser_getOptions,
    gfmLogParser_getType,
    gfmLogParser_max
};
typedef enum enGFMLogParser gfmLogParser;

/**
 * Alloc a new logger
 * 
 * @param  ppCtx The logger
 * @return       GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_ALLOC_FAILED
 */
gfmRV gfmLog_getNew(gfmLog **ppCtx) {
    gfmRV rv;
    
    // Sanitize arguments
    ASSERT(ppCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT(!(*ppCtx), GFMRV_ARGUMENTS_BAD);
    
    // Alloc the struct
    *ppCtx = (gfmLog*)malloc(sizeof(gfmLog));
    ASSERT(*ppCtx, GFMRV_ALLOC_FAILED);
    // Clean it
    memset(*ppCtx, 0x0, sizeof(gfmLog));
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Close the file and clean the logger
 * 
 * @param  ppCtx The logger
 * @reutrn       GFMRV_OK, GFMRV_ARGUMENTS_BAD
 */
gfmRV gfmLog_free(gfmLog **ppCtx) {
    gfmRV rv;
    
    // Sanitize arguments
    ASSERT(ppCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT(*ppCtx, GFMRV_ARGUMENTS_BAD);
    
    // Clean it
    gfmLog_clean(*ppCtx);
    // Free it
    free(*ppCtx);
    *ppCtx = 0;
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Initialize the logger
 * 
 * @param  pLog  The logger context
 * @param  pCtx  The game's context
 * @param  level The minimum logging level
 * @return       GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_LOG_ALREADY_INITIALIZED,
 *               GFMRV_COULDNT_OPEN_FILE, GFMRV_LOG_INVALID_LEVEL
 */
gfmRV gfmLog_init(gfmLog *pLog, gfmCtx *pCtx, gfmLogLevel level) {
    gfmRV rv;
    
    // Sanitize arguments
    ASSERT(pLog, GFMRV_ARGUMENTS_BAD);
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    // Check that the log level is valid
    ASSERT(level > gfmLog_none, GFMRV_LOG_INVALID_LEVEL);
    ASSERT(level < gfmLog_max, GFMRV_LOG_INVALID_LEVEL);
#ifndef EMCC
    // Check that the log stil wasn't initialized
    ASSERT(pLog->pFile == 0, GFMRV_LOG_ALREADY_INITIALIZED);
    
    // Alloc and open the file
    rv = gfmFile_getNew(&(pLog->pFile));
    ASSERT(rv == GFMRV_OK, rv);

    /* TODO Check for an "error on previous run" flag and open in append mode */

    rv = gfmFile_openLocal(pLog->pFile, pCtx, "game.log", 8/*nameLen*/, "w");
    ASSERT(rv == GFMRV_OK, rv);
#endif
    
    // Set the minimum log level
    pLog->minLevel = level;
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Close the log file and clean resources
 * 
 * @param  pLog  The logger context
 * @return       GFMRV_OK, GFMRV_ARGUMENTS_BAD
 */
gfmRV gfmLog_clean(gfmLog *pCtx) {
    gfmRV rv;
    
    // Sanitize arguments
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    
#ifndef EMCC
    gfmFile_free(&(pCtx->pFile));
#endif
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Write a string to the file
 * 
 * @param  pCtx The logger
 * @param  pStr The string
 * @param  len  The string's length
 */
static gfmRV gfmLog_logString(gfmLog *pCtx, char *pStr, int len) {
    gfmRV rv;
    
    // Sanitize arguments
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    // Check that the logger was initialized
#ifndef EMCC
    ASSERT(pCtx->pFile, GFMRV_LOG_NOT_INITIALIZED);
#endif
    
    // Write the bytes to the file
#ifdef EMCC
    printf("%.*s", len, pStr);
#else
    rv = gfmFile_writeBytes(pCtx->pFile, pStr, len);
    ASSERT(rv == GFMRV_OK, rv);
#endif
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Log an integer
 * 
 * @param  pCtx The logger
 * @param  val  Value to be logged
 */
static gfmRV gfmLog_logInt(gfmLog *pCtx, int val) {
    /* Enough bytes for 32 bits signed + '\0' */
    char pBuf[12], *pTmp;
    gfmRV rv;
    int len, isNeg;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that the logger was initialized */
#ifndef EMCC
    ASSERT(pCtx->pFile, GFMRV_LOG_NOT_INITIALIZED);
#endif

    /* Stored whether the number is negative */
    isNeg = val < 0;
    if (val < 0) {
        val = -val;
    }

    /* Point to the end of the string */
    pTmp = pBuf + sizeof(pBuf) - 1;
    /* Set the NULL terminator */
    *pTmp = '\0';

    len = 0;
    /* Loop until the last digit (so '0' is correctly displayed) */
    while (val >= 10 && len < 10) {
        char c;

        /* "Convert" the integer to a string (a digit at a time) */
        c = (char)(val % 10);
        val /= 10;
        c += '0';

        /* Prepend the character */
        pTmp--;
        *pTmp = c;
        len++;
    }

    /* Add the last digit to the string */
    pTmp--;
    *pTmp = (char)(val % 10 + '0');
    len++;
    /* Set the signal, if necessary */
    if (isNeg) {
        pTmp--;
        *pTmp = '-';
        len++;
    }

    /* Write the integer as a string */
    rv = gfmLog_logString(pCtx, pTmp, len);
    ASSERT(rv == GFMRV_OK, rv);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Log an hexadecimal number
 *
 * NOTE: It's always logged as 8 hexadecimal digits
 *
 * @param  [ in]pCtx    The logger
 * @param  [ in]val     The value to be logged
 * @param  [ in]isUpper Whether it should be printed as upper case
 * @return              GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_LOG_NOT_INITIALIZED
 */
static gfmRV gfmLog_logHex(gfmLog *pCtx, int val, int isUpper) {
    /* Enough bytes for 8 hexadecimal digits + '\0' */
    char pBuf[9];
    gfmRV rv;
    int i;

    /* Sanitize arguments */
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    /* Check that the logger was initialized */
#ifndef EMCC
    ASSERT(pCtx->pFile, GFMRV_LOG_NOT_INITIALIZED);
#endif

    i = 0;
    while (i < 8) {
        char c;

        c = val & 0x0000000f;

        /* Output the current nibble */
        if (c < 10) {
            pBuf[7 - i] = c + '0';
        }
        else if (isUpper) {
            pBuf[7 - i] = c - 10 + 'A';
        }
        else {
            pBuf[7 - i] = c - 10 + 'a';
        }

        /* Go to the next one */
        val >>= 4;
        i++;
    }
    /* Set the NULL terminating byte */
    pBuf[8] = '\0';

    /* Write the hexa number as string */
    rv = gfmLog_logString(pCtx, pBuf, 8);
    ASSERT(rv == GFMRV_OK, rv);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Log the current time to the file
 * 
 * @param  pCtx The logger
 * @return      GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_LOG_NOT_INITIALIZED,
 *              GFMRV_INTERNAL_ERROR
 */
static gfmRV gfmLog_logTime(gfmLog *pCtx) {
	//char *_ctime;
    gfmRV rv;
    //int len;
    struct tm *_pTm;
	time_t _time, ret;

    // Sanitize arguments
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    // Check that the logger was initialized
#ifndef EMCC
    ASSERT(pCtx->pFile, GFMRV_LOG_NOT_INITIALIZED);
#endif

	// Get current time
	ret = time(&_time);
    ASSERT(ret != ((time_t) -1), GFMRV_INTERNAL_ERROR);
    _pTm = localtime(&_time);
    ASSERT(_pTm != 0, GFMRV_INTERNAL_ERROR);

#define LOG_INT(var) \
    if (var < 10) { \
        /* Make sure the integer is 2 digits long */ \
        rv = gfmLog_logString(pCtx, "0", 1); \
        ASSERT(rv == GFMRV_OK, rv); \
    } \
    rv = gfmLog_logInt(pCtx, var)

    // Log the time
    LOG_INT(_pTm->tm_year + 1900);
    ASSERT(rv == GFMRV_OK, rv);
    rv = gfmLog_logString(pCtx, "/", 1);
    ASSERT(rv == GFMRV_OK, rv);
    LOG_INT(_pTm->tm_mon);
    ASSERT(rv == GFMRV_OK, rv);
    rv = gfmLog_logString(pCtx, "/", 1);
    ASSERT(rv == GFMRV_OK, rv);
    LOG_INT(_pTm->tm_mday);
    ASSERT(rv == GFMRV_OK, rv);
    rv = gfmLog_logString(pCtx, " ", 1);
    ASSERT(rv == GFMRV_OK, rv);
    LOG_INT(_pTm->tm_hour);
    ASSERT(rv == GFMRV_OK, rv);
    rv = gfmLog_logString(pCtx, ":", 1);
    ASSERT(rv == GFMRV_OK, rv);
    LOG_INT(_pTm->tm_min);
    ASSERT(rv == GFMRV_OK, rv);
    rv = gfmLog_logString(pCtx, ":", 1);
    ASSERT(rv == GFMRV_OK, rv);
    LOG_INT(_pTm->tm_sec);
    ASSERT(rv == GFMRV_OK, rv);
    rv = gfmLog_logString(pCtx, " ", 1);
    ASSERT(rv == GFMRV_OK, rv);

    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Log a message; The current time will be printed prior to the message
 * 
 * @param  pCtx  The logger
 * @param  level This message's logging level
 * @param  pFmt  The message's format (similar to printf's)
 * @param  ...   The message's arguments (similar to printf's)
 * @return       GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_LOG_NOT_INITIALIZED
 */
gfmRV gfmLog_simpleLog(gfmLog *pCtx, gfmLogLevel level, char *pFmt, ...) {
    gfmLogParser parser;
    gfmRV rv;
    int i, len, strIni, strLen;
	va_list args;
    
    // Sanitize arguments
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    // Check that the logger was initialized
#ifndef EMCC
    ASSERT(pCtx->pFile, GFMRV_LOG_NOT_INITIALIZED);
#endif
    // Check that the message can be logged
    if (level < pCtx->minLevel) {
        rv = GFMRV_OK;
        goto __ret;
    }
    
    // Log Time
    rv = gfmLog_logTime(pCtx);
    ASSERT(rv == GFMRV_OK, rv);
    
	va_start(args, pFmt);
    
    // Log what was sent
    strIni = -1;
    strLen = 0;
    i = 0;
    parser = gfmLogParser_waiting;
    len = strlen(pFmt);
    while (i < len) {
        char c;
        
        c = pFmt[i];
        switch (parser) {
            case gfmLogParser_waiting: {
                if (c == '%') {
                    // After a '%' comes the options, so flag it!
                    parser = gfmLogParser_getOptions;
                    // Reset the length
                    strLen = -1;
                    // If there was a string before the '%', print it
                    if (strIni != -1) {
                        rv = gfmLog_logString(pCtx, pFmt + strIni, i - strIni);
                        ASSERT(rv == GFMRV_OK, rv);
                    }
                    strIni = -1;
                }
                else if (strIni == -1) {
                    // Retrive a static string's first char
                    strIni = i;
                }
            } break;
            case gfmLogParser_getOptions: {
                switch (c) {
                    case '*': {
                        // Retrieve the string's max size from the variadic args
                        strLen = va_arg(args, int);
                    } break;
                    // TODO Get other formatting options
                    default: {
                        // Since all formatting options were parsed, go to the
                        // next state
                        parser = gfmLogParser_getType;
                        continue;
                    }
                }
            } break;
            case gfmLogParser_getType: {
                switch (c) {
                    case 'c': {
                        // TODO Retrieve a character from the variadic args
                        // TODO Print it to the log
                    } break;
                    case 'i':
                    case 'd': {
                        int val;
                        
                        // Retrieve a integer from the variadic args
                        val = va_arg(args, int);
                        // Print it to the log
                        rv = gfmLog_logInt(pCtx, val);
                        ASSERT(rv == GFMRV_OK, rv);
                    } break;
                    case 'X':
                    case 'x': {
                        int val;
                        
                        // Retrieve a integer from the variadic args
                        val = va_arg(args, int);
                        // Print it to the log
                        rv = gfmLog_logHex(pCtx, val, c == 'X');
                        ASSERT(rv == GFMRV_OK, rv);
                    } break;
                    case 's': {
                        char *pStr;
                        int localStrLen;
                        
                        // Retrieve a string from the variadic args
                        pStr = va_arg(args, char*);
                        // Get its length
                        if (strLen > 0) {
                            localStrLen = 1;
                            while (localStrLen < strLen &&
                                    pStr[localStrLen] != '\0') {
                                localStrLen++;
                            }
                        }
                        else {
                            localStrLen = strlen(pStr);
                        }
                        // Print it to the log
                        rv = gfmLog_logString(pCtx, pStr, localStrLen);
                        ASSERT(rv == GFMRV_OK, rv);
                    } break;
                    // TODO Parse other types
                    default: ASSERT(0, GFMRV_LOG_UNKNOWN_TOKEN);
                }
                // Go back to the previous state
                parser = gfmLogParser_waiting;
            } break;
            default: ASSERT(0, GFMRV_LOG_UNKNOWN_TOKEN);
        }
        
        i++;
    }
    // If there's a string that wasn't printed
    if (strIni != -1) {
        rv = gfmLog_logString(pCtx, pFmt + strIni, len - strIni);
        ASSERT(rv == GFMRV_OK, rv);
    }
    
#ifndef EMCC
    rv = gfmFile_flush(pCtx->pFile);
#endif
    ASSERT(rv == GFMRV_OK, rv);
    
	va_end(args);
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

