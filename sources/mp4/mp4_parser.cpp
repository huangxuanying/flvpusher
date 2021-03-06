#include <cstdlib>
#include <xlog.h>

#include "mp4_parser.h"
#include "common/common.h"

//#define XDEBUG

namespace flvpusher {

MP4Parser::MP4Parser() :
  m_box(NULL),
  m_parsed_track(0),
  m_mp4(NULL)
{
}

MP4Parser::~MP4Parser()
{
  mp4_free(m_mp4);
  Box::free_box(m_box);
  m_file.close();
}

int MP4Parser::set_file(const std::string &mp4_file)
{
  if (!m_file.open(mp4_file, "rb"))
    return -1;

  if (Box::parse_box(m_box, &m_file, m_file.size()) < 0)
    return -1;

  if (init() < 0)
    return -1;

  return 0;
}

int MP4Parser::init()
{
  bzero(&m_track, sizeof(m_track));
  Track *trak = &m_track[VIDEO];
  if (init_tracks_from_box(m_box, trak) < 0) {
    LOGE("Init mp4's tracks failed");
    return -1;
  }

  // Make sure the video and audio tracks are valid
  if (m_parsed_track < NB_TRACK || // Only one track in file, not support
      !m_track[AUDIO].track_ID || !m_track[VIDEO].track_ID || // Missing audio or video track
      !m_track[AUDIO].mp4a || !m_track[VIDEO].avc1) { // Either video track's codec isn't h264 or
    // audio track's codec isn't mp4a
    LOGE("Invalid audio(%u:%p) track or video(%u:%p) track",
         m_track[AUDIO].track_ID, m_track[AUDIO].mp4a,
         m_track[VIDEO].track_ID, m_track[VIDEO].avc1);
    return -1;
  }

  bzero(&m_status, sizeof(m_status));
  ReadStatus *rstatus;

  // Compute the shift timebase for tracks
  // NOTE: We manage only one case : when video does not start at 
  // 0, we delay all others tracks by the amount indicated
  trak = &m_track[VIDEO];
  rstatus = &m_status[VIDEO];
  if (trak->elst && trak->elst->entry_count == 1) {
    // Only support 1 entry in "elst" box
    int64_t media_time =
      trak->elst->ver==1 ? trak->elst->elst_entry[0].media_time.i64 :
      trak->elst->elst_entry[0].media_time.i32;
    if (media_time > 0) { // Only delay audio tracks
      // Update audio track's shift timebase
      m_status[AUDIO].shift_time = media_time*1000/trak->timescale;
    }
  }
  frac_init(&rstatus->dts, 0, 0, trak->timescale);

  trak = &m_track[AUDIO];
  rstatus = &m_status[AUDIO];
  uint8_t samplerate_idx = str_to_samplerate_idx(
      STR(sprintf_("%d", trak->mp4a->samplerate>>16)));
  if (trak->esds->to_confirm) {
    // Confirm the asc which is generated by hand
    generate_asc(trak->esds->asc,
                 0x02/*LC fixed*/, samplerate_idx, trak->mp4a->channelcount);

    trak->esds->to_confirm = false;
  } else {
    if (samplerate_idx != trak->esds->samplerate_idx ||
        trak->mp4a->channelcount != trak->esds->channel) {
#ifdef XDEBUG
      LOGD("|esds|:idx=%u, channel=%u != |mp4a|:idx=%u, channel=%u",
           trak->esds->samplerate_idx, trak->esds->channel,
           samplerate_idx, trak->mp4a->channelcount);
#endif
#if 0
      // |mp4a| wins
      generate_asc(trak->esds->asc,
          trak->esds->audio_object_type,
          samplerate_idx, trak->mp4a->channelcount);
#else
      // Use the asc in |esds| by default
      LOGW("AudioSpecificConfig in |esds| is different from |mp4a|");
#endif
    }
  }
  frac_init(&rstatus->dts, 0, 0, trak->timescale);
  // Only audio has shift_time
  if (rstatus->shift_time > 0)
    rstatus->dts.val += rstatus->shift_time;
  return 0;
}

int MP4Parser::get_resolution(uint32_t &width, uint32_t &height) const
{
  const Track *trak = &m_track[VIDEO];
  if (trak->avc1->width && trak->avc1->height) {
    width = trak->avc1->width;
    height = trak->avc1->height;
    return 0;
  }
  return -1;
}

AVRational MP4Parser::get_vtime_base() const
{
  const Track *trak = &m_track[VIDEO];
  return (AVRational) {
    (int) (trak->duration/trak->timescale),
    (int) trak->stsz->sample_count
  };
}

int MP4Parser::init_tracks_from_box(Box *box, Track *&trak)
{
  Box *p = box;
  while (p != NULL) {
    if (p->typ == MKTAG4('t', 'k', 'h', 'd')) {
      if (++m_parsed_track > NB_TRACK) {
        // Already got first NB_TRACK tracks' info
        break;
      }

      TrackHeaderBox *tkhd = (TrackHeaderBox *) p;
      // Update track index to init
      if (tkhd->volume != 0) {
        trak = &m_track[AUDIO];
        trak->video_track = false;
      } else {
        trak = &m_track[VIDEO];
        trak->video_track = true;
      }
      trak->track_ID = tkhd->track_ID;
    } else if (p->typ == MKTAG4('m', 'd', 'h', 'd')) {
      MovieHeaderBox *mdhd = (MovieHeaderBox *) p;
      trak->timescale = mdhd->timescale;
      trak->duration =
        mdhd->ver == 1 ? mdhd->duration.u64 : mdhd->duration.u32;
    } else if (p->typ == MKTAG4('a', 'v', 'c', '1')) {
      trak->avc1 = (VisualSampleEntry *) p;
    } else if (p->typ == MKTAG4('a', 'v', 'c', 'C')) {
      trak->avcC = (avcCBox *) p;
    } else if (p->typ == MKTAG4('e', 's', 'd', 's')) {
      trak->esds = (esdsBox *) p;
    } else if (p->typ == MKTAG4('s', 't', 't', 's')) {
      trak->stts = (TimeToSampleBox *) p;
    } else if (p->typ == MKTAG4('c', 't', 't', 's')) {
      trak->ctts = (CompositionOffsetBox *) p;
    } else if (p->typ == MKTAG4('s', 't', 's', 'c')) {
      trak->stsc = (SampleToChunkBox *) p;
    } else if (p->typ == MKTAG4('s', 't', 's', 'z')) {
      trak->stsz = (SampleSizeBox *) p;
    } else if (p->typ == MKTAG4('s', 't', 'c', 'o')) {
      trak->stco = (ChunkOffsetBox *) p;
    } else if (p->typ == MKTAG4('c', 'o', '6', '4')) {
      trak->large_offset = true;
      trak->co64 = (ChunkLargeOffsetBox *) p;
    } else if (p->typ == MKTAG4('e', 'l', 's', 't')) {
      trak->elst = (EditListBox *) p;
    } else if (p->typ == MKTAG4('m', 'p', '4', 'a')) {
      trak->mp4a = (mp4aBox *) p;
    }

    // Handle box's sub-boxes
    init_tracks_from_box(p->sub, trak);

    // Handle the next box
    p = p->next;
  }
  return 0;
}

uint32_t MP4Parser::chunk_containing_sample(uint32_t sample_idx,
                                            const SampleToChunkBox *stsc, uint32_t &first_sample_in_chunk,
                                            ReadStatus::LocateChunkCache *lcc)
{
  bool is_cont =
    lcc && lcc->cached_sample_idx + 1 == sample_idx ? true : false;

  // Cover only 1 entry in "stsc" box
  if (is_cont && !lcc->cached_entry_idx)
    is_cont = false;

  uint32_t prev_chunk =
    stsc->first_chunk[is_cont ? lcc->cached_entry_idx - 1: 0];
  uint32_t prev_chunk_samples =
    stsc->sample_per_chunk[is_cont ? lcc->cached_entry_idx - 1: 0];
  uint32_t total_samples =
    is_cont ? lcc->cached_total_samples : 0;
  uint32_t chunk;
  uint32_t range_samples;
  for (uint32_t i = is_cont ? lcc->cached_entry_idx : 1;
      i < stsc->entry_count;
      ++i) {
    chunk = stsc->first_chunk[i];
    range_samples = (chunk - prev_chunk)*prev_chunk_samples;

    if (lcc) {
      lcc->cached_entry_idx = i;
      lcc->cached_total_samples = total_samples;
    }

    if (sample_idx < total_samples + range_samples) {
      // Found sample in chunk [prev_chunk, chunk)
      break;
    }

    total_samples += range_samples;
    prev_chunk = stsc->first_chunk[i];
    prev_chunk_samples = stsc->sample_per_chunk[i];
  }

  // Calc the chunk containing sample_idx
  chunk =
    prev_chunk + (sample_idx - total_samples)/prev_chunk_samples;

  // Get first sample in this chunk
  first_sample_in_chunk =
    total_samples + (chunk - prev_chunk)*prev_chunk_samples;

  if (lcc)
    lcc->cached_sample_idx = sample_idx;

  return chunk - 1; // Adjust to begin from 0
}

int MP4Parser::locate_sample(Track *trak, ReadStatus *rstatus,
                             SampleEntry *sentry)
{
  if (rstatus->sample_idx == trak->stsz->sample_count) {
    // No more frames to read
    return -1;
  }

  if (rstatus->stts.offset ==
      trak->stts->sample_count[rstatus->stts.cnt]) {
    // Handle next sample_count in "stts"
    ++rstatus->stts.cnt;
    rstatus->stts.offset = 0;
  }

  if (trak->ctts && rstatus->ctts.offset ==
                    trak->ctts->sample_count[rstatus->ctts.cnt]) {
    // Handle next sample_count in "ctts"
    ++rstatus->ctts.cnt;
    rstatus->ctts.offset = 0;
  }

  // Get chunk containing this sample and chunk's first sample idx
  uint32_t first_sample_in_chunk;
  uint32_t chunk = chunk_containing_sample(
      rstatus->sample_idx, trak->stsc, first_sample_in_chunk,
      &rstatus->lcc);
#ifdef XDEBUG
  LOGI("sample_idx: %u, chunk: %u, first_sample_in_chunk: %u",
       rstatus->sample_idx, chunk, first_sample_in_chunk);
#endif

  // Get sample's offset in file
  if (trak->large_offset)
    rstatus->sample_offset = trak->co64->chunk_offset[chunk];
  else
    rstatus->sample_offset = trak->stco->chunk_offset[chunk];
  if (first_sample_in_chunk != rstatus->sample_idx) {
    for (uint32_t i = first_sample_in_chunk;
         i < rstatus->sample_idx;
         ++i) {
      rstatus->sample_offset += trak->stsz->entry_size[i];
    }
  }
  // Get sample's size
  uint32_t sample_sz =
    trak->stsz->entry_size[rstatus->sample_idx];
#ifdef XDEBUG
  LOGI("sample_idx: %u, FILE offset: 0x%08x, size: 0x%08x",
       rstatus->sample_idx,
       rstatus->sample_offset,
       sample_sz);
#endif

  if (sentry) {
    sentry->decode_ts = rstatus->dts.val;
    sentry->sample_idx = rstatus->sample_idx;
    sentry->sample_offset = rstatus->sample_offset;
    sentry->sample_sz = sample_sz;
  }

  // Update the next frame's decode timestamp
  frac_add(&rstatus->dts,
           trak->stts->sample_delta[rstatus->stts.cnt]*1000);
  if (trak->ctts && sentry) {
    sentry->composition_time =
      trak->ctts->sample_offset[rstatus->ctts.cnt]*1000/trak->timescale;
  }
  // Handle next sample in this sample_count in "stts"
  ++rstatus->stts.offset;
  if (trak->ctts) {
    // Handle next sample in this sample_count in "ctts"
    ++rstatus->ctts.offset;
  }
  // Update the sample idx to read next time
  ++rstatus->sample_idx;
  return 0;
}

int MP4Parser::read_frame(File &file, Track *trak,
                          SampleEntry *sentry, Frame *f)
{
  if (!sentry || !f)
    return -1;

  int64_t decode_ts = sentry->decode_ts;
  uint32_t sample_idx = sentry->sample_idx;
  off_t sample_offset = sentry->sample_offset;
  uint32_t sample_sz = sentry->sample_sz;

  // Seek to where it is and read
  if (!file.seek_to(sample_offset)) {
    LOGE("Seek to sample_offset(%ld) failed", sample_offset);
    return -1;
  }

  // Read the frame
  uint8_t *dat = (uint8_t *) malloc(sample_sz+7); // 7 is for adts-header length
  if (!dat) {
    LOGE("malloc for sample failed: %s", ERRNOMSG);
    return -1;
  }
  if (!file.read_buffer(trak->video_track ? dat : dat+7, sample_sz)) {
    LOGE("Read frame failed, sample_offset: %ld, sample_idx: %u, sample_sz: 0x%08x",
         sample_offset, sample_idx, sample_sz);
    SAFE_FREE(dat);
    return -1;
  }

  if (trak->video_track) { // Video track
    bool is_keyframe = false; // Whether this is a keyframe
    bool got_sps = false;     // Keyframe whether has sps & pps

    // Convert 4bytes nalu-length to 00 00 00 01
    uint32_t nalu_offset = 0;
    while (nalu_offset < sample_sz) {
      uint32_t nalu_size = ENTOHL(*(uint32_t *)(dat+nalu_offset));

      put_be32(dat+nalu_offset, 0x00000001);

      uint8_t nalu_typ = *(dat+nalu_offset+4)&0x1F;
      if (nalu_typ == 0x07) {
        // Keyframe with sps (pps is also followed)
        got_sps = true;
        is_keyframe = true;
      } else if (!got_sps &&
          (nalu_typ == 0x08 ||
           nalu_typ == 0x06 ||
           nalu_typ == 0x05)) {
        // Keyframe with sps and pps in box "avcC"
        is_keyframe = true;
      }

      // Handle next nalu in this frame
      nalu_offset += nalu_size + 4;
    }

    if (is_keyframe && !got_sps) {
      // Recover sps&pps from box "avcC" to make it a complete I-Frame
      AVCDecorderConfigurationRecord &avc_dcr = trak->avcC->avc_dcr;
      uint8_t *complete_frame = (uint8_t *) malloc (
          4+avc_dcr.sps_length+
          4+avc_dcr.pps_length+
          sample_sz);
      if (!complete_frame) {
        LOGE("malloc for complete_frame failed: %s", ERRNOMSG);
        SAFE_FREE(dat);
        return -1;
      }
      // Copy startcode and sps
      put_be32(complete_frame, 0x00000001);
      memcpy(complete_frame+4,
             avc_dcr.sps, avc_dcr.sps_length);
      // Copy startcode and pps
      put_be32(complete_frame+4+avc_dcr.sps_length, 0x00000001);
      memcpy(complete_frame+4+avc_dcr.sps_length+4,
             avc_dcr.pps, avc_dcr.pps_length);
      // Copy the rest of video frame
      memcpy(complete_frame+4+avc_dcr.sps_length+4+avc_dcr.pps_length,
             dat, sample_sz);
      SAFE_FREE(dat);
      // Update the frame statistics
      sample_sz += 4+avc_dcr.sps_length+4+avc_dcr.pps_length;
      dat = complete_frame;
    }
  } else { // Audio track
    // Add adts header to the begining of frame
    generate_adts_header(trak->esds->asc, sample_sz, dat);
    // Update the frame statistics
    sample_sz += 7;
  }

  return f->make_frame(decode_ts, dat, sample_sz, true, sentry->composition_time);
}

/////////////////////////////////////////////////////////////

int MP4Parser::mp4_init(MP4Context *&mp4, File *file)
{
  FormatContext *ic = (FormatContext *) calloc(1, sizeof(FormatContext));
  if (!ic) goto bail;

  ic->start_time = -1;
  ic->file = file;
  ic->max_interleave_delta = 10000000;
  ic->otime_base = (AVRational) {1, 1000};
  ic->iformat = &mp4_demuxer;
  if (ic->iformat->priv_data_size > 0) {
    ic->priv_data = calloc(1, ic->iformat->priv_data_size);
    if (!ic->priv_data) goto bail;
  }

  mp4 = (MP4Context *) ic->priv_data;
  mp4->stream = ic;
  return 0;

bail:
  if (ic) SAFE_FREE(ic->priv_data);
  SAFE_FREE(ic);
  return -1;
}

void MP4Parser::mp4_free(MP4Context *&mp4)
{
  if (!mp4) return;

  FormatContext *ic = mp4->stream;
  flush_packet_queue(ic);
  for (unsigned i = 0; i < ic->nb_streams; ++i) {
    Stream *st = ic->streams[i];
    if (st->parser) {
      parser_close(st->parser);
      st->parser = NULL;
    }
    SAFE_FREE(st->codec);
    SAFE_FREE(st);
  }
  SAFE_FREE(ic->streams);
  SAFE_FREE(ic);

  SAFE_FREE(mp4);
}

int MP4Parser::process(void *opaque, FrameCb cb)
{
  FormatContext *ic = NULL;

  if (init_ffmpeg_context() < 0) {
    LOGE("init_ffmpeg_context() failed");
    return -1;
  }

  // |ic| is created after init_ffmpeg_context()
  ic = m_mp4->stream;
  ic->cb      = cb;
  ic->opaque  = opaque;

  unsigned i;
  while (!*ic->watch_variable) {
    i = choose_output(ic);

    if (process_input(ic, i) < 0)
      break;
  }

  write_trailer(ic);
  return 0;
}

int MP4Parser::init_ffmpeg_context()
{
  if (mp4_init(m_mp4, &m_file) < 0) {
    LOGE("mp4_init() failed");
    return -1;
  }

  ReadStatus status_bak[2];
  memcpy(&status_bak, &m_status, sizeof(m_status));

  FormatContext *ic = m_mp4->stream;
  ic->watch_variable = interrupt_variable();
  for (unsigned i = 0; i < NB_TRACK; ++i) {
    Stream *st = format_new_stream(ic);
    if (!st) return -1;
    st->id = i;
    Track *trak = &m_track[i];
    st->codec->codec_type =
      trak->video_track ? MEDIA_TYPE_VIDEO : MEDIA_TYPE_AUDIO;
    st->codec->codec_id = 
      trak->video_track ? CODEC_ID_H264 : CODEC_ID_AAC;
    // Parse for audio's sample_rate&channel and video's width&height
    st->need_parsing = 1;
    st->nb_index_entries = trak->stsz->sample_count;
    st->priv_data = this;
    // The read frame's decode timestamp is already in millisecond, 32bits
    priv_set_pts_info(st, 32, 1, 1000);
  }

  if (format_find_stream_info(ic) < 0)
    return -1;

  // For mp4 file, no need to parse Audio&video again
  for (unsigned i = 0; i < ic->nb_streams; ++i)
    ic->streams[i]->need_parsing = 0;
  // Recover the read-status
  memcpy(&m_status, &status_bak, sizeof(status_bak));

  int64_t timestamps = 0;
  if (ic->start_time != -1)
    timestamps += ic->start_time;
  ic->ts_offset = 0 - timestamps;
  return 0;
}

int MP4Parser::mp4_read_packet(FormatContext *s, Packet *pkt)
{
  int selected_stream = -1;
  int64_t best_dts = INT64_MAX;
  MP4Parser *obj = NULL;
  Track *trak = NULL;
  ReadStatus *rstatus = NULL;
  // Read frame with monotonous decode timestamp 
  for (unsigned i = 0; i < s->nb_streams; ++i) {
    Stream *st = s->streams[i];
    obj = (MP4Parser *) st->priv_data;;
    trak = &obj->m_track[st->id];
    rstatus = &obj->m_status[st->id];
    // Already read to the end of this stream, ignore it and try another one
    if (rstatus->sample_idx == trak->stsz->sample_count)
      continue;
    int64_t dts = av_rescale(rstatus->dts.val, AV_TIME_BASE, st->time_base.den);
    if (selected_stream == -1 ||
        ((abs(best_dts - dts) <= 4 && rstatus->sample_offset < obj->m_status[selected_stream].sample_offset) ||
         (abs(best_dts - dts) > 4 && dts < best_dts))) {
      selected_stream = i;
      best_dts = dts;
    }
  }

  // No more frame to be read, parse done
  if (selected_stream < 0)
    return -1;

  trak = &obj->m_track[selected_stream];
  rstatus = &obj->m_status[selected_stream];
  SampleEntry sentry;
  Frame frame;
  if (locate_sample(trak, rstatus, &sentry) < 0 ||
      read_frame(obj->m_file, trak, &sentry, &frame) < 0)
    return -1;

  pkt->stream_index = selected_stream;
  pkt->pts = frame.get_dts() + frame.get_composition_time();
  pkt->dts = frame.get_dts();
  pkt->duration = rstatus->dts.val - pkt->dts;
  pkt->pos = sentry.sample_offset;
  // Hack style, optimize the memory algorithm
  pkt->data = frame.get_data();
  pkt->size = frame.get_data_length();
  frame.set_data(NULL);

#ifdef XDEBUG
  LOGD("%s pkt->pts=%lld, pkt->dts=%lld (composition_time=%u), pkt->size=%d, pkt->duration: %d, current_sample#=%d, total samples#=%d",
       pkt->stream_index == AUDIO ? "AUDIO" : "VIDEO",
       pkt->pts, pkt->dts, frame.get_composition_time(), pkt->size, pkt->duration,
       rstatus->sample_idx,
       trak->stsz->sample_count);
#endif
  return 0;
}

void MP4Parser::print_ReadStatus(const ReadStatus &rs)
{
  LOGD("%u %u %u %u {%d, %d} %u %u {%u, %u, %u} %lld",
       rs.stts.cnt, rs.stts.offset, rs.ctts.cnt, rs.ctts.offset, rs.dts.num, rs.dts.den, rs.shift_time, rs.sample_idx, rs.lcc.cached_sample_idx, rs.lcc.cached_entry_idx, rs.lcc.cached_total_samples, rs.sample_offset);
}

InputFormat MP4Parser::mp4_demuxer = { "mp4|3gp|3gpp", sizeof(MP4Context), mp4_read_packet };

}
