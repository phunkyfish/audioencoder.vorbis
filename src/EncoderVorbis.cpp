/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include <algorithm>
#include <kodi/addon-instance/AudioEncoder.h>
#include <ogg/ogg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vorbis/vorbisenc.h>

static const size_t OGG_BLOCK_FRAMES = 1024; // number of frames to encode at a time

class ATTR_DLL_LOCAL CEncoderVorbis : public kodi::addon::CInstanceAudioEncoder
{
public:
  CEncoderVorbis(const kodi::addon::IInstanceInfo& instance);
  ~CEncoderVorbis() override;

  bool Start(const kodi::addon::AudioEncoderInfoTag& tag) override;
  ssize_t Encode(const uint8_t* stream, size_t numBytesRead) override;
  bool Finish() override;

private:
  vorbis_info m_vorbisInfo; ///< struct that stores all the static vorbis bitstream settings
  vorbis_dsp_state m_vorbisDspState; ///< central working state for the packet->PCM decoder
  vorbis_block m_vorbisBlock; ///< local working space for packet->PCM decode

  ogg_stream_state m_oggStreamState; ///< take physical pages, weld into a logical stream of packets

  bool m_inited; ///< whether Init() was successful

  int m_preset;
  int m_bitrate;
};

CEncoderVorbis::CEncoderVorbis(const kodi::addon::IInstanceInfo& instance)
  : CInstanceAudioEncoder(instance), m_inited(false), m_preset(-1)
{
  // create encoder context
  vorbis_info_init(&m_vorbisInfo);

  int value = kodi::addon::GetSettingInt("preset");
  if (value == 0)
    m_preset = 4;
  else if (value == 1)
    m_preset = 5;
  else if (value == 2)
    m_preset = 7;

  m_bitrate = 128 + 32 * kodi::addon::GetSettingInt("bitrate");
}

CEncoderVorbis::~CEncoderVorbis()
{
  /* clean up and exit. vorbis_info_clear() must be called last */
  if (m_inited)
  {
    ogg_stream_clear(&m_oggStreamState);
    vorbis_block_clear(&m_vorbisBlock);
    vorbis_dsp_clear(&m_vorbisDspState);
  }
  vorbis_info_clear(&m_vorbisInfo);
}

bool CEncoderVorbis::Start(const kodi::addon::AudioEncoderInfoTag& tag)
{
  // we accept only 2 ch 16 bit atm
  if (tag.GetChannels() != 2 || tag.GetBitsPerSample() != 16)
  {
    kodi::Log(ADDON_LOG_ERROR, "Invalid input format to encode");
    return false;
  }

  if (m_preset == -1)
    vorbis_encode_init(&m_vorbisInfo, tag.GetChannels(), tag.GetSamplerate(), -1, m_bitrate * 1000,
                       -1);
  else
    vorbis_encode_init_vbr(&m_vorbisInfo, tag.GetChannels(), tag.GetSamplerate(),
                           float(m_preset) / 10.0f);

  /* add a comment */
  vorbis_comment comm;
  vorbis_comment_init(&comm);
  vorbis_comment_add_tag(&comm, "comment", tag.GetComment().c_str());
  vorbis_comment_add_tag(&comm, "artist", tag.GetArtist().c_str());
  vorbis_comment_add_tag(&comm, "title", tag.GetTitle().c_str());
  vorbis_comment_add_tag(&comm, "album", tag.GetAlbum().c_str());
  vorbis_comment_add_tag(&comm, "albumartist", tag.GetAlbumArtist().c_str());
  vorbis_comment_add_tag(&comm, "genre", tag.GetGenre().c_str());
  vorbis_comment_add_tag(&comm, "tracknumber", std::to_string(tag.GetTrack()).c_str());
  vorbis_comment_add_tag(&comm, "date", tag.GetReleaseDate().c_str());

  /* set up the analysis state and auxiliary encoding storage */
  vorbis_analysis_init(&m_vorbisDspState, &m_vorbisInfo);

  vorbis_block_init(&m_vorbisDspState, &m_vorbisBlock);

  /* set up our packet->stream encoder */
  /* pick a random serial number; that way we can more likely build
  chained streams just by concatenation */
  srand((unsigned int)time(nullptr));
  ogg_stream_init(&m_oggStreamState, rand());

  /* write out the metadata */
  ogg_packet header;
  ogg_packet header_comm;
  ogg_packet header_code;
  ogg_page page;

  vorbis_analysis_headerout(&m_vorbisDspState, &comm, &header, &header_comm, &header_code);

  ogg_stream_packetin(&m_oggStreamState, &header);
  ogg_stream_packetin(&m_oggStreamState, &header_comm);
  ogg_stream_packetin(&m_oggStreamState, &header_code);

  while (1)
  {
    /* This ensures the actual
     * audio data will start on a new page, as per spec
     */
    int result = ogg_stream_flush(&m_oggStreamState, &page);
    if (result == 0)
      break;
    Write(page.header, page.header_len);
    Write(page.body, page.body_len);
  }
  vorbis_comment_clear(&comm);

  m_inited = true;
  return true;
}

ssize_t CEncoderVorbis::Encode(const uint8_t* stream, size_t numBytesRead)
{
  int eos = 0;

  size_t bytes_left = numBytesRead;
  while (bytes_left)
  {
    const int channels = 2;
    const int bits_per_channel = 16;

    float** buffer = vorbis_analysis_buffer(&m_vorbisDspState, OGG_BLOCK_FRAMES);

    /* uninterleave samples */

    size_t bytes_per_frame = channels * (bits_per_channel >> 3);
    size_t frames = std::min(bytes_left / bytes_per_frame, OGG_BLOCK_FRAMES);

    const int16_t* buf = reinterpret_cast<const int16_t*>(stream);
    for (int i = 0; i < frames; i++)
    {
      for (int j = 0; j < channels; j++)
        buffer[j][i] = (*buf++) / 32768.0f;
    }
    stream += frames * bytes_per_frame;
    bytes_left -= frames * bytes_per_frame;

    /* tell the library how much we actually submitted */
    vorbis_analysis_wrote(&m_vorbisDspState, frames);

    /* vorbis does some data preanalysis, then divvies up blocks for
    more involved (potentially parallel) processing.  Get a single
    block for encoding now */
    while (vorbis_analysis_blockout(&m_vorbisDspState, &m_vorbisBlock) == 1)
    {
      /* analysis, assume we want to use bitrate management */
      vorbis_analysis(&m_vorbisBlock, NULL);
      vorbis_bitrate_addblock(&m_vorbisBlock);

      ogg_packet packet;
      ogg_page page;
      while (vorbis_bitrate_flushpacket(&m_vorbisDspState, &packet))
      {
        /* weld the packet into the bitstream */
        ogg_stream_packetin(&m_oggStreamState, &packet);

        /* write out pages (if any) */
        while (!eos)
        {
          int result = ogg_stream_pageout(&m_oggStreamState, &page);
          if (result == 0)
            break;
          Write(page.header, page.header_len);
          Write(page.body, page.body_len);

          /* this could be set above, but for illustrative purposes, I do
          it here (to show that vorbis does know where the stream ends) */
          if (ogg_page_eos(&page))
            eos = 1;
        }
      }
    }
  }

  // return bytes consumed
  return numBytesRead - bytes_left;
}

bool CEncoderVorbis::Finish()
{
  int eos = 0;
  // tell vorbis we are encoding the end of the stream
  vorbis_analysis_wrote(&m_vorbisDspState, 0);
  while (vorbis_analysis_blockout(&m_vorbisDspState, &m_vorbisBlock) == 1)
  {
    /* analysis, assume we want to use bitrate management */
    vorbis_analysis(&m_vorbisBlock, nullptr);
    vorbis_bitrate_addblock(&m_vorbisBlock);

    ogg_packet packet;
    ogg_page page;
    while (vorbis_bitrate_flushpacket(&m_vorbisDspState, &packet))
    {
      /* weld the packet into the bitstream */
      ogg_stream_packetin(&m_oggStreamState, &packet);

      /* write out pages (if any) */
      while (!eos)
      {
        int result = ogg_stream_pageout(&m_oggStreamState, &page);
        if (result == 0)
          break;
        Write(page.header, page.header_len);
        Write(page.body, page.body_len);

        /* this could be set above, but for illustrative purposes, I do
        it here (to show that vorbis does know where the stream ends) */
        if (ogg_page_eos(&page))
          eos = 1;
      }
    }
  }
  return true;
}

//------------------------------------------------------------------------------

class ATTR_DLL_LOCAL CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon() = default;
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override;
};

ADDON_STATUS CMyAddon::CreateInstance(const kodi::addon::IInstanceInfo& instance,
                                      KODI_ADDON_INSTANCE_HDL& hdl)
{
  hdl = new CEncoderVorbis(instance);
  return ADDON_STATUS_OK;
}

ADDONCREATOR(CMyAddon)
