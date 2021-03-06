/**
 * @file src/util/gfmAudio_wave.c
 * 
 * Module to parse a WAVE audio file
 */
#include <GFraMe/gfmAssert.h>
#include <GFraMe/gfmError.h>
#include <GFraMe/gfmLog.h>
#include <GFraMe/core/gfmFile_bkend.h>
#include <GFraMe_int/gfmAudio_wave.h>

#include <stdlib.h>
#include <string.h>

/** Format of the waveform */
enum enWAVEFormatTag {
    /** PCM */
    WAVE_FORMAT_PCM        = 0x0001,
    /** IEEE float */
    WAVE_FORMAT_IEEE_FLOAT = 0x0003,
    /** 8-bit ITU-T G.711 A-law */
    WAVE_FORMAT_ALAW       = 0x0006,
    /** 8-bit ITU-T G.711 µ-law */
    WAVE_FORMAT_MULAW      = 0x0007,
    /** Determined by SubFormat */
    WAVE_FORMAT_EXTENSIBLE = 0xFFFE
};
typedef enum enWAVEFormatTag waveFormatTag;

/** The format of the stored wave file */
struct stWAVEFormat {
    waveFormatTag format;
    /** Number of interleaved channels */
    int numChannels;
    /** Sample's frequency (in Hertz) */
    int sampleRate;
    /** Frequency in bytes per second */
    int byteRate;
    /** How many bytes are needed per sample (in case the bits-per-samples is
     *  misaligned) */
    int bytesPerSample;
    /** How many bits are needed per sample */
    int bitsPerSample;
    /** Ratio between samples in the source and destination */
    int downsampleRate;
};
typedef struct stWAVEFormat waveFormat;

/** RIFF files are comprised of chunks/sub chunks with this 'format' */
struct stRIFFChunk {
    /** Chunk ID (always 4 bytes + '\0') */
    char pId[5];
    /** Chunk size (4 bytes integer) */
    int size;
    /** Chunk data (of size bytes) */
    char *pData;
};
typedef struct stRIFFChunk riffChunk;

/******************************************************************************/
/*                                                                            */
/* Static functions                                                           */
/*                                                                            */
/******************************************************************************/

/**
 * Converts a little-endian buffer to an '16-bits integer'
 */
#define gfmAudio_getHalfWordLE(pBuf) \
    (int)(    ( ((pBuf)[0]      ) & 0x000000ff) \
            | ( ((pBuf)[1] << 8 ) & 0x0000ff00) )

/**
 * Converts a little-endian buffer to an '32-bits integer'
 */
#define gfmAudio_getWordLE(pBuf) \
    (int)(    ( ((pBuf)[0]      ) & 0x000000ff) \
            | ( ((pBuf)[1] << 8 ) & 0x0000ff00) \
            | ( ((pBuf)[2] << 16) & 0x00ff0000) \
            | ( ((pBuf)[3] << 24) & 0xff000000) )

/**
 * Read a chunk's id and size from a file
 * 
 * @param  pCtx The chunk read
 * @param  pFp  The file
 * @return      GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_READ_ERROR
 */
static gfmRV gfmAudio_readRIFFChunkHeader(riffChunk *pCtx, gfmFile *pFp) {
    gfmRV rv;
    int count;
    
    // Sanitize arguments
    ASSERT(pCtx, GFMRV_ARGUMENTS_BAD);
    ASSERT(pFp, GFMRV_ARGUMENTS_BAD);
    
    // Read the chunk's ID (4 bytes)
    rv =  gfmFile_readBytes(pCtx->pId, &count, pFp, 4/*numBytes*/);
    ASSERT_NR(rv == GFMRV_OK);
    ASSERT(count == 4, GFMRV_READ_ERROR);
    // Set the string's end
    pCtx->pId[4] = '\0';
    
    // Read the chunk's size (4 bytes)
    rv = gfmFile_readWord(&(pCtx->size), pFp);
    ASSERT_NR(rv == GFMRV_OK);
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Read the wave's first chunk, check that it's valid and get its size
 * 
 * @param  pSize The 'master' chunk size (minux the 4 'file type' bytes)
 * @param  pFp   The file pointer
 * @return       GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_READ_ERROR,
 *               GFMRV_FUNCTION_FAILED
 */
static gfmRV gfmAudio_readMasterChunk(int *pSize, gfmFile *pFp) {
    char pBuf[4];
    gfmRV rv;
    int count;
    riffChunk chunk;
    
    // Sanitize arguments
    ASSERT(pFp, GFMRV_ARGUMENTS_BAD);
    ASSERT(pSize, GFMRV_ARGUMENTS_BAD);
    
    // Read the first chunk (it's ID must be RIFF)
    rv = gfmAudio_readRIFFChunkHeader(&chunk, pFp);
    ASSERT_NR(rv == GFMRV_OK);
    // Check the ID
    ASSERT(chunk.pId[0] == 'R' && chunk.pId[1] == 'I' && chunk.pId[2] == 'F' &&
            chunk.pId[3] == 'F', GFMRV_FUNCTION_FAILED);
    // Check that there are at least 4 more byte (that must contain "WAVE")
    ASSERT(chunk.size >= 4, GFMRV_FUNCTION_FAILED);
    // Read those next 4 bytes
    rv = gfmFile_readBytes(pBuf, &count, pFp, 4/*numBytes*/);
    ASSERT_NR(rv == GFMRV_OK);
    ASSERT(count == 4, GFMRV_READ_ERROR);
    // Check that those bytes actually read "WAVE"
    ASSERT(pBuf[0] == 'W' && pBuf[1] == 'A' && pBuf[2] == 'V' && pBuf[3] == 'E',
            GFMRV_FUNCTION_FAILED);
    
    // Return the retrieved size (after removing 4 bytes)
    *pSize = chunk.size - 4;
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Read a few samples from an input buffer and downsample it; It expects the
 * input to have at most two channels
 * 
 * @param  pSamples The downsampled sample(s)
 * @param  pBuf     The input buffer
 * @param  pFormat  The input format
 * @return          GFMRV_OK, GFMRV_ARGUMENTS_BAD
 */
static gfmRV gfmAudio_downsampleWave(int pSamples[2], char *pBuf,
        waveFormat *pFormat) {
    gfmRV rv;
    int i;
    
    // Sanitize arguments
    ASSERT(pSamples, GFMRV_ARGUMENTS_BAD);
    ASSERT(pBuf, GFMRV_ARGUMENTS_BAD);
    ASSERT(pFormat, GFMRV_ARGUMENTS_BAD);
    
    // Clean the output
    pSamples[0] = 0;
    pSamples[1] = 0;
    
    // Accumulate the samples (according to bitsPerSample and input channels)
    i = 0;
    if (pFormat->bitsPerSample == 8 && pFormat->numChannels == 1) {
        while (i < pFormat->downsampleRate) {
            pSamples[0] += (int)pBuf[i];
            // Repeat the channel, in case output is stereo
            pSamples[1] += (int)pBuf[i];
            i += pFormat->bytesPerSample;
        }
    }
    else if (pFormat->bitsPerSample == 8 && pFormat->numChannels == 2) {
        while (i < pFormat->downsampleRate) {
            pSamples[0] += (int)pBuf[i];
            pSamples[1] += (int)pBuf[i + 1];
            i += pFormat->bytesPerSample;
        }
    }
    else if (pFormat->bitsPerSample == 16 && pFormat->numChannels == 1) {
        while (i < pFormat->downsampleRate) {
            int tmp;
            
            tmp = gfmAudio_getHalfWordLE(pBuf + i);
            // If the input was negative (it's a 2-complement number), fix it
            if (tmp & 0x7000) {
                tmp |= 0xffff0000;
            }
            
            pSamples[0] += tmp;
            // Repeat the channel, in case output is stereo
            pSamples[1] += tmp;
            
            i += pFormat->bytesPerSample;
        }
    }
    else if (pFormat->bitsPerSample == 16 && pFormat->numChannels == 2) {
        while (i < pFormat->downsampleRate) {
            int tmp;
            
            tmp = gfmAudio_getHalfWordLE(pBuf + i);
            // If the input was negative (it's a 2-complement number), fix it
            if (tmp & 0x8000) {
                tmp |= 0xffff0000;
            }
            pSamples[0] += tmp;
            
            tmp = gfmAudio_getHalfWordLE(pBuf + i + 2);
            // If the input was negative (it's a 2-complement number), fix it
            if (tmp & 0x8000) {
                tmp |= 0xffff0000;
            }
            pSamples[1] += tmp;
            
            i += pFormat->bytesPerSample;
        }
    }
    
    // Normalize the downsampled data
    pSamples[0] /= pFormat->downsampleRate;
    pSamples[1] /= pFormat->downsampleRate;
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Get some samples in a 'bit rate' and convert it to the output 'bit rate'
 * 
 * @param  pDst          The destination buffer (still as integers)
 * @param  pSrc          The source buffer
 * @param  pFormat       Format of the input buffer
 * @param  bitsPerSample Desired bits per sample
 */
static gfmRV gfmAudio_convertWaveBits(int pDst[2], int pSrc[2],
        waveFormat *pFormat, int bitsPerSample) {
    gfmRV rv;
    
    // Sanitize arguments
    ASSERT(pDst, GFMRV_ARGUMENTS_BAD);
    ASSERT(pSrc, GFMRV_ARGUMENTS_BAD);
    ASSERT(pFormat, GFMRV_ARGUMENTS_BAD);
    ASSERT(bitsPerSample == 8 || bitsPerSample == 16, GFMRV_ARGUMENTS_BAD);
    
    if (pFormat->bitsPerSample == bitsPerSample) {
        // If the sample rate is the same, just return it
        pDst[0] = pSrc[0];
        pDst[1] = pSrc[1];
    }
    else if (pFormat->bitsPerSample == 8 && bitsPerSample == 16) {
        // Otherwise, expand its range and normalize it
        pDst[0] = (pSrc[0] << 8) - 0x8000;
        pDst[1] = (pSrc[1] << 8) - 0x8000;
    }
    else if (pFormat->bitsPerSample == 16 && bitsPerSample == 8) {
        // Make the range shorter
        pDst[0] = (pSrc[0] + 0x8000) >> 8;
        pDst[1] = (pSrc[1] + 0x8000) >> 8;
    }
    else {
        // Shouldn't happen, but avoids compiler warnings
        return GFMRV_FUNCTION_FAILED;
    }
    
    rv = GFMRV_OK;
__ret:
    return rv;
}

/**
 * Read the format of the wave data
 * 
 * @param  pFormat The retrieved format
 * @param  pFp     The file pointer
 * @param  size    How many bytes there are in the format
 * @return         GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_ALLOC_FAILED,
 *                 GFMRV_READ_ERROR
 */
static gfmRV gfmAudio_readWaveFormat(waveFormat *pFormat, gfmFile *pFp, int size) {
    char *pBuf;
    gfmRV rv;
    int count;
    
    // Set default values
    pBuf = 0;
    
    // Sanitize arguments
    ASSERT(pFormat, GFMRV_ARGUMENTS_BAD);
    ASSERT(pFp, GFMRV_ARGUMENTS_BAD);
    // Those are the only possible sizes for the data format (it's on the docs)
    ASSERT(size == 16 || size == 18 || size == 40, GFMRV_ARGUMENTS_BAD);
    
    // Alloc enough bytes to read the message
    pBuf = (char*)malloc(sizeof(char)*size);
    ASSERT(pBuf, GFMRV_ALLOC_FAILED);
    
    // Read the data format from the file
    rv = gfmFile_readBytes(pBuf, &count, pFp, size);
    ASSERT_NR(rv == GFMRV_OK);
    ASSERT(count == size, GFMRV_READ_ERROR);
    
    // Read from the buffer into a format struct (so it can be returned)
    pFormat->format         = gfmAudio_getHalfWordLE(pBuf + 0);
    pFormat->numChannels    = gfmAudio_getHalfWordLE(pBuf + 2);
    pFormat->sampleRate     = gfmAudio_getWordLE(pBuf + 4);
    pFormat->byteRate       = gfmAudio_getWordLE(pBuf + 8);
    pFormat->bytesPerSample = gfmAudio_getHalfWordLE(pBuf + 12);
    pFormat->bitsPerSample  = gfmAudio_getHalfWordLE(pBuf + 14);
    
    rv = GFMRV_OK;
__ret:
    if (pBuf) {
        free(pBuf);
    }
    
    return rv;
}

/******************************************************************************/
/*                                                                            */
/* Public functions                                                           */
/*                                                                            */
/******************************************************************************/

/**
 * Check if an audio file is encoded as WAVE
 * 
 * @param  pFp The file pointer
 * @return     GFMRV_TRUE, GFMRV_FALSE, GFMRV_ARGUMENTS_BAD, GFMRV_READ_ERROR
 */
gfmRV gfmAudio_isWave(gfmFile *pFp) {
    gfmRV rv;
    int size;
    
    // Sanitize arguments
    ASSERT(pFp, GFMRV_ARGUMENTS_BAD);
    // Rewind the file
    rv = gfmFile_rewind(pFp);
    ASSERT_NR(rv == GFMRV_OK);
    
    // Try to read the master chunk (will succeed if it's an wave file)
    rv = gfmAudio_readMasterChunk(&size, pFp);
    ASSERT(rv == GFMRV_OK, GFMRV_FALSE);
    
    rv = GFMRV_TRUE;
__ret:
    return rv;
}

/**
 * Loads a wave audio into a buffer
 * 
 * @param  ppBuf          Output buffer, must be cleared by the caller
 * @param  pLen           Size of the output buffer, in bytes
 * @param  pFp            The audio file
 * @param  freq           Audio sub-system's frequency/sampling rate
 * @param  bitsPerSamples Audio sub-system's bit per samples
 * @param  numChannels    Audio sub-system's number of channels
 * @return                GFMRV_OK, GFMRV_ARGUMENTS_BAD, GFMRV_READ_ERROR,
 *                        GFMRV_FUNCTION_FAILED, GFMRV_AUDIO_FILE_NOT_SUPPORTED,
 *                        GFMRV_ALLOC_FAILED
 */
gfmRV gfmAudio_loadWave(char **ppBuf, int *pLen, gfmFile *pFp, gfmLog *pLog,
        int freq, int bitsPerSample, int numChannels) {
    char *pBuf, *pDst;
    gfmRV rv;
    int bufLen, dstLen, irv, size;
    waveFormat format;
    
    // Set default values
    bufLen = 0;
    dstLen = 0;
    pBuf = 0;
    pDst = 0;
    
    // Sanitize arguments
    ASSERT_LOG(pFp, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(ppBuf, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(!(*ppBuf), GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(pLen, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(freq > 0, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(bitsPerSample == 8 || bitsPerSample == 16, GFMRV_ARGUMENTS_BAD, pLog);
    ASSERT_LOG(numChannels > 0, GFMRV_ARGUMENTS_BAD, pLog);
    
    // Rewind the file
    rv = gfmFile_rewind(pFp);
    ASSERT_LOG(rv == GFMRV_OK, rv, pLog);
    
    // Read the master chunk and retrieve its size
    rv = gfmAudio_readMasterChunk(&size, pFp);
    ASSERT_LOG(rv == GFMRV_OK, rv, pLog);
    
    rv = gfmLog_log(pLog, gfmLog_info, "File size: %i bytes", size);
    ASSERT(rv == GFMRV_OK, rv);
    
    // Clear the format, before it's read
    memset(&format, 0x0, sizeof(waveFormat));
    
    // Finish reading the file
    while (size > 0) {
        riffChunk chunk;
        
        // Read the next chunk
        rv = gfmAudio_readRIFFChunkHeader(&chunk, pFp);
        ASSERT_LOG(rv == GFMRV_OK, rv, pLog);
        // The chunk header is 8 bytes long, so...
        size -= 8;
        
        // Check which chunk was read...
        if (strcmp(chunk.pId, "fmt ") == 0) {
            
            rv = gfmLog_log(pLog, gfmLog_info, "Got a 'fmt ' chunk");
            ASSERT(rv == GFMRV_OK, rv);
            
            // Read the file format
            rv = gfmAudio_readWaveFormat(&format, pFp, chunk.size);
            ASSERT_LOG(rv == GFMRV_OK, rv, pLog);
            
            rv = gfmLog_log(pLog, gfmLog_info, "Audio sample rate: %i",
                    format.sampleRate);
            ASSERT(rv == GFMRV_OK, rv);
            rv = gfmLog_log(pLog, gfmLog_info, "Audio bits per sample: %i",
                    format.bitsPerSample);
            ASSERT(rv == GFMRV_OK, rv);
            rv = gfmLog_log(pLog, gfmLog_info, "Audio number of channels: %i",
                    format.numChannels);
            ASSERT(rv == GFMRV_OK, rv);
            
            // Check that the format is valid
            // Check that the sample rate is valid (and easy to work with)
            ASSERT_LOG(format.sampleRate == 11025 || format.sampleRate == 22050 ||
                    format.sampleRate == 44100 || format.sampleRate == 88200,
                    GFMRV_AUDIO_FILE_NOT_SUPPORTED, pLog);
            // The file must have at least the same sample rate as the audio
            // device (so audios are only downsampled, and no noise is added)
            ASSERT_LOG(format.sampleRate >= freq, GFMRV_AUDIO_FILE_NOT_SUPPORTED, pLog);
            // This is quite easy to convert, so both are supported
            ASSERT_LOG(format.bitsPerSample == 8 || format.bitsPerSample == 16,
                    GFMRV_AUDIO_FILE_NOT_SUPPORTED, pLog);
            // I'll be lazy an only support those for now...
            ASSERT_LOG(format.numChannels == 1 || format.numChannels == 2,
                    GFMRV_AUDIO_FILE_NOT_SUPPORTED, pLog);
            
            // Calculate the downsample rate
            format.downsampleRate = format.sampleRate / freq;
            
            rv = gfmLog_log(pLog, gfmLog_info, "Downsample rate: %i",
                    format.downsampleRate);
            ASSERT(rv == GFMRV_OK, rv);
        }
        else if (strcmp(chunk.pId, "LIST") == 0) {
            rv = gfmLog_log(pLog, gfmLog_info, "Got a 'LIST' chunk");
            ASSERT(rv == GFMRV_OK, rv);
            
            // This type of chunk may be ignored
            rv = gfmFile_seek(pFp, chunk.size);
            ASSERT_LOG(rv == GFMRV_OK, rv, pLog);
        }
        else if (strcmp(chunk.pId, "data") == 0) {
            int i, j, len;
            
            rv = gfmLog_log(pLog, gfmLog_info, "Got a 'data' chunk");
            ASSERT(rv == GFMRV_OK, rv);
            
            // Check that the format was already gotten
            ASSERT_LOG(format.sampleRate != 0, GFMRV_READ_ERROR, pLog);
            
            // Check that the input buffer is big enough to hold the data
            if (bufLen < chunk.size) {
                // Enpand it as necessary
                pBuf = (char*)realloc(pBuf, sizeof(char)*chunk.size);
                ASSERT_LOG(pBuf, GFMRV_ALLOC_FAILED, pLog);
                bufLen = chunk.size;
            }
            
            // Read the bytes
            rv = gfmFile_readBytes(pBuf, &irv, pFp, chunk.size);
            ASSERT_LOG(rv == GFMRV_OK, rv, pLog);
            ASSERT_LOG(irv == chunk.size, GFMRV_READ_ERROR, pLog);
            
            // Calculate how many more bytes are required on the destination
            // Get how many samples were read from the source
            len = chunk.size / format.bytesPerSample;
            // Downsample it as necessary
            len /= format.downsampleRate;
            // Get the length, in bytes, per channel
            len *= bitsPerSample / 8;
            // Get the total length
            len *= numChannels;
            
            // Expand the destination buffer
            pDst = (char*)realloc(pDst, sizeof(char) * len);
            ASSERT_LOG(pDst, GFMRV_ALLOC_FAILED, pLog);
            
            // Convert it to the desired format
            i = 0;
            j = 0;
            while (i < chunk.size) {
                int pSamples[2], pDstSamples[2];
                
                // Downsample the wave to the desired sample rate
                rv = gfmAudio_downsampleWave(pSamples, pBuf + i, &format);
                ASSERT_LOG(rv == GFMRV_OK, rv, pLog);
                // Convert it to the desired 'bit rate'
                rv = gfmAudio_convertWaveBits(pDstSamples, pSamples, &format,
                        bitsPerSample);
                ASSERT_LOG(rv == GFMRV_OK, rv, pLog);
                
                // Output it to the buffer
                pDst[j] = pDstSamples[0] & 0xff;
                j++;
                if (bitsPerSample == 16) {
                    pDst[j] = (pDstSamples[0] >> 8 ) & 0xff;
                    j++;
                }
                if (numChannels == 2) {
                    pDst[j] = pDstSamples[1] & 0xff;
                    j++;
                    if (bitsPerSample == 16) {
                        pDst[j] = (pDstSamples[1] >> 8 ) & 0xff;
                        j++;
                    }
                }
                
                // Go to the next sample
                i += format.bytesPerSample * format.downsampleRate;
            }
            // Store how many bytes were read
            dstLen += len;
        }
        else {
            if (size > 8) {
                // Got an invalid chunk at the middle of the file
                ASSERT_LOG(0, GFMRV_READ_ERROR, pLog);
            }
            else {
                // It was after the last chunk, so...
                // TODO Check why this happens
                break;
            }
        }
        
        // Update how many bytes were read
        size -= chunk.size;
    }
    
    // Set the return values
    *ppBuf = pDst;
    *pLen = dstLen;
    rv = GFMRV_OK;
__ret:
    if (rv != GFMRV_OK && pDst)
        free(pDst);
    if (pBuf)
        free(pBuf);
    
    return rv;
}

