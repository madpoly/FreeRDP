/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Server Audio Virtual Channel
 *
 * Copyright 2012 Vic Lee
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	 http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/print.h>
#include <winpr/stream.h>

#include <freerdp/channels/log.h>

#include "rdpsnd_common.h"
#include "rdpsnd_main.h"

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_send_formats(RdpsndServerContext* context, wStream* s)
{
	size_t pos;
	UINT16 i;
	BOOL status = FALSE;
	ULONG written;
	Stream_Write_UINT8(s, SNDC_FORMATS);
	Stream_Write_UINT8(s, 0);
	Stream_Seek_UINT16(s);
	Stream_Write_UINT32(s, 0);                           /* dwFlags */
	Stream_Write_UINT32(s, 0);                           /* dwVolume */
	Stream_Write_UINT32(s, 0);                           /* dwPitch */
	Stream_Write_UINT16(s, 0);                           /* wDGramPort */
	Stream_Write_UINT16(s, context->num_server_formats); /* wNumberOfFormats */
	Stream_Write_UINT8(s, context->block_no);            /* cLastBlockConfirmed */
	Stream_Write_UINT16(s, CHANNEL_VERSION_WIN_MAX);     /* wVersion */
	Stream_Write_UINT8(s, 0);                            /* bPad */

	for (i = 0; i < context->num_server_formats; i++)
	{
		AUDIO_FORMAT format = context->server_formats[i];
		// TODO: Eliminate this!!!
		format.nAvgBytesPerSec =
		    format.nSamplesPerSec * format.nChannels * format.wBitsPerSample / 8;

		if (!audio_format_write(s, &format))
			goto fail;
	}

	pos = Stream_GetPosition(s);
	Stream_SetPosition(s, 2);
	Stream_Write_UINT16(s, pos - 4);
	Stream_SetPosition(s, pos);
	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR)Stream_Buffer(s),
	                                Stream_GetPosition(s), &written);
	Stream_SetPosition(s, 0);
fail:
	return status ? CHANNEL_RC_OK : ERROR_INTERNAL_ERROR;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_recv_waveconfirm(RdpsndServerContext* context, wStream* s)
{
	UINT16 timestamp;
	BYTE confirmBlockNum;
	UINT error = CHANNEL_RC_OK;

	if (Stream_GetRemainingLength(s) < 4)
	{
		WLog_ERR(TAG, "not enough data in stream!");
		return ERROR_INVALID_DATA;
	}

	Stream_Read_UINT16(s, timestamp);
	Stream_Read_UINT8(s, confirmBlockNum);
	Stream_Seek_UINT8(s);
	IFCALLRET(context->ConfirmBlock, error, context, confirmBlockNum, timestamp);

	if (error)
		WLog_ERR(TAG, "context->ConfirmBlock failed with error %" PRIu32 "", error);

	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_recv_quality_mode(RdpsndServerContext* context, wStream* s)
{
	UINT16 quality;

	if (Stream_GetRemainingLength(s) < 4)
	{
		WLog_ERR(TAG, "not enough data in stream!");
		return ERROR_INVALID_DATA;
	}

	Stream_Read_UINT16(s, quality);
	Stream_Seek_UINT16(s); // reserved
	WLog_DBG(TAG, "Client requested sound quality: 0x%04" PRIX16 "", quality);
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_recv_formats(RdpsndServerContext* context, wStream* s)
{
	UINT16 i, num_known_format = 0;
	UINT32 flags, vol, pitch;
	UINT16 udpPort;
	BYTE lastblock;
	UINT error = CHANNEL_RC_OK;

	if (Stream_GetRemainingLength(s) < 20)
	{
		WLog_ERR(TAG, "not enough data in stream!");
		return ERROR_INVALID_DATA;
	}

	Stream_Read_UINT32(s, flags);                       /* dwFlags */
	Stream_Read_UINT32(s, vol);                         /* dwVolume */
	Stream_Read_UINT32(s, pitch);                       /* dwPitch */
	Stream_Read_UINT16(s, udpPort);                     /* wDGramPort */
	Stream_Read_UINT16(s, context->num_client_formats); /* wNumberOfFormats */
	Stream_Read_UINT8(s, lastblock);                    /* cLastBlockConfirmed */
	Stream_Read_UINT16(s, context->clientVersion);      /* wVersion */
	Stream_Seek_UINT8(s);                               /* bPad */

	/* this check is only a guess as cbSize can influence the size of a format record */
	if (Stream_GetRemainingLength(s) < context->num_client_formats * 18ULL)
	{
		WLog_ERR(TAG, "not enough data in stream!");
		return ERROR_INVALID_DATA;
	}

	if (!context->num_client_formats)
	{
		WLog_ERR(TAG, "client doesn't support any format!");
		return ERROR_INTERNAL_ERROR;
	}

	context->client_formats = audio_formats_new(context->num_client_formats);

	if (!context->client_formats)
	{
		WLog_ERR(TAG, "calloc failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	for (i = 0; i < context->num_client_formats; i++)
	{
		if (Stream_GetRemainingLength(s) < 18)
		{
			WLog_ERR(TAG, "not enough data in stream!");
			error = ERROR_INVALID_DATA;
			goto out_free;
		}

		Stream_Read_UINT16(s, context->client_formats[i].wFormatTag);
		Stream_Read_UINT16(s, context->client_formats[i].nChannels);
		Stream_Read_UINT32(s, context->client_formats[i].nSamplesPerSec);
		Stream_Read_UINT32(s, context->client_formats[i].nAvgBytesPerSec);
		Stream_Read_UINT16(s, context->client_formats[i].nBlockAlign);
		Stream_Read_UINT16(s, context->client_formats[i].wBitsPerSample);
		Stream_Read_UINT16(s, context->client_formats[i].cbSize);

		if (context->client_formats[i].cbSize > 0)
		{
			if (!Stream_SafeSeek(s, context->client_formats[i].cbSize))
			{
				WLog_ERR(TAG, "Stream_SafeSeek failed!");
				error = ERROR_INTERNAL_ERROR;
				goto out_free;
			}
		}

		if (context->client_formats[i].wFormatTag != 0)
		{
			// lets call this a known format
			// TODO: actually look through our own list of known formats
			num_known_format++;
		}
	}

	if (!context->num_client_formats)
	{
		WLog_ERR(TAG, "client doesn't support any known format!");
		goto out_free;
	}

	return CHANNEL_RC_OK;
out_free:
	free(context->client_formats);
	return error;
}

static DWORD WINAPI rdpsnd_server_thread(LPVOID arg)
{
	DWORD nCount = 0, status;
	HANDLE events[2] = { 0 };
	RdpsndServerContext* context = (RdpsndServerContext*)arg;
	UINT error = CHANNEL_RC_OK;
	WINPR_ASSERT(context);
	events[nCount++] = context->priv->channelEvent;
	events[nCount++] = context->priv->StopEvent;

	WINPR_ASSERT(nCount <= ARRAYSIZE(events));

	while (TRUE)
	{
		status = WaitForMultipleObjects(nCount, events, FALSE, INFINITE);

		if (status == WAIT_FAILED)
		{
			error = GetLastError();
			WLog_ERR(TAG, "WaitForMultipleObjects failed with error %" PRIu32 "!", error);
			break;
		}

		status = WaitForSingleObject(context->priv->StopEvent, 0);

		if (status == WAIT_FAILED)
		{
			error = GetLastError();
			WLog_ERR(TAG, "WaitForSingleObject failed with error %" PRIu32 "!", error);
			break;
		}

		if (status == WAIT_OBJECT_0)
			break;

		if ((error = rdpsnd_server_handle_messages(context)))
		{
			WLog_ERR(TAG, "rdpsnd_server_handle_messages failed with error %" PRIu32 "", error);
			break;
		}
	}

	if (error && context->rdpcontext)
		setChannelError(context->rdpcontext, error, "rdpsnd_server_thread reported an error");

	ExitThread(error);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_initialize(RdpsndServerContext* context, BOOL ownThread)
{
	context->priv->ownThread = ownThread;
	return context->Start(context);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_select_format(RdpsndServerContext* context, UINT16 client_format_index)
{
	int bs;
	int out_buffer_size;
	AUDIO_FORMAT* format;
	UINT error = CHANNEL_RC_OK;

	if ((client_format_index >= context->num_client_formats) || (!context->src_format))
	{
		WLog_ERR(TAG, "index %d is not correct.", client_format_index);
		return ERROR_INVALID_DATA;
	}

	EnterCriticalSection(&context->priv->lock);
	context->priv->src_bytes_per_sample = context->src_format->wBitsPerSample / 8;
	context->priv->src_bytes_per_frame =
	    context->priv->src_bytes_per_sample * context->src_format->nChannels;
	context->selected_client_format = client_format_index;
	format = &context->client_formats[client_format_index];

	if (format->nSamplesPerSec == 0)
	{
		WLog_ERR(TAG, "invalid Client Sound Format!!");
		error = ERROR_INVALID_DATA;
		goto out;
	}

	if (context->latency <= 0)
		context->latency = 50;

	context->priv->out_frames = context->src_format->nSamplesPerSec * context->latency / 1000;

	if (context->priv->out_frames < 1)
		context->priv->out_frames = 1;

	switch (format->wFormatTag)
	{
		case WAVE_FORMAT_DVI_ADPCM:
			bs = (format->nBlockAlign - 4 * format->nChannels) * 4;
			context->priv->out_frames -= context->priv->out_frames % bs;

			if (context->priv->out_frames < bs)
				context->priv->out_frames = bs;

			break;

		case WAVE_FORMAT_ADPCM:
			bs = (format->nBlockAlign - 7 * format->nChannels) * 2 / format->nChannels + 2;
			context->priv->out_frames -= context->priv->out_frames % bs;

			if (context->priv->out_frames < bs)
				context->priv->out_frames = bs;

			break;
	}

	context->priv->out_pending_frames = 0;
	out_buffer_size = context->priv->out_frames * context->priv->src_bytes_per_frame;

	if (context->priv->out_buffer_size < out_buffer_size)
	{
		BYTE* newBuffer;
		newBuffer = (BYTE*)realloc(context->priv->out_buffer, out_buffer_size);

		if (!newBuffer)
		{
			WLog_ERR(TAG, "realloc failed!");
			error = CHANNEL_RC_NO_MEMORY;
			goto out;
		}

		context->priv->out_buffer = newBuffer;
		context->priv->out_buffer_size = out_buffer_size;
	}

	freerdp_dsp_context_reset(context->priv->dsp_context, format, 0u);
out:
	LeaveCriticalSection(&context->priv->lock);
	return error;
}

static BOOL rdpsnd_server_align_wave_pdu(wStream* s, UINT32 alignment)
{
	size_t size;
	Stream_SealLength(s);
	size = Stream_Length(s);

	if ((size % alignment) != 0)
	{
		size_t offset = alignment - size % alignment;

		if (!Stream_EnsureRemainingCapacity(s, offset))
			return FALSE;

		Stream_Zero(s, offset);
	}

	Stream_SealLength(s);
	return TRUE;
}

/**
 * Function description
 * context->priv->lock should be obtained before calling this function
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_send_wave_pdu(RdpsndServerContext* context, UINT16 wTimestamp)
{
	size_t length;
	size_t start, end = 0;
	const BYTE* src;
	AUDIO_FORMAT* format;
	ULONG written;
	wStream* s = context->priv->rdpsnd_pdu;
	UINT error = CHANNEL_RC_OK;

	if (context->selected_client_format >= context->num_client_formats)
		return ERROR_INTERNAL_ERROR;

	format = &context->client_formats[context->selected_client_format];
	/* WaveInfo PDU */
	Stream_SetPosition(s, 0);
	Stream_Write_UINT8(s, SNDC_WAVE);                        /* msgType */
	Stream_Write_UINT8(s, 0);                                /* bPad */
	Stream_Write_UINT16(s, 0);                               /* BodySize */
	Stream_Write_UINT16(s, wTimestamp);                      /* wTimeStamp */
	Stream_Write_UINT16(s, context->selected_client_format); /* wFormatNo */
	Stream_Write_UINT8(s, context->block_no);                /* cBlockNo */
	Stream_Seek(s, 3);                                       /* bPad */
	start = Stream_GetPosition(s);
	src = context->priv->out_buffer;
	length = context->priv->out_pending_frames * context->priv->src_bytes_per_frame * 1ULL;

	if (!freerdp_dsp_encode(context->priv->dsp_context, context->src_format, src, length, s))
		return ERROR_INTERNAL_ERROR;
	else
	{
		/* Set stream size */
		if (!rdpsnd_server_align_wave_pdu(s, format->nBlockAlign))
			return ERROR_INTERNAL_ERROR;

		end = Stream_GetPosition(s);
		Stream_SetPosition(s, 2);
		Stream_Write_UINT16(s, end - start + 8);
		Stream_SetPosition(s, end);
		context->block_no = (context->block_no + 1) % 256;

		if (!WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR)Stream_Buffer(s),
		                            start + 4, &written))
		{
			WLog_ERR(TAG, "WTSVirtualChannelWrite failed!");
			error = ERROR_INTERNAL_ERROR;
		}
	}

	if (error != CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "WTSVirtualChannelWrite failed!");
		error = ERROR_INTERNAL_ERROR;
		goto out;
	}

	Stream_SetPosition(s, start);
	Stream_Write_UINT32(s, 0); /* bPad */
	Stream_SetPosition(s, start);

	if (!WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR)Stream_Pointer(s), end - start,
	                            &written))
	{
		WLog_ERR(TAG, "WTSVirtualChannelWrite failed!");
		error = ERROR_INTERNAL_ERROR;
	}

out:
	Stream_SetPosition(s, 0);
	context->priv->out_pending_frames = 0;
	return error;
}

/**
 * Function description
 * context->priv->lock should be obtained before calling this function
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_send_wave2_pdu(RdpsndServerContext* context, UINT16 wTimestamp)
{
	size_t length;
	size_t end = 0;
	const BYTE* src;
	AUDIO_FORMAT* format;
	ULONG written;
	wStream* s = context->priv->rdpsnd_pdu;
	UINT error = CHANNEL_RC_OK;

	if (context->selected_client_format >= context->num_client_formats)
		return ERROR_INTERNAL_ERROR;

	format = &context->client_formats[context->selected_client_format];
	/* WaveInfo PDU */
	Stream_SetPosition(s, 0);
	Stream_Write_UINT8(s, SNDC_WAVE2);                       /* msgType */
	Stream_Write_UINT8(s, 0);                                /* bPad */
	Stream_Write_UINT16(s, 0);                               /* BodySize */
	Stream_Write_UINT16(s, wTimestamp);                      /* wTimeStamp */
	Stream_Write_UINT16(s, context->selected_client_format); /* wFormatNo */
	Stream_Write_UINT8(s, context->block_no);                /* cBlockNo */
	Stream_Seek(s, 3);                                       /* bPad */
	Stream_Write_UINT32(s, wTimestamp);                      /* dwAudioTimeStamp */
	src = context->priv->out_buffer;
	length = context->priv->out_pending_frames * context->priv->src_bytes_per_frame;

	if (!freerdp_dsp_encode(context->priv->dsp_context, context->src_format, src, length, s))
		error = ERROR_INTERNAL_ERROR;
	else
	{
		BOOL rc;

		/* Set stream size */
		if (!rdpsnd_server_align_wave_pdu(s, format->nBlockAlign))
			return ERROR_INTERNAL_ERROR;

		end = Stream_GetPosition(s);
		Stream_SetPosition(s, 2);
		Stream_Write_UINT16(s, end - 4);
		context->block_no = (context->block_no + 1) % 256;
		rc = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR)Stream_Buffer(s), end,
		                            &written);

		if (!rc || (end != written))
		{
			WLog_ERR(TAG,
			         "WTSVirtualChannelWrite failed! [stream length=%" PRIdz " - written=%" PRIu32,
			         end, written);
			error = ERROR_INTERNAL_ERROR;
		}
	}

	Stream_SetPosition(s, 0);
	context->priv->out_pending_frames = 0;
	return error;
}

/* Wrapper function to send WAVE or WAVE2 PDU depending on client connected */
static UINT rdpsnd_server_send_audio_pdu(RdpsndServerContext* context, UINT16 wTimestamp)
{
	if (context->clientVersion >= CHANNEL_VERSION_WIN_8)
		return rdpsnd_server_send_wave2_pdu(context, wTimestamp);
	else
		return rdpsnd_server_send_wave_pdu(context, wTimestamp);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_send_samples(RdpsndServerContext* context, const void* buf,
                                       size_t nframes, UINT16 wTimestamp)
{
	UINT error = CHANNEL_RC_OK;
	EnterCriticalSection(&context->priv->lock);

	if (context->selected_client_format >= context->num_client_formats)
	{
		/* It's possible while format negotiation has not been done */
		WLog_WARN(TAG, "Drop samples because client format has not been negotiated.");
		error = ERROR_NOT_READY;
		goto out;
	}

	while (nframes > 0)
	{
		const size_t cframes =
		    MIN(nframes, context->priv->out_frames - context->priv->out_pending_frames);
		size_t cframesize = cframes * context->priv->src_bytes_per_frame;
		CopyMemory(context->priv->out_buffer +
		               (context->priv->out_pending_frames * context->priv->src_bytes_per_frame),
		           buf, cframesize);
		buf = (const BYTE*)buf + cframesize;
		nframes -= cframes;
		context->priv->out_pending_frames += cframes;

		if (context->priv->out_pending_frames >= context->priv->out_frames)
		{
			if ((error = rdpsnd_server_send_audio_pdu(context, wTimestamp)))
			{
				WLog_ERR(TAG, "rdpsnd_server_send_audio_pdu failed with error %" PRIu32 "", error);
				break;
			}
		}
	}

out:
	LeaveCriticalSection(&context->priv->lock);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_set_volume(RdpsndServerContext* context, int left, int right)
{
	size_t pos;
	BOOL status;
	ULONG written;
	wStream* s = context->priv->rdpsnd_pdu;
	Stream_Write_UINT8(s, SNDC_SETVOLUME);
	Stream_Write_UINT8(s, 0);
	Stream_Seek_UINT16(s);
	Stream_Write_UINT16(s, left);
	Stream_Write_UINT16(s, right);
	pos = Stream_GetPosition(s);
	Stream_SetPosition(s, 2);
	Stream_Write_UINT16(s, pos - 4);
	Stream_SetPosition(s, pos);
	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR)Stream_Buffer(s),
	                                Stream_GetPosition(s), &written);
	Stream_SetPosition(s, 0);
	return status ? CHANNEL_RC_OK : ERROR_INTERNAL_ERROR;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_close(RdpsndServerContext* context)
{
	size_t pos;
	BOOL status;
	ULONG written;
	wStream* s = context->priv->rdpsnd_pdu;
	UINT error = CHANNEL_RC_OK;
	EnterCriticalSection(&context->priv->lock);

	if (context->priv->out_pending_frames > 0)
	{
		if (context->selected_client_format >= context->num_client_formats)
		{
			WLog_ERR(TAG, "Pending audio frame exists while no format selected.");
			error = ERROR_INVALID_DATA;
		}
		else if ((error = rdpsnd_server_send_audio_pdu(context, 0)))
		{
			WLog_ERR(TAG, "rdpsnd_server_send_audio_pdu failed with error %" PRIu32 "", error);
		}
	}

	LeaveCriticalSection(&context->priv->lock);

	if (error)
		return error;

	context->selected_client_format = 0xFFFF;
	Stream_Write_UINT8(s, SNDC_CLOSE);
	Stream_Write_UINT8(s, 0);
	Stream_Seek_UINT16(s);
	pos = Stream_GetPosition(s);
	Stream_SetPosition(s, 2);
	Stream_Write_UINT16(s, pos - 4);
	Stream_SetPosition(s, pos);
	status = WTSVirtualChannelWrite(context->priv->ChannelHandle, (PCHAR)Stream_Buffer(s),
	                                Stream_GetPosition(s), &written);
	Stream_SetPosition(s, 0);
	return status ? CHANNEL_RC_OK : ERROR_INTERNAL_ERROR;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_start(RdpsndServerContext* context)
{
	void* buffer = NULL;
	DWORD bytesReturned;
	RdpsndServerPrivate* priv = context->priv;
	UINT error = ERROR_INTERNAL_ERROR;
	priv->ChannelHandle =
	    WTSVirtualChannelOpen(context->vcm, WTS_CURRENT_SESSION, RDPSND_CHANNEL_NAME);

	if (!priv->ChannelHandle)
	{
		WLog_ERR(TAG, "WTSVirtualChannelOpen failed!");
		return ERROR_INTERNAL_ERROR;
	}

	if (!WTSVirtualChannelQuery(priv->ChannelHandle, WTSVirtualEventHandle, &buffer,
	                            &bytesReturned) ||
	    (bytesReturned != sizeof(HANDLE)))
	{
		WLog_ERR(TAG,
		         "error during WTSVirtualChannelQuery(WTSVirtualEventHandle) or invalid returned "
		         "size(%" PRIu32 ")",
		         bytesReturned);

		if (buffer)
			WTSFreeMemory(buffer);

		goto out_close;
	}

	CopyMemory(&priv->channelEvent, buffer, sizeof(HANDLE));
	WTSFreeMemory(buffer);
	priv->rdpsnd_pdu = Stream_New(NULL, 4096);

	if (!priv->rdpsnd_pdu)
	{
		WLog_ERR(TAG, "Stream_New failed!");
		error = CHANNEL_RC_NO_MEMORY;
		goto out_close;
	}

	if (!InitializeCriticalSectionEx(&context->priv->lock, 0, 0))
	{
		WLog_ERR(TAG, "InitializeCriticalSectionEx failed!");
		goto out_pdu;
	}

	if ((error = rdpsnd_server_send_formats(context, context->priv->rdpsnd_pdu)))
	{
		WLog_ERR(TAG, "rdpsnd_server_send_formats failed with error %" PRIu32 "", error);
		goto out_lock;
	}

	if (priv->ownThread)
	{
		context->priv->StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

		if (!context->priv->StopEvent)
		{
			WLog_ERR(TAG, "CreateEvent failed!");
			goto out_lock;
		}

		context->priv->Thread =
		    CreateThread(NULL, 0, rdpsnd_server_thread, (void*)context, 0, NULL);

		if (!context->priv->Thread)
		{
			WLog_ERR(TAG, "CreateThread failed!");
			goto out_stopEvent;
		}
	}

	return CHANNEL_RC_OK;
out_stopEvent:
	CloseHandle(context->priv->StopEvent);
	context->priv->StopEvent = NULL;
out_lock:
	DeleteCriticalSection(&context->priv->lock);
out_pdu:
	Stream_Free(context->priv->rdpsnd_pdu, TRUE);
	context->priv->rdpsnd_pdu = NULL;
out_close:
	WTSVirtualChannelClose(context->priv->ChannelHandle);
	context->priv->ChannelHandle = NULL;
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT rdpsnd_server_stop(RdpsndServerContext* context)
{
	UINT error = CHANNEL_RC_OK;
	if (!context->priv->StopEvent)
		return error;

	if (context->priv->ownThread)
	{
		if (context->priv->StopEvent)
		{
			SetEvent(context->priv->StopEvent);

			if (WaitForSingleObject(context->priv->Thread, INFINITE) == WAIT_FAILED)
			{
				error = GetLastError();
				WLog_ERR(TAG, "WaitForSingleObject failed with error %" PRIu32 "!", error);
				return error;
			}

			CloseHandle(context->priv->Thread);
			CloseHandle(context->priv->StopEvent);
			context->priv->Thread = NULL;
			context->priv->StopEvent = NULL;
		}
	}

	DeleteCriticalSection(&context->priv->lock);

	if (context->priv->rdpsnd_pdu)
	{
		Stream_Free(context->priv->rdpsnd_pdu, TRUE);
		context->priv->rdpsnd_pdu = NULL;
	}

	if (context->priv->ChannelHandle)
	{
		WTSVirtualChannelClose(context->priv->ChannelHandle);
		context->priv->ChannelHandle = NULL;
	}

	return error;
}

RdpsndServerContext* rdpsnd_server_context_new(HANDLE vcm)
{
	RdpsndServerContext* context;
	RdpsndServerPrivate* priv;
	context = (RdpsndServerContext*)calloc(1, sizeof(RdpsndServerContext));

	if (!context)
	{
		WLog_ERR(TAG, "calloc failed!");
		return NULL;
	}

	context->vcm = vcm;
	context->Start = rdpsnd_server_start;
	context->Stop = rdpsnd_server_stop;
	context->selected_client_format = 0xFFFF;
	context->Initialize = rdpsnd_server_initialize;
	context->SelectFormat = rdpsnd_server_select_format;
	context->SendSamples = rdpsnd_server_send_samples;
	context->SetVolume = rdpsnd_server_set_volume;
	context->Close = rdpsnd_server_close;
	context->priv = priv = (RdpsndServerPrivate*)calloc(1, sizeof(RdpsndServerPrivate));

	if (!priv)
	{
		WLog_ERR(TAG, "calloc failed!");
		goto out_free;
	}

	priv->dsp_context = freerdp_dsp_context_new(TRUE);

	if (!priv->dsp_context)
	{
		WLog_ERR(TAG, "freerdp_dsp_context_new failed!");
		goto out_free_priv;
	}

	priv->input_stream = Stream_New(NULL, 4);

	if (!priv->input_stream)
	{
		WLog_ERR(TAG, "Stream_New failed!");
		goto out_free_dsp;
	}

	priv->expectedBytes = 4;
	priv->waitingHeader = TRUE;
	priv->ownThread = TRUE;
	return context;
out_free_dsp:
	freerdp_dsp_context_free(priv->dsp_context);
out_free_priv:
	free(context->priv);
out_free:
	free(context);
	return NULL;
}

void rdpsnd_server_context_reset(RdpsndServerContext* context)
{
	context->priv->expectedBytes = 4;
	context->priv->waitingHeader = TRUE;
	Stream_SetPosition(context->priv->input_stream, 0);
}

void rdpsnd_server_context_free(RdpsndServerContext* context)
{
	if (!context)
		return;

	rdpsnd_server_stop(context);

	free(context->priv->out_buffer);

	if (context->priv->dsp_context)
		freerdp_dsp_context_free(context->priv->dsp_context);

	if (context->priv->input_stream)
		Stream_Free(context->priv->input_stream, TRUE);

	free(context->server_formats);
	free(context->client_formats);
	free(context->priv);
	free(context);
}

HANDLE rdpsnd_server_get_event_handle(RdpsndServerContext* context)
{
	return context->priv->channelEvent;
}

/*
 * Handle rpdsnd messages - server side
 *
 * @param Server side context
 *
 * @return 0 on success
 * 		   ERROR_NO_DATA if no data could be read this time
 *         otherwise error
 */
/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
UINT rdpsnd_server_handle_messages(RdpsndServerContext* context)
{
	DWORD bytesReturned;
	UINT ret = CHANNEL_RC_OK;
	RdpsndServerPrivate* priv = context->priv;
	wStream* s = priv->input_stream;

	if (!WTSVirtualChannelRead(priv->ChannelHandle, 0, (PCHAR)Stream_Pointer(s),
	                           priv->expectedBytes, &bytesReturned))
	{
		if (GetLastError() == ERROR_NO_DATA)
			return ERROR_NO_DATA;

		WLog_ERR(TAG, "channel connection closed");
		return ERROR_INTERNAL_ERROR;
	}

	priv->expectedBytes -= bytesReturned;
	Stream_Seek(s, bytesReturned);

	if (priv->expectedBytes)
		return CHANNEL_RC_OK;

	Stream_SealLength(s);
	Stream_SetPosition(s, 0);

	if (priv->waitingHeader)
	{
		/* header case */
		Stream_Read_UINT8(s, priv->msgType);
		Stream_Seek_UINT8(s); /* bPad */
		Stream_Read_UINT16(s, priv->expectedBytes);
		priv->waitingHeader = FALSE;
		Stream_SetPosition(s, 0);

		if (priv->expectedBytes)
		{
			if (!Stream_EnsureCapacity(s, priv->expectedBytes))
			{
				WLog_ERR(TAG, "Stream_EnsureCapacity failed!");
				return CHANNEL_RC_NO_MEMORY;
			}

			return CHANNEL_RC_OK;
		}
	}

	/* when here we have the header + the body */
#ifdef WITH_DEBUG_SND
	WLog_DBG(TAG, "message type %" PRIu8 "", priv->msgType);
#endif
	priv->expectedBytes = 4;
	priv->waitingHeader = TRUE;

	switch (priv->msgType)
	{
		case SNDC_WAVECONFIRM:
			ret = rdpsnd_server_recv_waveconfirm(context, s);
			break;

		case SNDC_FORMATS:
			ret = rdpsnd_server_recv_formats(context, s);

			if ((ret == CHANNEL_RC_OK) && (context->clientVersion < CHANNEL_VERSION_WIN_7))
				IFCALL(context->Activated, context);

			break;

		case SNDC_QUALITYMODE:
			ret = rdpsnd_server_recv_quality_mode(context, s);
			Stream_SetPosition(s,
			                   0); /* in case the Activated callback tries to treat some messages */

			if ((ret == CHANNEL_RC_OK) && (context->clientVersion >= CHANNEL_VERSION_WIN_7))
				IFCALL(context->Activated, context);

			break;

		default:
			WLog_ERR(TAG, "UNKNOWN MESSAGE TYPE!! (0x%02" PRIX8 ")", priv->msgType);
			ret = ERROR_INVALID_DATA;
			break;
	}

	Stream_SetPosition(s, 0);
	return ret;
}
