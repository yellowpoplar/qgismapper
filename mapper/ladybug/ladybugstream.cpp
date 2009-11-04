/******************************************************************************
 *   Copyright (c) 2009 Martin Dobias
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 ******************************************************************************/

#include "ladybugstream.h"

#include "ladybugimage.h"

#include "ladybug_pgr.h"
//#include "ladybug.h"

#include <QRegExp>

// index increment is probably fixed also in ladybug SDK
#define IDX_INCREMENT   50

#define STREAM_SIGNATURE  "PGRLADYBUGSTREAM"
#define STREAM_VERSION    4

#define LIMIT_2GB 0x7fffffff

LadybugStream::LadybugStream()
{
}
	
bool LadybugStream::openForWriting(QString baseName, LadybugInfo cameraInfo, int fileIndex)
{
  mTotalNumFrames = 0;
  mNumMegabytes = 0;
  mCameraInfo = cameraInfo;
  return openForWritingInternal(baseName, fileIndex);
}

bool LadybugStream::openForWritingInternal(QString baseName, int fileIndex)
{
	// c:\ladybug\myStream-000000.pgr
	
	if (mFile.isOpen())
		return false;
	
  mFile.setFileName( pgrFilename(baseName,fileIndex) );
  if (!mFile.open(QIODevice::WriteOnly))
		return false;

  mBaseName = baseName;
  mFileIndex = fileIndex;
  mMode = Writing;
	
  mFile.write(STREAM_SIGNATURE, 16);
	
  uint32 dataOffset = 16 + sizeof(LadybugStreamHeadInfo) + mCameraInfo.calibration.count();
			
	// align to 512 bytes... just for fun. (sdk seems to be doing something similar)
	dataOffset = (dataOffset & ~0x1ff) + 0x200;
	
	LadybugStreamHeadInfo header;
	memset(&header, 0, sizeof(header));
  header.ulLadybugStreamVersion = STREAM_VERSION;
  header.ulFrameRate = 15; // TODO
  header.serialBase = mCameraInfo.serialBase;
  header.serialHead = mCameraInfo.serialHead;
	header.ulPaddingSize = 0; // not used for compressed data
	header.dataFormat = LADYBUG_DATAFORMAT_COLOR_SEP_SEQUENTIAL_JPEG;
	header.resolution = LADYBUG_RESOLUTION_1024x768;
	header.stippledFormat = LADYBUG_BGGR;
  header.ulConfigrationDataSize = mCameraInfo.calibration.count();

	header.ulNumberOfImages = 0; // to be filled later

	// initialize the offset table 
  mIndexIncrement = IDX_INCREMENT;
  mOffsetCount = 0;
  header.ulNumberOfKeyIndex = 1;
  header.ulIncrement = mIndexIncrement;
  header.ulOffsetTable[511] = dataOffset; // set first entry
	
	header.ulStreamDataOffset = dataOffset;
	
	// GPS info not used		
	header.ulGPSDataOffset = 0;
	header.ulGPSDataSize = 0;
			
	mFile.write((const char*) &header, sizeof(LadybugStreamHeadInfo));
	
  mFile.write(mCameraInfo.calibration);

	// go to the position of first image
  mFile.seek(dataOffset);
			
	mNumFrames = 0;
			
	return true;
}
	
bool LadybugStream::close()
{
	if (!mFile.isOpen())
		return false;
	
  // finish some tasks when writing
  if (mMode == Writing)
  {
    // update image count
    mFile.seek(16 + 0x88);
    mFile.write((const char*) &mNumFrames, 4); // num frames
    mFile.write((const char*) &mOffsetCount, 4); // num index entries

    // save index
    for (int i = 0; i < mOffsetCount; i++)
    {
      mFile.seek(16 + 0xbec - (i*4));
      mFile.write((const char*) (mOffsets+i), 4);
    }
  }
	
	mFile.close();
  return true;
}


bool LadybugStream::startNextFile()
{
  printf("starting new file: index %d\n", mFileIndex+1);

  if (!close())
    return false;

  if (!openForWritingInternal(mBaseName, mFileIndex+1))
    return false;

  return true;
}


bool LadybugStream::writeImage(const LadybugImage& image)
{
  if (!mFile.isOpen() || mMode != Writing)
    return false;

  if (mNumFrames % mIndexIncrement == 0)
	{
    int key = mNumFrames / mIndexIncrement;
		if (key < 512) // save only if we're not going out of the array :-)
		{
			// put to index
      mOffsets[key] = (uint32) mFile.pos();
			mOffsetCount = key + 1;
		}
	}
	
  // check whether we're not getting over 2GB limit
  if (mFile.pos() + image.frameBytes() >= LIMIT_2GB)
  {
    // create a new file if this is the case
    // otherwise it's not possible to index further frames
    // and ladybug sdk has problems with that
    if (!startNextFile())
      return false;
  }
	
  mFile.write((const char*) image.frameData(), image.frameBytes());

  // align every frame to 512 bytes - ladybug SDK does it too...
  /*
  // TODO: we have to alter image size in data
  int bytesLeft = (image.frameBytes() % 512);
  if (bytesLeft > 0)
  {
    mFile.write(QByteArray(512-bytesLeft, '\0'));
  }
  */

  // make sure the writes get to disk and not only to caches!
  if (mNumFrames % 16 == 15)
    sync();

	mNumFrames++;
  mTotalNumFrames++;
	mNumMegabytes += (double) image.frameBytes() / (1024*1024);
	
  return true;
}

double LadybugStream::megabytesWritten() const
{
	return mNumMegabytes;
}
	
ulong LadybugStream::framesCount() const
{
  return mTotalNumFrames;
}


bool LadybugStream::isOpen() const
{
	return mFile.isOpen();
}

LadybugStream::Mode LadybugStream::mode() const
{
  return mMode;
}

QString LadybugStream::pgrFilename(QString baseName, int index)
{
  return QString("%1-%2.pgr").arg(baseName).arg(index, 6, 10, QChar('0'));
}


bool LadybugStream::parseBaseNameAndIndex(QString filename, QString& outBaseName, int& outFileIndex)
{
  QRegExp re("(.+)-(\\d{6}).pgr");
  re.setCaseSensitivity(Qt::CaseInsensitive);
  if (re.indexIn(filename) == -1)
    return false;
  outBaseName = re.cap(1);
  outFileIndex = re.cap(2).toInt();
  return true;
}

bool LadybugStream::openForReading(QString baseName, int fileIndex)
{
  if (mFile.isOpen())
    return false;

  QString streamFilename = pgrFilename(baseName, fileIndex);

  if (!QFile::exists(streamFilename))
  {
    // it's possible that someone is calling this routine with whole filename
    QString outBaseName;
    int outFileIndex;
    if (!parseBaseNameAndIndex(baseName, outBaseName, outFileIndex))
      return false;
    streamFilename = pgrFilename(outBaseName, outFileIndex);
    if (!QFile::exists(streamFilename))
      return false;
    baseName = outBaseName;
    fileIndex = outFileIndex;
  }

  mBaseName = baseName;
  mFirstFileIndex = fileIndex;
  mTotalNumFrames = 0;
  mTotalNumFiles = 0;
  mFirstFrameList.clear();

  // now we have a base name and index suitable for opening
  // cycle through subsequent stream files, increase total num. of frames

  printf("trying to open stream files....\n");
  while (openForReadingInternal(fileIndex, mTotalNumFrames))
  {
    printf("opened index %d - frames count %d\n", fileIndex, mNumFrames);
    mFirstFrameList.append(mTotalNumFrames);
    mTotalNumFrames += mNumFrames;
    mTotalNumFiles++;
    close();
    fileIndex++;
  }

  // and now open the first stream file (if we're not at it)
  if (fileIndex != mFirstFileIndex)
  {
    openForReadingInternal(mFirstFileIndex, 0);
  }

  return (mTotalNumFiles != 0);
}


bool LadybugStream::openForReadingInternal(int fileIndex, int firstFrame)
{
  if (mFile.isOpen())
    return false;

  mFile.setFileName( pgrFilename(mBaseName, fileIndex) );
  if (!mFile.open(QIODevice::ReadOnly))
    return false;

  mFileIndex = fileIndex;
  mMode = Reading;

  // check file signature
  QByteArray signature = mFile.read(16);
  if (signature != QByteArray(STREAM_SIGNATURE, 16))
  {
    mFile.close();
    return false;
  }

  // read header
  LadybugStreamHeadInfo header;
  if (mFile.read((char*) &header, sizeof(header)) != sizeof(header))
  {
    mFile.close();
    return false;
  }

  // check data format
  if (header.dataFormat != LADYBUG_DATAFORMAT_COLOR_SEP_SEQUENTIAL_JPEG
   || header.resolution != LADYBUG_RESOLUTION_1024x768
   || header.stippledFormat != LADYBUG_BGGR)
  {
    mFile.close();
    return false;
  }

  // load no. of images, data offset
  mNumFrames = header.ulNumberOfImages;
  mDataOffset = header.ulStreamDataOffset;

  // load index info
  mOffsetCount = header.ulNumberOfKeyIndex;
  mIndexIncrement = header.ulIncrement;
  memcpy(mOffsets, header.ulOffsetTable, sizeof(mOffsets));
  // reverse the table - i.e. map 0-511 -> 511-0
  uint32 tmpOffset;
  for (int i = 0; i < 128; i++)
  {
    tmpOffset = mOffsets[i];
    mOffsets[i] = mOffsets[511-i];
    mOffsets[511-i] = tmpOffset;
  }

  // go to the position of first image
  mFile.seek(mDataOffset);

  mCurrentFrame = firstFrame; // at first frame
  mFirstFrame = firstFrame;

  printf("# frames: %u\n", mNumFrames);
  printf("offsets: increment %u count %d\n", mIndexIncrement, mOffsetCount);
  for (int i = 0; i < mOffsetCount && i < 512; i++)
  {
    printf("%08x  ", mOffsets[i]);
    if (i % 8 == 7) printf("\n");
  }

  return true;
}

#ifndef WIN32
#include <arpa/inet.h>
#else
#include <winsock.h>
#endif

static void big2little_struct(unsigned int* str, int size)
{
  for (int i = 0; i < size; i += 4)
  {
    *str = ntohl(*str);
    str++;
  }
}

ulong LadybugStream::currentFrame() const
{
  return mCurrentFrame-1;
}


bool LadybugStream::readNextFrame(LadybugImage& image)
{
  if (!mFile.isOpen() || mMode != Reading)
    return false;
  if (mCurrentFrame >= mTotalNumFrames)
    return false;

  printf("read next frame (at %d)\n", mCurrentFrame);
  // check whether we don't have to change file
  if (mCurrentFrame < mFirstFrame || mCurrentFrame >= mFirstFrame+mNumFrames)
  {
    printf("have to seek to other file! (at %d)\n", mCurrentFrame);
    seekToFrame(mCurrentFrame);
  }

  ulong pos = mFile.pos();

  LadybugImageHeader header, headerOrig;
  LadybugImageInfo info, infoOrig;

  if (mFile.read((char*) &headerOrig, sizeof(header)) != sizeof(header))
    return false;

  if (mFile.read((char*) &infoOrig, sizeof(info)) != sizeof(info))
    return false;

  memcpy(&header, &headerOrig, sizeof(header));
  memcpy(&info, &infoOrig, sizeof(info));
  big2little_struct((unsigned int*)&header, sizeof(header));
  big2little_struct((unsigned int*)&info, sizeof(info));

  //printf("read: %u\n", header.ulDataSize);

  unsigned char* data = new unsigned char[ header.ulDataSize ];
  unsigned char* ptr = data;
  memcpy(ptr, &headerOrig, sizeof(LadybugImageHeader));
  ptr += sizeof(LadybugImageHeader);
  memcpy(ptr, &infoOrig, sizeof(LadybugImageInfo));
  ptr += sizeof(LadybugImageInfo);
  unsigned int moreSize = header.ulDataSize - sizeof(LadybugImageHeader) - sizeof(LadybugImageInfo);
  if (mFile.read((char*) ptr, moreSize) != moreSize)
  {
    delete [] data;
    return false;
  }

  uint miliSecs = info.ulTimeMicroSeconds / 1000;
  printf("%04d ... seq#: %u ... time %u.%03u\n", mCurrentFrame, info.ulSequenceId, info.ulTimeSeconds, miliSecs);

  mCurrentFrame++;

  // transfer the buffer to the passed LadybugImage class
  image.setData(data, header.ulDataSize, true);

  return true;
}

bool LadybugStream::seekToFrame(int frameId)
{
  if (!mFile.isOpen() || mMode != Reading)
    return false;

  if (frameId < 0 || frameId >= mTotalNumFrames)
  {
    printf("frame is out of range!\n");
    return false;
  }

  // 1. check whether we're in correct file
  if (frameId < mFirstFrame || frameId >= mFirstFrame+mNumFrames)
  {
    printf("going to change file\n");
    // find out the right file index
    int i = 0; // index in the list
    while (mFirstFrameList.count() > i && mFirstFrameList[i] <= frameId)
      i++;
    int fileIndex = mFirstFileIndex + i-1;
    int firstFrame = mFirstFrameList[i-1];
    printf("new file index: %d\n", fileIndex);

    // close this one and open new one
    close();
    if (!openForReadingInternal(fileIndex, firstFrame))
      return false;
    printf("in new file!\n");
  }

  // 2. set position in the file
  // within one file...

  // set new current frame
  mCurrentFrame = frameId;

  // lower the frame id to work in context of the current file
  frameId -= mFirstFrame;

  ulong offset = mDataOffset; // position of first image

  int offsetIndex = frameId / mIndexIncrement;
  if (offsetIndex >= mOffsetCount)
  {
    // we're behind the indexed space, let's use the last offset
    offsetIndex = mOffsetCount-1;
  }

  if (offsetIndex > 0)
  {
    // we have valid offset index, use it so we'll traverse less frames directly
    offset = mOffsets[offsetIndex];
    frameId -= mIndexIncrement*offsetIndex;
  }

  // seek to the starting position
  mFile.seek(offset);

  // while we're not there, move forward
  LadybugImageHeader header;
  while (frameId > 0)
  {
    // read image's data size
    if (mFile.read((char*) &header, sizeof(header)) != sizeof(header))
    {
      mFile.seek(mDataOffset); // fallback to the start on error
      mCurrentFrame = 0;
      return false;
    }
    big2little_struct((unsigned int*)&header, sizeof(header));

    // seek to next frame
    if (!mFile.seek( mFile.pos() + header.ulDataSize - sizeof(header) ))
    {
      mFile.seek(mDataOffset); // fallback to the start on error
      mCurrentFrame = 0;
      return false;
    }

    frameId--;
  }

  return true;
}

bool LadybugStream::seekToTime(unsigned int miliseconds)
{
  // TODO: make sure we're in the right file
  // (use "this file start time" and "next file start time" to find out)
  // ("next file start time" could be evaluated lazily)

  // try to guess which frame should be the one
  int frameId = miliseconds * 15 / 1000;

  // TODO: go through frames and find out the closest
  // ... make use of frameTime() function?

  return seekToFrame(frameId);
}

uint LadybugStream::frameTime(int frameId)
{
  if (frameId != mCurrentFrame)
  {
    // go to the frame if we're not at it
    seekToFrame(frameId);
  }

  ulong framePos = mFile.pos();

  LadybugImageHeader header;
  LadybugImageInfo info;

  // read the image info
  if (mFile.read((char*) &header, sizeof(header)) != sizeof(header))
    return 0;
  if (mFile.read((char*) &info, sizeof(info)) != sizeof(info))
    return 0;

  // convert image info
  big2little_struct((unsigned int*)&header, sizeof(header));
  big2little_struct((unsigned int*)&info, sizeof(info));

  // calculate time
  uint imgMsecs = info.ulTimeSeconds * 1000 + info.ulTimeMicroSeconds / 1000;

  // go back to start of this frame
  mFile.seek(framePos);

 return imgMsecs;
}