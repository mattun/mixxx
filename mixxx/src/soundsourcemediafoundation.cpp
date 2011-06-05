/**
 * \file soundsourcemediafoundation.cpp
 * \author Albert Santoni <alberts at mixxx dot org>
 * \date Jan 10, 2011
 */

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <QtDebug>
#include <QUrl>
#include <taglib/mp4file.h>


//#define WINVER _WIN32_WINNT_WIN7

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <mferror.h>

#include "soundsourcemediafoundation.h"

const int kBitsPerSample = 16;
const int kNumChannels = 2;
const int kSampleRate = 44100;

/** Microsoft examples use this snippet often. */
template<class T> void SafeRelease(T **ppT)
{
    if (*ppT) {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

SoundSourceMediaFoundation::SoundSourceMediaFoundation(QString filename)
    : SoundSource(filename)
    , m_file(filename)
    , m_samples(0)
    , m_hFile(INVALID_HANDLE_VALUE)
    , m_pAudioType(NULL)
    , m_pReader(NULL)
{
    m_wcFilename = new wchar_t[255];
}

SoundSourceMediaFoundation::~SoundSourceMediaFoundation()
{	
    delete [] m_wcFilename;

    // Clean up.
    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
    }

    SafeRelease(&m_pReader);
    SafeRelease(&m_pAudioType);
    MFShutdown();
    CoUninitialize();
}

// soundsource overrides
int SoundSourceMediaFoundation::open()
{
    HRESULT hr = S_OK;

    //m_file.open(QIODevice::ReadOnly);

    QString qurlStr = m_qFilename;//blah.toString();
    
    int wcFilenameLength = m_qFilename.toWCharArray(m_wcFilename);
    //toWCharArray does not append a null terminator to the string!
    m_wcFilename[wcFilenameLength] = '\0';
    
    // Initialize the COM library.
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Initialize the Media Foundation platform.
    if (SUCCEEDED(hr)) {
        hr = MFStartup(MF_VERSION);
    }

    // Create the source reader to read the input file.
    if (SUCCEEDED(hr)) {
        hr = MFCreateSourceReaderFromURL(m_wcFilename, NULL, &m_pReader);
        if (FAILED(hr)) {
            qDebug() << __FILE__ << "Error opening input file:" << m_qFilename << hr;
            return ERR;
        }
    }    

	if (hr != S_OK) {
		qDebug() << __FILE__ << "Error opening file.";
		return ERR;
	}

    hr = ConfigureAudioStream(m_pReader, &m_pAudioType);
	if (hr != S_OK) {
		qDebug() << __FILE__ <<"Error configuring audio stream.";
		return ERR;
	}
    
    PROPVARIANT thing;
    //Get the bitrate
    m_pReader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                        MF_PD_AUDIO_ENCODING_BITRATE,
                                        &thing);
    m_iBitrate = thing.intVal;
    qDebug() << __FILE__ << "Bitrate:" << m_iBitrate;
    PropVariantClear(&thing);
    
    //Get the duration
    m_pReader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                        MF_PD_DURATION, //Gets the duration in 100-nanosecond units.
                                        &thing);
    // QuadPart isn't available on compilers that don't support _int64. Visual
    // Studio 6.0 introduced the type in 1998, so I think we're safe here
    // -bkgood
    m_iDuration = thing.hVal.QuadPart * 1E-7;
    qDebug() << __FILE__ << "Duration:" << m_iDuration;
    PropVariantClear(&thing);

	//Set m_iChannels and m_samples;
	m_iChannels = kNumChannels;
	
	m_samples = m_iDuration * kSampleRate * kNumChannels;
    //(totalFrameCount/*-m_headerFrames*/)*m_iChannels;
	//m_iDuration = m_samples / (inputFormat.mSampleRate * m_iChannels);
	m_iSampleRate = kSampleRate; //inputFormat.mSampleRate;
	qDebug() << m_samples << m_iChannels;
	
	//Seek to position 0, which forces us to skip over all the header frames.
	//This makes sure we're ready to just let the Analyser rip and it'll
	//get the number of samples it expects (ie. no header frames).
	seek(0);

    return OK;
}

long SoundSourceMediaFoundation::seek(long filepos)
{
    //http://msdn.microsoft.com/en-us/library/dd374668(v=VS.85).aspx
    float timeInSeconds = filepos / ((float) kSampleRate * kNumChannels);
    float timeIn100Nanosecs = timeInSeconds * 10E7;
    PROPVARIANT v;
    memset(&v, 0, sizeof(PROPVARIANT)); 
    v.vt = VT_R4;
    v.fltVal = timeIn100Nanosecs;
    qDebug() << __FILE__ << "Seeking to" << timeIn100Nanosecs << timeIn100Nanosecs/10E7;

    if (m_pReader->SetCurrentPosition(GUID_NULL, //Means 100-nanosecond units.
                                  v) != S_OK) {
        qDebug() << __FILE__ << "Failed to seek.";
    }

    //XXX: Fudge, SetCurrentPosition doesn't guarantee sample-accurate seeking 
    //(at least for video). Microsoft recommends using ReadSample after a 
    //seek to jog forward to the exact position we want. 

    return filepos;
}

unsigned int SoundSourceMediaFoundation::read(unsigned long size, const SAMPLE *destination)
{
    //if (!m_decoder) return 0;
    SAMPLE *destBuffer(const_cast<SAMPLE*>(destination));
    unsigned int i = 0;
    int numFrames = 0;//(size / 2); /// m_inputFormat.mBytesPerFrame);
    unsigned int totalFramesToRead = size/2;
    unsigned int numFramesRead = 0;
    unsigned int numFramesToRead = totalFramesToRead;

    HRESULT hr = S_OK;
    //DWORD cbAudioData = 0;
    DWORD cbBuffer = 0;  // Length of pAudioData, in bytes
    BYTE *pAudioData = NULL; // Raw audio buffer

    IMFSample *pSample = NULL;
    IMFMediaBuffer *pBuffer = NULL;

    while (numFramesRead < totalFramesToRead) {
    	numFramesToRead = totalFramesToRead - numFramesRead;

        DWORD dwFlags = 0;

        //FIXME: Deal with leftover frames!

        // Read the next "sample" (it's really a buffer of audio).
        hr = m_pReader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, // [in] DWORD dwStreamIndex,
            0,                                          // [in] DWORD dwControlFlags,
            NULL,                                       // [out] DWORD *pdwActualStreamIndex,
            &dwFlags,                                   // [out] DWORD *pdwStreamFlags,
            NULL,                                       // [out] LONGLONG *pllTimestamp,
            &pSample);                                  // [out] IMFSample **ppSample

        if (FAILED(hr)) break;

        if (dwFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            qDebug() << "Type change";
            break;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            qDebug() << "End of input file.";
            break;
        }

        if (pSample == NULL) {
            qDebug() << "No sample";
            continue;
        }

        // Get a pointer to the audio data in the sample.

        hr = pSample->ConvertToContiguousBuffer(&pBuffer);

        if (FAILED(hr)) break;

        //Get access to the raw data in the buffer. 
        hr = pBuffer->Lock(&pAudioData, NULL, &cbBuffer);

        if (FAILED(hr)) break;

        //Calculate the number of frames read based on the number of bytes returned.
        numFrames = cbBuffer / ( (kBitsPerSample / 8) * kNumChannels);
        for (int i = 0; i < numFrames*2; i++) {
            destBuffer[numFramesRead*kNumChannels + i] = pAudioData[i];
        }
        //Copy the data to destBuffer;

        // Unlock the buffer.
        hr = pBuffer->Unlock();
        pAudioData = NULL;

        if (FAILED(hr)) break;

        // Update running total of audio data.
        /*
        cbAudioData += cbBuffer;

        if (cbAudioData >= cbMaxAudioData)
        {
            break;
        } */

        SafeRelease(&pSample);
        SafeRelease(&pBuffer);

		if (!numFrames) {
			// this is our termination condition
			break;
		}
		numFramesRead += numFrames;
    }

    if (pAudioData) {
        pBuffer->Unlock();
    }

    SafeRelease(&pBuffer);
    SafeRelease(&pSample);

    return numFramesRead*2;
}

inline unsigned long SoundSourceMediaFoundation::length()
{
    return m_samples; 
}

int SoundSourceMediaFoundation::parseHeader()
{
    setType("m4a");

    TagLib::MP4::File f(getFilename().toUtf8().constData());
    bool result = processTaglibFile(f);
    TagLib::MP4::Tag* tag = f.tag();

    if (tag) {
        processMP4Tag(tag);
    }

    if (result)
        return OK;
    return ERR;
}


// static
QList<QString> SoundSourceMediaFoundation::supportedFileExtensions()
{
    QList<QString> list;
    list.push_back("m4a");
    list.push_back("mp4");
    return list;
}


//-------------------------------------------------------------------
// ConfigureAudioStream
//
// Selects an audio stream from the source file, and configures the
// stream to deliver decoded PCM audio.
//-------------------------------------------------------------------

/** Cobbled together from:
    http://msdn.microsoft.com/en-us/library/dd757929(v=vs.85).aspx
    and http://msdn.microsoft.com/en-us/library/dd317928(VS.85).aspx
    -- Albert
    */
HRESULT SoundSourceMediaFoundation::ConfigureAudioStream(
    IMFSourceReader *pReader,   // Pointer to the source reader.
    IMFMediaType **ppPCMAudio   // Receives the audio format.
    )
{
    IMFMediaType *pUncompressedAudioType = NULL;
    IMFMediaType *pPartialType = NULL;

    // Select the first audio stream, and deselect all other streams.
    HRESULT hr = pReader->SetStreamSelection(
        (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);

    if (SUCCEEDED(hr)) {
        hr = pReader->SetStreamSelection(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    }

    // Calculate derived values.
    UINT32 blockAlign = kNumChannels * (kBitsPerSample / 8);
    UINT32 bytesPerSecond = blockAlign * kSampleRate;

    // Create a partial media type that specifies uncompressed PCM audio.
    hr = MFCreateMediaType(&pPartialType);
    //XXX: This is probably a full type now that I filled out the rest
    //     of the fields that the second link above said to for creating
    //     uncompressed PCM audio.

    if (SUCCEEDED(hr)) {
        hr = pPartialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }

    if (SUCCEEDED(hr)) {
        hr = pPartialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    }

    if (SUCCEEDED(hr)) {
        hr = pPartialType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    }

    if (SUCCEEDED(hr)) {
        hr = pPartialType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kNumChannels);
    }

    if (SUCCEEDED(hr)) {
        hr = pPartialType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kBitsPerSample); 
        //We'll get signed integers out.
    }

    if (SUCCEEDED(hr)) {
        hr = pPartialType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
    }

    if (SUCCEEDED(hr)) {
        hr = pPartialType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
    }

    if (SUCCEEDED(hr)) {
        hr = pPartialType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }

    // Set this type on the source reader. The source reader will
    // load the necessary decoder.
    if (SUCCEEDED(hr)) {
        hr = pReader->SetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            NULL, pPartialType);
    }

    // Get the complete uncompressed format.
    if (SUCCEEDED(hr)) {
        hr = pReader->GetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            &pUncompressedAudioType);
    }

    // Ensure the stream is selected.
    if (SUCCEEDED(hr)) {
        hr = pReader->SetStreamSelection(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            TRUE);
    }

    // Return the PCM format to the caller.
    if (SUCCEEDED(hr)) {
        *ppPCMAudio = pUncompressedAudioType;
        (*ppPCMAudio)->AddRef();
    }

    SafeRelease(&pUncompressedAudioType);
    SafeRelease(&pPartialType);
    return hr;
}
