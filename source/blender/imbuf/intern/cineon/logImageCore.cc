/* SPDX-FileCopyrightText: 1999-2001 David Hodson <hodsond@acm.org>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbcineon
 *
 * Cineon image file format library routines.
 */

#include "logImageCore.h"
#include "cineonlib.h"
#include "dpxlib.h"
#include "logmemfile.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "BLI_fileops.h"
#include "BLI_utildefines.h"

#include "IMB_imbuf.hh"

#include "MEM_guardedalloc.h"

/*
 * Declaration of static functions
 */

static int logImageSetData8(LogImageFile *logImage,
                            const LogImageElement &logElement,
                            const float *data);
static int logImageSetData10(LogImageFile *logImage,
                             const LogImageElement &logElement,
                             const float *data);
static int logImageSetData12(LogImageFile *logImage,
                             const LogImageElement &logElement,
                             const float *data);
static int logImageSetData16(LogImageFile *logImage,
                             const LogImageElement &logElement,
                             const float *data);
static int logImageElementGetData(LogImageFile *logImage,
                                  const LogImageElement &logElement,
                                  float *data);
static int logImageElementGetData1(LogImageFile *logImage,
                                   const LogImageElement &logElement,
                                   float *data);
static int logImageElementGetData8(LogImageFile *logImage,
                                   const LogImageElement &logElement,
                                   float *data);
static int logImageElementGetData10(LogImageFile *logImage,
                                    const LogImageElement &logElement,
                                    float *data);
static int logImageElementGetData10Packed(LogImageFile *logImage,
                                          const LogImageElement &logElement,
                                          float *data);
static int logImageElementGetData12(LogImageFile *logImage,
                                    const LogImageElement &logElement,
                                    float *data);
static int logImageElementGetData12Packed(LogImageFile *logImage,
                                          const LogImageElement &logElement,
                                          float *data);
static int logImageElementGetData16(LogImageFile *logImage,
                                    const LogImageElement &logElement,
                                    float *data);
static int convertLogElementToRGBA(const float *src,
                                   float *dst,
                                   const LogImageFile *logImage,
                                   const LogImageElement &logElement,
                                   int dstIsLinearRGB);
static int convertRGBAToLogElement(const float *src,
                                   float *dst,
                                   const LogImageFile *logImage,
                                   const LogImageElement &logElement,
                                   int srcIsLinearRGB);

/*
 * For debug purpose
 */

static int verbose = 0;

void logImageSetVerbose(int verbosity)
{
  verbose = verbosity;
  cineonSetVerbose(verbosity);
  dpxSetVerbose(verbosity);
}

/*
 * IO stuff
 */

int logImageIsDpx(const void *buffer, const uint size)
{
  uint magicNum;
  if (size < sizeof(magicNum)) {
    return 0;
  }
  magicNum = *(uint *)buffer;
  return (magicNum == DPX_FILE_MAGIC || magicNum == swap_uint(DPX_FILE_MAGIC, 1));
}

int logImageIsCineon(const void *buffer, const uint size)
{
  uint magicNum;
  if (size < sizeof(magicNum)) {
    return 0;
  }
  magicNum = *(uint *)buffer;
  return (magicNum == CINEON_FILE_MAGIC || magicNum == swap_uint(CINEON_FILE_MAGIC, 1));
}

LogImageFile *logImageOpenFromFile(const char *filepath, int cineon)
{
  uint magicNum;
  FILE *f = BLI_fopen(filepath, "rb");

  (void)cineon;

  if (f == nullptr) {
    return nullptr;
  }

  if (fread(&magicNum, sizeof(magicNum), 1, f) != 1) {
    fclose(f);
    return nullptr;
  }

  fclose(f);

  if (logImageIsDpx(&magicNum, sizeof(magicNum))) {
    return dpxOpen((const uchar *)filepath, 0, 0);
  }
  if (logImageIsCineon(&magicNum, sizeof(magicNum))) {
    return cineonOpen((const uchar *)filepath, 0, 0);
  }

  return nullptr;
}

LogImageFile *logImageOpenFromMemory(const uchar *buffer, uint size)
{
  if (logImageIsDpx(buffer, size)) {
    return dpxOpen(buffer, 1, size);
  }
  if (logImageIsCineon(buffer, size)) {
    return cineonOpen(buffer, 1, size);
  }

  return nullptr;
}

LogImageFile *logImageCreate(const char *filepath,
                             int cineon,
                             int width,
                             int height,
                             int bitsPerSample,
                             int isLogarithmic,
                             int hasAlpha,
                             int referenceWhite,
                             int referenceBlack,
                             float gamma,
                             const char *creator)
{
  /* referenceWhite, referenceBlack and gamma values are only supported for DPX file */
  if (cineon) {
    return cineonCreate(filepath, width, height, bitsPerSample, creator);
  }

  return dpxCreate(filepath,
                   width,
                   height,
                   bitsPerSample,
                   isLogarithmic,
                   hasAlpha,
                   referenceWhite,
                   referenceBlack,
                   gamma,
                   creator);
}

void logImageClose(LogImageFile *logImage)
{
  if (logImage != nullptr) {
    if (logImage->file) {
      fclose(logImage->file);
      logImage->file = nullptr;
    }
    MEM_freeN(logImage);
  }
}

void logImageGetSize(const LogImageFile *logImage, int *width, int *height, int *depth)
{
  *width = logImage->width;
  *height = logImage->height;
  *depth = logImage->depth;
}

/*
 * Helper
 */

static size_t getRowLength(size_t width, const LogImageElement &logElement)
{
  /* return the row length in bytes according to width and packing method */
  switch (logElement.bitsPerSample) {
    case 1:
      return ((width * logElement.depth - 1) / 32 + 1) * 4;

    case 8:
      return ((width * logElement.depth - 1) / 4 + 1) * 4;

    case 10:
      if (logElement.packing == 0) {
        return ((width * logElement.depth * 10 - 1) / 32 + 1) * 4;
      }
      else if (ELEM(logElement.packing, 1, 2)) {
        return ((width * logElement.depth - 1) / 3 + 1) * 4;
      }
      break;
    case 12:
      if (logElement.packing == 0) {
        return ((width * logElement.depth * 12 - 1) / 32 + 1) * 4;
      }
      else if (ELEM(logElement.packing, 1, 2)) {
        return width * logElement.depth * 2;
      }
      break;
    case 16:
      return width * logElement.depth * 2;
  }
  return 0;
}

size_t getRowLength(size_t width, const LogImageElement *logElement)
{
  /* For the C-API. */

  return getRowLength(width, *logElement);
}

/*
 * Data writing
 */

int logImageSetDataRGBA(LogImageFile *logImage, const float *data, int dataIsLinearRGB)
{
  float *elementData;
  int returnValue;

  elementData = (float *)imb_alloc_pixels(
      logImage->width, logImage->height, logImage->depth, sizeof(float), true, __func__);
  if (elementData == nullptr) {
    return 1;
  }

  if (convertRGBAToLogElement(
          data, elementData, logImage, logImage->element[0], dataIsLinearRGB) != 0)
  {
    MEM_freeN(elementData);
    return 1;
  }

  switch (logImage->element[0].bitsPerSample) {
    case 8:
      returnValue = logImageSetData8(logImage, logImage->element[0], elementData);
      break;

    case 10:
      returnValue = logImageSetData10(logImage, logImage->element[0], elementData);
      break;

    case 12:
      returnValue = logImageSetData12(logImage, logImage->element[0], elementData);
      break;

    case 16:
      returnValue = logImageSetData16(logImage, logImage->element[0], elementData);
      break;

    default:
      returnValue = 1;
      break;
  }

  MEM_freeN(elementData);
  return returnValue;
}

static int logImageSetData8(LogImageFile *logImage,
                            const LogImageElement &logElement,
                            const float *data)
{
  size_t rowLength = getRowLength(logImage->width, logElement);
  uchar *row;

  row = (uchar *)MEM_mallocN(rowLength, __func__);
  if (row == nullptr) {
    if (verbose) {
      printf("DPX/Cineon: Cannot allocate row.\n");
    }
    return 1;
  }
  memset(row, 0, rowLength);

  for (size_t y = 0; y < logImage->height; y++) {
    for (size_t x = 0; x < logImage->width * logImage->depth; x++) {
      row[x] = uchar(float_uint(data[y * logImage->width * logImage->depth + x], 255));
    }

    if (logimage_fwrite(row, rowLength, 1, logImage) == 0) {
      if (verbose) {
        printf("DPX/Cineon: Error while writing file.\n");
      }
      MEM_freeN(row);
      return 1;
    }
  }
  MEM_freeN(row);
  return 0;
}

static int logImageSetData10(LogImageFile *logImage,
                             const LogImageElement &logElement,
                             const float *data)
{
  size_t rowLength = getRowLength(logImage->width, logElement);
  uint pixel, index;
  uint *row;

  row = (uint *)MEM_mallocN(rowLength, __func__);
  if (row == nullptr) {
    if (verbose) {
      printf("DPX/Cineon: Cannot allocate row.\n");
    }
    return 1;
  }

  for (size_t y = 0; y < logImage->height; y++) {
    int offset = 22;
    index = 0;
    pixel = 0;

    for (size_t x = 0; x < logImage->width * logImage->depth; x++) {
      pixel |= uint(float_uint(data[y * logImage->width * logImage->depth + x], 1023)) << offset;
      offset -= 10;
      if (offset < 0) {
        row[index] = swap_uint(pixel, logImage->isMSB);
        index++;
        pixel = 0;
        offset = 22;
      }
    }
    if (pixel != 0) {
      row[index] = swap_uint(pixel, logImage->isMSB);
    }

    if (logimage_fwrite(row, rowLength, 1, logImage) == 0) {
      if (verbose) {
        printf("DPX/Cineon: Error while writing file.\n");
      }
      MEM_freeN(row);
      return 1;
    }
  }
  MEM_freeN(row);
  return 0;
}

static int logImageSetData12(LogImageFile *logImage,
                             const LogImageElement &logElement,
                             const float *data)
{
  size_t rowLength = getRowLength(logImage->width, logElement);
  ushort *row;

  row = (ushort *)MEM_mallocN(rowLength, __func__);
  if (row == nullptr) {
    if (verbose) {
      printf("DPX/Cineon: Cannot allocate row.\n");
    }
    return 1;
  }

  for (size_t y = 0; y < logImage->height; y++) {
    for (size_t x = 0; x < logImage->width * logImage->depth; x++) {
      row[x] = swap_ushort(
          ushort(float_uint(data[y * logImage->width * logImage->depth + x], 4095)) << 4,
          logImage->isMSB);
    }

    if (logimage_fwrite(row, rowLength, 1, logImage) == 0) {
      if (verbose) {
        printf("DPX/Cineon: Error while writing file.\n");
      }
      MEM_freeN(row);
      return 1;
    }
  }
  MEM_freeN(row);
  return 0;
}

static int logImageSetData16(LogImageFile *logImage,
                             const LogImageElement &logElement,
                             const float *data)
{
  size_t rowLength = getRowLength(logImage->width, logElement);
  ushort *row;

  row = (ushort *)MEM_mallocN(rowLength, __func__);
  if (row == nullptr) {
    if (verbose) {
      printf("DPX/Cineon: Cannot allocate row.\n");
    }
    return 1;
  }

  for (size_t y = 0; y < logImage->height; y++) {
    for (size_t x = 0; x < logImage->width * logImage->depth; x++) {
      row[x] = swap_ushort(
          ushort(float_uint(data[y * logImage->width * logImage->depth + x], 65535)),
          logImage->isMSB);
    }

    if (logimage_fwrite(row, rowLength, 1, logImage) == 0) {
      if (verbose) {
        printf("DPX/Cineon: Error while writing file.\n");
      }
      MEM_freeN(row);
      return 1;
    }
  }
  MEM_freeN(row);
  return 0;
}

/*
 * Data reading
 */

int logImageGetDataRGBA(LogImageFile *logImage, float *data, int dataIsLinearRGB)
{
  /* Fills data with 32 bits float RGBA values */
  int i, j, returnValue, sortedElementData[8], hasAlpha;
  float *elementData[8];
  float *elementData_ptr[8];
  float *mergedData;
  uint sampleIndex;
  LogImageElement mergedElement;

  /* Determine the depth of the picture and if there's a separate alpha element.
   * If the element is supported, load it into an `uint` array. */
  memset(&elementData, 0, 8 * sizeof(float *));
  hasAlpha = 0;

  for (i = 0; i < logImage->numElements; i++) {
    /* descriptor_Depth and descriptor_Composite are not supported */
    if (!ELEM(logImage->element[i].descriptor, descriptor_Depth, descriptor_Composite)) {
      /* Allocate memory */
      elementData[i] = static_cast<float *>(imb_alloc_pixels(logImage->width,
                                                             logImage->height,
                                                             logImage->element[i].depth,
                                                             sizeof(float),
                                                             true,
                                                             __func__));
      if (elementData[i] == nullptr) {
        if (verbose) {
          printf("DPX/Cineon: Cannot allocate memory for elementData[%d]\n.", i);
        }
        for (j = 0; j < i; j++) {
          if (elementData[j] != nullptr) {
            MEM_freeN(elementData[j]);
          }
        }
        return 1;
      }
      elementData_ptr[i] = elementData[i];

      /* Load data */
      if (logImageElementGetData(logImage, logImage->element[i], elementData[i]) != 0) {
        if (verbose) {
          printf("DPX/Cineon: Cannot read elementData[%d]\n.", i);
        }
        for (j = 0; j < i; j++) {
          if (elementData[j] != nullptr) {
            MEM_freeN(elementData[j]);
          }
        }
        return 1;
      }
    }

    if (logImage->element[i].descriptor == descriptor_Alpha) {
      hasAlpha = 1;
    }
  }

  /* Only one element, easy case, no need to do anything. */
  if (logImage->numElements == 1) {
    returnValue = convertLogElementToRGBA(
        elementData[0], data, logImage, logImage->element[0], dataIsLinearRGB);
    MEM_freeN(elementData[0]);
  }
  else {
    /* The goal here is to merge every elements into only one
     * to recreate a classic 16 bits RGB, RGBA or YCbCr element.
     * Unsupported elements are skipped (depth, composite) */

    memcpy(&mergedElement, &logImage->element[0], sizeof(LogImageElement));
    mergedElement.descriptor = -1;
    mergedElement.depth = logImage->depth;
    memset(&sortedElementData, -1, sizeof(int[8]));

    /* Try to know how to assemble the elements */
    for (i = 0; i < logImage->numElements; i++) {
      switch (logImage->element[i].descriptor) {
        case descriptor_Red:
        case descriptor_RGB:
          if (hasAlpha == 0) {
            mergedElement.descriptor = descriptor_RGB;
          }
          else {
            mergedElement.descriptor = descriptor_RGBA;
          }

          sortedElementData[0] = i;
          break;

        case descriptor_Green:
          if (hasAlpha == 0) {
            mergedElement.descriptor = descriptor_RGB;
          }
          else {
            mergedElement.descriptor = descriptor_RGBA;
          }

          sortedElementData[1] = i;
          break;

        case descriptor_Blue:
          if (hasAlpha == 0) {
            mergedElement.descriptor = descriptor_RGB;
          }
          else {
            mergedElement.descriptor = descriptor_RGBA;
          }

          sortedElementData[2] = i;
          break;

        case descriptor_Alpha:
          /* Alpha component is always the last one */
          sortedElementData[mergedElement.depth - 1] = i;
          break;

        case descriptor_Luminance:
          if (mergedElement.descriptor == -1) {
            if (hasAlpha == 0) {
              mergedElement.descriptor = descriptor_Luminance;
            }
            else {
              mergedElement.descriptor = descriptor_YA;
            }
          }
          else if (mergedElement.descriptor == descriptor_Chrominance) {
            if (mergedElement.depth == 2) {
              mergedElement.descriptor = descriptor_CbYCrY;
            }
            else if (mergedElement.depth == 3) {
              if (hasAlpha == 0) {
                mergedElement.descriptor = descriptor_CbYCr;
              }
              else {
                mergedElement.descriptor = descriptor_CbYACrYA;
              }
            }
            else if (mergedElement.depth == 4) {
              mergedElement.descriptor = descriptor_CbYCrA;
            }
          }

          /* Y component always in 1 except if it's alone or with alpha */
          if (mergedElement.depth == 1 || (mergedElement.depth == 2 && hasAlpha == 1)) {
            sortedElementData[0] = i;
          }
          else {
            sortedElementData[1] = i;
          }
          break;

        case descriptor_Chrominance:
          if (mergedElement.descriptor == -1) {
            mergedElement.descriptor = descriptor_Chrominance;
          }
          else if (mergedElement.descriptor == descriptor_Luminance) {
            if (mergedElement.depth == 2) {
              mergedElement.descriptor = descriptor_CbYCrY;
            }
            else if (mergedElement.depth == 3) {
              if (hasAlpha == 0) {
                mergedElement.descriptor = descriptor_CbYCr;
              }
              else {
                mergedElement.descriptor = descriptor_CbYACrYA;
              }
            }
            else if (mergedElement.depth == 4) {
              mergedElement.descriptor = descriptor_CbYCrA;
            }
          }

          /* Cb and Cr always in 0 or 2 */
          if (sortedElementData[0] == -1) {
            sortedElementData[0] = i;
          }
          else {
            sortedElementData[2] = i;
          }
          break;

        case descriptor_CbYCr:
          if (hasAlpha == 0) {
            mergedElement.descriptor = descriptor_CbYCr;
          }
          else {
            mergedElement.descriptor = descriptor_CbYCrA;
          }

          sortedElementData[0] = i;
          break;

        case descriptor_RGBA:
        case descriptor_ABGR:
        case descriptor_CbYACrYA:
        case descriptor_CbYCrY:
        case descriptor_CbYCrA:
          /* I don't think these ones can be seen in a planar image */
          mergedElement.descriptor = logImage->element[i].descriptor;
          sortedElementData[0] = i;
          break;

        case descriptor_Depth:
        case descriptor_Composite:
          /* Not supported */
          break;
      }
    }

    mergedData = (float *)imb_alloc_pixels(
        logImage->width, logImage->height, mergedElement.depth, sizeof(float), true, __func__);
    if (mergedData == nullptr) {
      if (verbose) {
        printf("DPX/Cineon: Cannot allocate mergedData.\n");
      }
      for (i = 0; i < logImage->numElements; i++) {
        if (elementData[i] != nullptr) {
          MEM_freeN(elementData[i]);
        }
      }
      return 1;
    }

    sampleIndex = 0;
    while (sampleIndex < logImage->width * logImage->height * mergedElement.depth) {
      for (i = 0; i < logImage->numElements; i++) {
        for (j = 0; j < logImage->element[sortedElementData[i]].depth; j++) {
          mergedData[sampleIndex++] = *(elementData_ptr[sortedElementData[i]]++);
        }
      }
    }

    /* Done with elements data, clean-up */
    for (i = 0; i < logImage->numElements; i++) {
      if (elementData[i] != nullptr) {
        MEM_freeN(elementData[i]);
      }
    }

    returnValue = convertLogElementToRGBA(
        mergedData, data, logImage, mergedElement, dataIsLinearRGB);
    MEM_freeN(mergedData);
  }
  return returnValue;
}

static int logImageElementGetData(LogImageFile *logImage,
                                  const LogImageElement &logElement,
                                  float *data)
{
  switch (logElement.bitsPerSample) {
    case 1:
      return logImageElementGetData1(logImage, logElement, data);

    case 8:
      return logImageElementGetData8(logImage, logElement, data);

    case 10:
      if (logElement.packing == 0) {
        return logImageElementGetData10Packed(logImage, logElement, data);
      }
      else if (ELEM(logElement.packing, 1, 2)) {
        return logImageElementGetData10(logImage, logElement, data);
      }
      break;

    case 12:
      if (logElement.packing == 0) {
        return logImageElementGetData12Packed(logImage, logElement, data);
      }
      else if (ELEM(logElement.packing, 1, 2)) {
        return logImageElementGetData12(logImage, logElement, data);
      }
      break;

    case 16:
      return logImageElementGetData16(logImage, logElement, data);
  }
  /* format not supported */
  return 1;
}

static int logImageElementGetData1(LogImageFile *logImage,
                                   const LogImageElement &logElement,
                                   float *data)
{
  uint pixel;

  /* seek at the right place */
  if (logimage_fseek(logImage, logElement.dataOffset, SEEK_SET) != 0) {
    if (verbose) {
      printf("DPX/Cineon: Couldn't seek at %d\n", logElement.dataOffset);
    }
    return 1;
  }

  /* read 1 bit data padded to 32 bits */
  for (size_t y = 0; y < logImage->height; y++) {
    for (size_t x = 0; x < logImage->width * logElement.depth; x += 32) {
      if (logimage_read_uint(&pixel, logImage) != 0) {
        if (verbose) {
          printf("DPX/Cineon: EOF reached\n");
        }
        return 1;
      }
      pixel = swap_uint(pixel, logImage->isMSB);
      for (int offset = 0; offset < 32 && x + offset < logImage->width; offset++) {
        data[y * logImage->width * logElement.depth + x + offset] = float((pixel >> offset) &
                                                                          0x01);
      }
    }
  }
  return 0;
}

static int logImageElementGetData8(LogImageFile *logImage,
                                   const LogImageElement &logElement,
                                   float *data)
{
  size_t rowLength = getRowLength(logImage->width, logElement);
  uchar pixel;

  /* extract required pixels */
  for (size_t y = 0; y < logImage->height; y++) {
    /* 8 bits are 32-bits padded so we need to seek at each row */
    if (logimage_fseek(logImage, logElement.dataOffset + y * rowLength, SEEK_SET) != 0) {
      if (verbose) {
        printf("DPX/Cineon: Couldn't seek at %d\n", int(logElement.dataOffset + y * rowLength));
      }
      return 1;
    }

    for (size_t x = 0; x < logImage->width * logElement.depth; x++) {
      if (logimage_read_uchar(&pixel, logImage) != 0) {
        if (verbose) {
          printf("DPX/Cineon: EOF reached\n");
        }
        return 1;
      }
      data[y * logImage->width * logElement.depth + x] = float(pixel) / 255.0f;
    }
  }
  return 0;
}

static int logImageElementGetData10(LogImageFile *logImage,
                                    const LogImageElement &logElement,
                                    float *data)
{
  uint pixel;

  /* seek to data */
  if (logimage_fseek(logImage, logElement.dataOffset, SEEK_SET) != 0) {
    if (verbose) {
      printf("DPX/Cineon: Couldn't seek at %d\n", logElement.dataOffset);
    }
    return 1;
  }

  if (logImage->depth == 1 && logImage->srcFormat == format_DPX) {
    for (size_t y = 0; y < logImage->height; y++) {
      int offset = 32;
      for (size_t x = 0; x < logImage->width * logElement.depth; x++) {
        /* we need to read the next long */
        if (offset >= 30) {
          if (logElement.packing == 1) {
            offset = 2;
          }
          else if (logElement.packing == 2) {
            offset = 0;
          }

          if (logimage_read_uint(&pixel, logImage) != 0) {
            if (verbose) {
              printf("DPX/Cineon: EOF reached\n");
            }
            return 1;
          }
          pixel = swap_uint(pixel, logImage->isMSB);
        }
        data[y * logImage->width * logElement.depth + x] = float((pixel >> offset) & 0x3ff) /
                                                           1023.0f;
        offset += 10;
      }
    }
  }
  else {
    for (size_t y = 0; y < logImage->height; y++) {
      int offset = -1;
      for (size_t x = 0; x < logImage->width * logElement.depth; x++) {
        /* we need to read the next long */
        if (offset < 0) {
          if (logElement.packing == 1) {
            offset = 22;
          }
          else if (logElement.packing == 2) {
            offset = 20;
          }

          if (logimage_read_uint(&pixel, logImage) != 0) {
            if (verbose) {
              printf("DPX/Cineon: EOF reached\n");
            }
            return 1;
          }
          pixel = swap_uint(pixel, logImage->isMSB);
        }
        data[y * logImage->width * logElement.depth + x] = float((pixel >> offset) & 0x3ff) /
                                                           1023.0f;
        offset -= 10;
      }
    }
  }

  return 0;
}

static int logImageElementGetData10Packed(LogImageFile *logImage,
                                          const LogImageElement &logElement,
                                          float *data)
{
  size_t rowLength = getRowLength(logImage->width, logElement);
  uint pixel, oldPixel;

  /* converting bytes to pixels */
  for (size_t y = 0; y < logImage->height; y++) {
    /* seek to data */
    if (logimage_fseek(logImage, y * rowLength + logElement.dataOffset, SEEK_SET) != 0) {
      if (verbose) {
        printf("DPX/Cineon: Couldn't seek at %u\n", uint(y * rowLength + logElement.dataOffset));
      }
      return 1;
    }

    oldPixel = 0;
    int offset = 0;
    int offset2 = 0;

    for (size_t x = 0; x < logImage->width * logElement.depth; x++) {
      if (offset2 != 0) {
        offset = 10 - offset2;
        offset2 = 0;
        oldPixel = 0;
      }
      else if (offset == 32) {
        offset = 0;
      }
      else if (offset + 10 > 32) {
        /* next pixel is on two different longs */
        oldPixel = (pixel >> offset);
        offset2 = 32 - offset;
        offset = 0;
      }

      if (offset == 0) {
        /* we need to read the next long */
        if (logimage_read_uint(&pixel, logImage) != 0) {
          if (verbose) {
            printf("DPX/Cineon: EOF reached\n");
          }
          return 1;
        }
        pixel = swap_uint(pixel, logImage->isMSB);
      }
      data[y * logImage->width * logElement.depth + x] =
          float((((pixel << offset2) >> offset) & 0x3ff) | oldPixel) / 1023.0f;
      offset += 10;
    }
  }
  return 0;
}

static int logImageElementGetData12(LogImageFile *logImage,
                                    const LogImageElement &logElement,
                                    float *data)
{
  uint sampleIndex;
  uint numSamples = logImage->width * logImage->height * logElement.depth;
  ushort pixel;

  /* seek to data */
  if (logimage_fseek(logImage, logElement.dataOffset, SEEK_SET) != 0) {
    if (verbose) {
      printf("DPX/Cineon: Couldn't seek at %d\n", logElement.dataOffset);
    }
    return 1;
  }

  /* convert bytes to pixels */
  sampleIndex = 0;

  for (sampleIndex = 0; sampleIndex < numSamples; sampleIndex++) {
    if (logimage_read_ushort(&pixel, logImage) != 0) {
      if (verbose) {
        printf("DPX/Cineon: EOF reached\n");
      }
      return 1;
    }
    pixel = swap_ushort(pixel, logImage->isMSB);

    if (logElement.packing == 1) { /* padded to the right */
      data[sampleIndex] = float(pixel >> 4) / 4095.0f;
    }
    else if (logElement.packing == 2) { /* padded to the left */
      data[sampleIndex] = float(pixel) / 4095.0f;
    }
  }
  return 0;
}

static int logImageElementGetData12Packed(LogImageFile *logImage,
                                          const LogImageElement &logElement,
                                          float *data)
{
  size_t rowLength = getRowLength(logImage->width, logElement);
  uint pixel, oldPixel;

  /* converting bytes to pixels */
  for (size_t y = 0; y < logImage->height; y++) {
    /* seek to data */
    if (logimage_fseek(logImage, y * rowLength + logElement.dataOffset, SEEK_SET) != 0) {
      if (verbose) {
        printf("DPX/Cineon: Couldn't seek at %u\n", uint(y * rowLength + logElement.dataOffset));
      }
      return 1;
    }

    oldPixel = 0;
    int offset = 0;
    int offset2 = 0;

    for (size_t x = 0; x < logImage->width * logElement.depth; x++) {
      if (offset2 != 0) {
        offset = 12 - offset2;
        offset2 = 0;
        oldPixel = 0;
      }
      else if (offset == 32) {
        offset = 0;
      }
      else if (offset + 12 > 32) {
        /* next pixel is on two different longs */
        oldPixel = (pixel >> offset);
        offset2 = 32 - offset;
        offset = 0;
      }

      if (offset == 0) {
        /* we need to read the next long */
        if (logimage_read_uint(&pixel, logImage) != 0) {
          if (verbose) {
            printf("DPX/Cineon: EOF reached\n");
          }
          return 1;
        }
        pixel = swap_uint(pixel, logImage->isMSB);
      }
      data[y * logImage->width * logElement.depth + x] =
          float((((pixel << offset2) >> offset) & 0xfff) | oldPixel) / 4095.0f;
      offset += 12;
    }
  }
  return 0;
}

static int logImageElementGetData16(LogImageFile *logImage,
                                    const LogImageElement &logElement,
                                    float *data)
{
  uint numSamples = logImage->width * logImage->height * logElement.depth;
  uint sampleIndex;
  ushort pixel;

  /* seek to data */
  if (logimage_fseek(logImage, logElement.dataOffset, SEEK_SET) != 0) {
    if (verbose) {
      printf("DPX/Cineon: Couldn't seek at %d\n", logElement.dataOffset);
    }
    return 1;
  }

  for (sampleIndex = 0; sampleIndex < numSamples; sampleIndex++) {
    if (logimage_read_ushort(&pixel, logImage) != 0) {
      if (verbose) {
        printf("DPX/Cineon: EOF reached\n");
      }
      return 1;
    }
    pixel = swap_ushort(pixel, logImage->isMSB);
    data[sampleIndex] = float(pixel) / 65535.0f;
  }

  return 0;
}

/*
 * Color conversion
 */

static int getYUVtoRGBMatrix(float *matrix, const LogImageElement &logElement)
{
  float scaleY, scaleCbCr;
  float refHighData = float(logElement.refHighData) / logElement.maxValue;
  float refLowData = float(logElement.refLowData) / logElement.maxValue;

  scaleY = 1.0f / (refHighData - refLowData);
  scaleCbCr = scaleY * ((940.0f - 64.0f) / (960.0f - 64.0f));

  switch (logElement.transfer) {
    case 2: /* linear */
      matrix[0] = 1.0f * scaleY;
      matrix[1] = 1.0f * scaleCbCr;
      matrix[2] = 1.0f * scaleCbCr;
      matrix[3] = 1.0f * scaleY;
      matrix[4] = 1.0f * scaleCbCr;
      matrix[5] = 1.0f * scaleCbCr;
      matrix[6] = 1.0f * scaleY;
      matrix[7] = 1.0f * scaleCbCr;
      matrix[8] = 1.0f * scaleCbCr;
      return 0;

    case 5: /* SMPTE 240M */
      matrix[0] = 1.0000f * scaleY;
      matrix[1] = 0.0000f * scaleCbCr;
      matrix[2] = 1.5756f * scaleCbCr;
      matrix[3] = 1.0000f * scaleY;
      matrix[4] = -0.2253f * scaleCbCr;
      matrix[5] = -0.5000f * scaleCbCr;
      matrix[6] = 1.0000f * scaleY;
      matrix[7] = 1.8270f * scaleCbCr;
      matrix[8] = 0.0000f * scaleCbCr;
      return 0;

    case 6: /* CCIR 709-1 */
      matrix[0] = 1.000000f * scaleY;
      matrix[1] = 0.000000f * scaleCbCr;
      matrix[2] = 1.574800f * scaleCbCr;
      matrix[3] = 1.000000f * scaleY;
      matrix[4] = -0.187324f * scaleCbCr;
      matrix[5] = -0.468124f * scaleCbCr;
      matrix[6] = 1.000000f * scaleY;
      matrix[7] = 1.855600f * scaleCbCr;
      matrix[8] = 0.000000f * scaleCbCr;
      return 0;

    case 7: /* CCIR 601 */
    case 8: /* I'm not sure 7 and 8 should share the same matrix */
      matrix[0] = 1.000000f * scaleY;
      matrix[1] = 0.000000f * scaleCbCr;
      matrix[2] = 1.402000f * scaleCbCr;
      matrix[3] = 1.000000f * scaleY;
      matrix[4] = -0.344136f * scaleCbCr;
      matrix[5] = -0.714136f * scaleCbCr;
      matrix[6] = 1.000000f * scaleY;
      matrix[7] = 1.772000f * scaleCbCr;
      matrix[8] = 0.000000f * scaleCbCr;
      return 0;

    default:
      return 1;
  }
}

static float *getLinToLogLut(const LogImageFile *logImage, const LogImageElement &logElement)
{
  float *lut;
  float gain, negativeFilmGamma, offset, step;
  uint lutsize = uint(logElement.maxValue + 1);
  uint i;

  lut = MEM_malloc_arrayN<float>(lutsize, "getLinToLogLut");

  negativeFilmGamma = 0.6;
  step = logElement.refHighQuantity / logElement.maxValue;
  gain = logElement.maxValue /
         (1.0f - powf(10,
                      (logImage->referenceBlack - logImage->referenceWhite) * step /
                          negativeFilmGamma * logImage->gamma / 1.7f));
  offset = gain - logElement.maxValue;

  for (i = 0; i < lutsize; i++) {
    lut[i] = (logImage->referenceWhite +
              log10f(powf((i + offset) / gain, 1.7f / logImage->gamma)) /
                  (step / negativeFilmGamma)) /
             logElement.maxValue;
  }

  return lut;
}

static float *getLogToLinLut(const LogImageFile *logImage, const LogImageElement &logElement)
{
  float *lut;
  float breakPoint, gain, kneeGain, kneeOffset, negativeFilmGamma, offset, step, softClip;
  /* float filmGamma; unused */
  uint lutsize = uint(logElement.maxValue + 1);
  uint i;

  lut = MEM_malloc_arrayN<float>(lutsize, "getLogToLinLut");

  /* Building the Log -> Lin LUT */
  step = logElement.refHighQuantity / logElement.maxValue;
  negativeFilmGamma = 0.6;

  /* these are default values */
  /* filmGamma = 2.2f;  unused */
  softClip = 0;

  breakPoint = logImage->referenceWhite - softClip;
  gain = logElement.maxValue /
         (1.0f - powf(10,
                      (logImage->referenceBlack - logImage->referenceWhite) * step /
                          negativeFilmGamma * logImage->gamma / 1.7f));
  offset = gain - logElement.maxValue;
  kneeOffset = powf(10,
                    (breakPoint - logImage->referenceWhite) * step / negativeFilmGamma *
                        logImage->gamma / 1.7f) *
                   gain -
               offset;
  kneeGain = (logElement.maxValue - kneeOffset) / powf(5 * softClip, softClip / 100);

  for (i = 0; i < lutsize; i++) {
    if (i < logImage->referenceBlack) {
      lut[i] = 0.0f;
    }
    else if (i > breakPoint) {
      lut[i] = (powf(i - breakPoint, softClip / 100) * kneeGain + kneeOffset) /
               logElement.maxValue;
    }
    else {
      lut[i] = (powf(10,
                     (float(i) - logImage->referenceWhite) * step / negativeFilmGamma *
                         logImage->gamma / 1.7f) *
                    gain -
                offset) /
               logElement.maxValue;
    }
  }

  return lut;
}

static float *getLinToSrgbLut(const LogImageElement &logElement)
{
  float col, *lut;
  uint lutsize = uint(logElement.maxValue + 1);
  uint i;

  lut = MEM_malloc_arrayN<float>(lutsize, "getLogToLinLut");

  for (i = 0; i < lutsize; i++) {
    col = float(i) / logElement.maxValue;
    if (col < 0.0031308f) {
      lut[i] = (col < 0.0f) ? 0.0f : col * 12.92f;
    }
    else {
      lut[i] = 1.055f * powf(col, 1.0f / 2.4f) - 0.055f;
    }
  }

  return lut;
}

static float *getSrgbToLinLut(const LogImageElement &logElement)
{
  float col, *lut;
  uint lutsize = uint(logElement.maxValue + 1);
  uint i;

  lut = MEM_malloc_arrayN<float>(lutsize, "getLogToLinLut");

  for (i = 0; i < lutsize; i++) {
    col = float(i) / logElement.maxValue;
    if (col < 0.04045f) {
      lut[i] = (col < 0.0f) ? 0.0f : col * (1.0f / 12.92f);
    }
    else {
      lut[i] = powf((col + 0.055f) * (1.0f / 1.055f), 2.4f);
    }
  }

  return lut;
}

static int convertRGBA_RGB(const float *src,
                           float *dst,
                           const LogImageFile *logImage,
                           const LogImageElement &logElement,
                           int elementIsSource)
{
  uint i;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  switch (logElement.transfer) {
    case transfer_Unspecified:
    case transfer_UserDefined:
    case transfer_Linear:
    case transfer_Logarithmic: {
      for (i = 0; i < logImage->width * logImage->height; i++) {
        *(dst_ptr++) = *(src_ptr++);
        *(dst_ptr++) = *(src_ptr++);
        *(dst_ptr++) = *(src_ptr++);
        src_ptr++;
      }

      return 0;
    }

    case transfer_PrintingDensity: {
      float *lut;

      if (elementIsSource == 1) {
        lut = getLogToLinLut(logImage, logElement);
      }
      else {
        lut = getLinToLogLut(logImage, logElement);
      }

      for (i = 0; i < logImage->width * logImage->height; i++) {
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        src_ptr++;
      }

      MEM_freeN(lut);

      return 0;
    }

    default:
      if (verbose) {
        printf("DPX/Cineon: Unknown transfer %d.\n", logElement.transfer);
      }
      return 1;
  }
}

static int convertRGB_RGBA(const float *src,
                           float *dst,
                           const LogImageFile *logImage,
                           const LogImageElement &logElement,
                           int elementIsSource)
{
  uint i;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  switch (logElement.transfer) {
    case transfer_Unspecified:
    case transfer_UserDefined:
    case transfer_Linear:
    case transfer_Logarithmic: {
      for (i = 0; i < logImage->width * logImage->height; i++) {
        *(dst_ptr++) = *(src_ptr++);
        *(dst_ptr++) = *(src_ptr++);
        *(dst_ptr++) = *(src_ptr++);
        *(dst_ptr++) = 1.0f;
      }

      return 0;
    }

    case transfer_PrintingDensity: {
      float *lut;

      if (elementIsSource == 1) {
        lut = getLogToLinLut(logImage, logElement);
      }
      else {
        lut = getLinToLogLut(logImage, logElement);
      }

      for (i = 0; i < logImage->width * logImage->height; i++) {
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        *(dst_ptr++) = 1.0f;
      }

      MEM_freeN(lut);

      return 0;
    }

    default:
      if (verbose) {
        printf("DPX/Cineon: Unknown transfer %d.\n", logElement.transfer);
      }
      return 1;
  }
}

static int convertRGBA_RGBA(const float *src,
                            float *dst,
                            const LogImageFile *logImage,
                            const LogImageElement &logElement,
                            int elementIsSource)
{
  uint i;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  switch (logElement.transfer) {
    case transfer_UserDefined:
    case transfer_Linear:
    case transfer_Logarithmic: {
      memcpy(dst, src, 4 * size_t(logImage->width) * size_t(logImage->height) * sizeof(float));
      return 0;
    }

    case transfer_PrintingDensity: {
      float *lut;

      if (elementIsSource == 1) {
        lut = getLogToLinLut(logImage, logElement);
      }
      else {
        lut = getLinToLogLut(logImage, logElement);
      }

      for (i = 0; i < logImage->width * logImage->height; i++) {
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
        *(dst_ptr++) = *(src_ptr++);
      }

      MEM_freeN(lut);

      return 0;
    }

    default:
      return 1;
  }
}

static int convertABGR_RGBA(const float *src,
                            float *dst,
                            const LogImageFile *logImage,
                            const LogImageElement &logElement,
                            int elementIsSource)
{
  uint i;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  switch (logElement.transfer) {
    case transfer_UserDefined:
    case transfer_Linear:
    case transfer_Logarithmic: {
      for (i = 0; i < logImage->width * logImage->height; i++) {
        src_ptr += 4;
        *(dst_ptr++) = *(src_ptr--);
        *(dst_ptr++) = *(src_ptr--);
        *(dst_ptr++) = *(src_ptr--);
        *(dst_ptr++) = *(src_ptr--);
        src_ptr += 4;
      }
      return 0;
    }

    case transfer_PrintingDensity: {
      float *lut;

      if (elementIsSource == 1) {
        lut = getLogToLinLut(logImage, logElement);
      }
      else {
        lut = getLinToLogLut(logImage, logElement);
      }

      for (i = 0; i < logImage->width * logImage->height; i++) {
        src_ptr += 4;
        *(dst_ptr++) = lut[float_uint(*(src_ptr--), logElement.maxValue)];
        *(dst_ptr++) = lut[float_uint(*(src_ptr--), logElement.maxValue)];
        *(dst_ptr++) = lut[float_uint(*(src_ptr--), logElement.maxValue)];
        *(dst_ptr++) = *(src_ptr--);
        src_ptr += 4;
      }

      MEM_freeN(lut);

      return 0;
    }

    default:
      return 1;
  }
}

static int convertCbYCr_RGBA(const float *src,
                             float *dst,
                             const LogImageFile *logImage,
                             const LogImageElement &logElement)
{
  uint i;
  float conversionMatrix[9], refLowData, y, cb, cr;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  if (getYUVtoRGBMatrix((float *)&conversionMatrix, logElement) != 0) {
    return 1;
  }

  refLowData = float(logElement.refLowData) / logElement.maxValue;

  for (i = 0; i < logImage->width * logImage->height; i++) {
    cb = *(src_ptr++) - 0.5f;
    y = *(src_ptr++) - refLowData;
    cr = *(src_ptr++) - 0.5f;

    *(dst_ptr++) = clamp_float(
        y * conversionMatrix[0] + cb * conversionMatrix[1] + cr * conversionMatrix[2], 0.0f, 1.0f);
    *(dst_ptr++) = clamp_float(
        y * conversionMatrix[3] + cb * conversionMatrix[4] + cr * conversionMatrix[5], 0.0f, 1.0f);
    *(dst_ptr++) = clamp_float(
        y * conversionMatrix[6] + cb * conversionMatrix[7] + cr * conversionMatrix[8], 0.0f, 1.0f);
    *(dst_ptr++) = 1.0f;
  }
  return 0;
}

static int convertCbYCrA_RGBA(const float *src,
                              float *dst,
                              const LogImageFile *logImage,
                              const LogImageElement &logElement)
{
  uint i;
  float conversionMatrix[9], refLowData, y, cb, cr, a;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  if (getYUVtoRGBMatrix((float *)&conversionMatrix, logElement) != 0) {
    return 1;
  }

  refLowData = float(logElement.refLowData) / logElement.maxValue;

  for (i = 0; i < logImage->width * logImage->height; i++) {
    cb = *(src_ptr++) - 0.5f;
    y = *(src_ptr++) - refLowData;
    cr = *(src_ptr++) - 0.5f;
    a = *(src_ptr++);

    *(dst_ptr++) = clamp_float(
        y * conversionMatrix[0] + cb * conversionMatrix[1] + cr * conversionMatrix[2], 0.0f, 1.0f);
    *(dst_ptr++) = clamp_float(
        y * conversionMatrix[3] + cb * conversionMatrix[4] + cr * conversionMatrix[5], 0.0f, 1.0f);
    *(dst_ptr++) = clamp_float(
        y * conversionMatrix[6] + cb * conversionMatrix[7] + cr * conversionMatrix[8], 0.0f, 1.0f);
    *(dst_ptr++) = a;
  }
  return 0;
}

static int convertCbYCrY_RGBA(const float *src,
                              float *dst,
                              const LogImageFile *logImage,
                              const LogImageElement &logElement)
{
  uint i;
  float conversionMatrix[9], refLowData, y1, y2, cb, cr;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  if (getYUVtoRGBMatrix((float *)&conversionMatrix, logElement) != 0) {
    return 1;
  }

  refLowData = float(logElement.refLowData) / logElement.maxValue;

  for (i = 0; i < logImage->width * logImage->height / 2; i++) {
    cb = *(src_ptr++) - 0.5f;
    y1 = *(src_ptr++) - refLowData;
    cr = *(src_ptr++) - 0.5f;
    y2 = *(src_ptr++) - refLowData;

    *(dst_ptr++) = clamp_float(y1 * conversionMatrix[0] + cb * conversionMatrix[1] +
                                   cr * conversionMatrix[2],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = clamp_float(y1 * conversionMatrix[3] + cb * conversionMatrix[4] +
                                   cr * conversionMatrix[5],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = clamp_float(y1 * conversionMatrix[6] + cb * conversionMatrix[7] +
                                   cr * conversionMatrix[8],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = 1.0f;
    *(dst_ptr++) = clamp_float(y2 * conversionMatrix[0] + cb * conversionMatrix[1] +
                                   cr * conversionMatrix[2],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = clamp_float(y2 * conversionMatrix[3] + cb * conversionMatrix[4] +
                                   cr * conversionMatrix[5],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = clamp_float(y2 * conversionMatrix[6] + cb * conversionMatrix[7] +
                                   cr * conversionMatrix[8],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = 1.0f;
  }
  return 0;
}

static int convertCbYACrYA_RGBA(const float *src,
                                float *dst,
                                const LogImageFile *logImage,
                                const LogImageElement &logElement)
{
  uint i;
  float conversionMatrix[9], refLowData, y1, y2, cb, cr, a1, a2;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  if (getYUVtoRGBMatrix((float *)&conversionMatrix, logElement) != 0) {
    return 1;
  }

  refLowData = float(logElement.refLowData) / logElement.maxValue;

  for (i = 0; i < logImage->width * logImage->height / 2; i++) {
    cb = *(src_ptr++) - 0.5f;
    y1 = *(src_ptr++) - refLowData;
    a1 = *(src_ptr++);
    cr = *(src_ptr++) - 0.5f;
    y2 = *(src_ptr++) - refLowData;
    a2 = *(src_ptr++);

    *(dst_ptr++) = clamp_float(y1 * conversionMatrix[0] + cb * conversionMatrix[1] +
                                   cr * conversionMatrix[2],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = clamp_float(y1 * conversionMatrix[3] + cb * conversionMatrix[4] +
                                   cr * conversionMatrix[5],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = clamp_float(y1 * conversionMatrix[6] + cb * conversionMatrix[7] +
                                   cr * conversionMatrix[8],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = a1;
    *(dst_ptr++) = clamp_float(y2 * conversionMatrix[0] + cb * conversionMatrix[1] +
                                   cr * conversionMatrix[2],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = clamp_float(y2 * conversionMatrix[3] + cb * conversionMatrix[4] +
                                   cr * conversionMatrix[5],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = clamp_float(y2 * conversionMatrix[6] + cb * conversionMatrix[7] +
                                   cr * conversionMatrix[8],
                               0.0f,
                               1.0f);
    *(dst_ptr++) = a2;
  }
  return 0;
}

static int convertLuminance_RGBA(const float *src,
                                 float *dst,
                                 const LogImageFile *logImage,
                                 const LogImageElement &logElement)
{
  uint i;
  float conversionMatrix[9], value, refLowData;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  if (getYUVtoRGBMatrix((float *)&conversionMatrix, logElement) != 0) {
    return 1;
  }

  refLowData = float(logElement.refLowData) / logElement.maxValue;

  for (i = 0; i < logImage->width * logImage->height; i++) {
    value = clamp_float((*(src_ptr++) - refLowData) * conversionMatrix[0], 0.0f, 1.0f);
    *(dst_ptr++) = value;
    *(dst_ptr++) = value;
    *(dst_ptr++) = value;
    *(dst_ptr++) = 1.0f;
  }
  return 0;
}

static int convertYA_RGBA(const float *src,
                          float *dst,
                          const LogImageFile *logImage,
                          const LogImageElement &logElement)
{
  uint i;
  float conversionMatrix[9], value, refLowData;
  const float *src_ptr = src;
  float *dst_ptr = dst;

  if (getYUVtoRGBMatrix((float *)&conversionMatrix, logElement) != 0) {
    return 1;
  }

  refLowData = float(logElement.refLowData) / logElement.maxValue;

  for (i = 0; i < logImage->width * logImage->height; i++) {
    value = clamp_float((*(src_ptr++) - refLowData) * conversionMatrix[0], 0.0f, 1.0f);
    *(dst_ptr++) = value;
    *(dst_ptr++) = value;
    *(dst_ptr++) = value;
    *(dst_ptr++) = *(src_ptr++);
  }
  return 0;
}

static int convertLogElementToRGBA(const float *src,
                                   float *dst,
                                   const LogImageFile *logImage,
                                   const LogImageElement &logElement,
                                   int dstIsLinearRGB)
{
  int rvalue;
  uint i;
  float *src_ptr;
  float *dst_ptr;

  /* Convert data in src to linear RGBA in dst */
  switch (logElement.descriptor) {
    case descriptor_RGB:
      rvalue = convertRGB_RGBA(src, dst, logImage, logElement, 1);
      break;

    case descriptor_RGBA:
      rvalue = convertRGBA_RGBA(src, dst, logImage, logElement, 1);
      break;

    case descriptor_ABGR:
      rvalue = convertABGR_RGBA(src, dst, logImage, logElement, 1);
      break;

    case descriptor_Luminance:
      rvalue = convertLuminance_RGBA(src, dst, logImage, logElement);
      break;

    case descriptor_CbYCr:
      rvalue = convertCbYCr_RGBA(src, dst, logImage, logElement);
      break;

    case descriptor_CbYCrY:
      rvalue = convertCbYCrY_RGBA(src, dst, logImage, logElement);
      break;

    case descriptor_CbYACrYA:
      rvalue = convertCbYACrYA_RGBA(src, dst, logImage, logElement);
      break;

    case descriptor_CbYCrA:
      rvalue = convertCbYCrA_RGBA(src, dst, logImage, logElement);
      break;

    case descriptor_YA: /* this descriptor is for internal use only */
      rvalue = convertYA_RGBA(src, dst, logImage, logElement);
      break;

    default:
      return 1;
  }

  if (rvalue == 1) {
    return 1;
  }
  if (dstIsLinearRGB) {
    /* convert data from sRGB to Linear RGB via lut */
    float *lut = getSrgbToLinLut(logElement);
    src_ptr = dst; /* no error here */
    dst_ptr = dst;
    for (i = 0; i < logImage->width * logImage->height; i++) {
      *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
      *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
      *(dst_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
      dst_ptr++;
      src_ptr++;
    }
    MEM_freeN(lut);
  }
  return 0;
}

static int convertRGBAToLogElement(const float *src,
                                   float *dst,
                                   const LogImageFile *logImage,
                                   const LogImageElement &logElement,
                                   int srcIsLinearRGB)
{
  uint i;
  int rvalue;
  const float *srgbSrc;
  float *srgbSrc_alloc;
  float *srgbSrc_ptr;
  const float *src_ptr = src;
  float *lut;

  if (srcIsLinearRGB != 0) {
    /* we need to convert src to sRGB */
    srgbSrc_alloc = (float *)imb_alloc_pixels(
        logImage->width, logImage->height, 4, sizeof(float), false, __func__);
    if (srgbSrc_alloc == nullptr) {
      return 1;
    }

    memcpy(srgbSrc_alloc,
           src,
           4 * size_t(logImage->width) * size_t(logImage->height) * sizeof(float));
    srgbSrc_ptr = srgbSrc_alloc;

    /* convert data from Linear RGB to sRGB via lut */
    lut = getLinToSrgbLut(logElement);
    for (i = 0; i < logImage->width * logImage->height; i++) {
      *(srgbSrc_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
      *(srgbSrc_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
      *(srgbSrc_ptr++) = lut[float_uint(*(src_ptr++), logElement.maxValue)];
      srgbSrc_ptr++;
      src_ptr++;
    }
    MEM_freeN(lut);
    srgbSrc = srgbSrc_alloc;
  }
  else {
    srgbSrc = src;
  }

  /* Convert linear RGBA data in src to format described by logElement in dst */
  switch (logElement.descriptor) {
    case descriptor_RGB:
      rvalue = convertRGBA_RGB(srgbSrc, dst, logImage, logElement, 0);
      break;

    case descriptor_RGBA:
      rvalue = convertRGBA_RGBA(srgbSrc, dst, logImage, logElement, 0);
      break;

    /* these ones are not supported for the moment */
    case descriptor_ABGR:
    case descriptor_Luminance:
    case descriptor_CbYCr:
    case descriptor_CbYCrY:
    case descriptor_CbYACrYA:
    case descriptor_CbYCrA:
    case descriptor_YA: /* this descriptor is for internal use only */
    default:
      rvalue = 1;
      break;
  }

  if (srcIsLinearRGB != 0) {
    MEM_freeN(srgbSrc_alloc);
  }

  return rvalue;
}
