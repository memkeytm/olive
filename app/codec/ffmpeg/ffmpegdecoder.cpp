/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2019 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "ffmpegdecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QtMath>

#include "codec/waveinput.h"
#include "common/define.h"
#include "common/filefunctions.h"
#include "common/functiontimer.h"
#include "common/timecodefunctions.h"
#include "ffmpegcommon.h"
#include "render/diskmanager.h"
#include "render/pixelformat.h"

OLIVE_NAMESPACE_ENTER

QHash< Stream*, QList<FFmpegDecoderInstance*> > FFmpegDecoder::instances_;
QMutex FFmpegDecoder::instance_lock_;

FFmpegDecoder::FFmpegDecoder() :
  scale_ctx_(nullptr),
  scale_divider_(-1)
{
  /*
  // FIXME: Hardcoded, ideally this value is dynamically chosen based on memory restraints
  clear_timer_.setInterval(2000);
  clear_timer_.moveToThread(qApp->thread());
  connect(&clear_timer_, &QTimer::timeout, this, &FFmpegDecoder::ClearTimerEvent);
  */
}

FFmpegDecoder::~FFmpegDecoder()
{
  Close();
}

bool FFmpegDecoder::Open()
{
  QMutexLocker locker(&mutex_);

  if (open_) {
    return true;
  }

  Q_ASSERT(stream());

  // Convert QString to a C string
  QByteArray fn_bytes = stream()->footage()->filename().toUtf8();

  FFmpegDecoderInstance* instance = new FFmpegDecoderInstance(fn_bytes.constData(), stream()->index());

  if (!instance->IsValid()) {
    delete instance;
    return false;
  }

  if (stream()->type() == Stream::kVideo) {
    // Get an Olive compatible AVPixelFormat
    src_pix_fmt_ = static_cast<AVPixelFormat>(instance->stream()->codecpar->format);
    ideal_pix_fmt_ = FFmpegCommon::GetCompatiblePixelFormat(src_pix_fmt_);

    // Determine which Olive native pixel format we retrieved
    // Note that FFmpeg doesn't support float formats
    switch (ideal_pix_fmt_) {
    case AV_PIX_FMT_RGB24:
      native_pix_fmt_ = PixelFormat::PIX_FMT_RGB8;
      break;
    case AV_PIX_FMT_RGBA:
      native_pix_fmt_ = PixelFormat::PIX_FMT_RGBA8;
      break;
    case AV_PIX_FMT_RGB48:
      native_pix_fmt_ = PixelFormat::PIX_FMT_RGB16U;
      break;
    case AV_PIX_FMT_RGBA64:
      native_pix_fmt_ = PixelFormat::PIX_FMT_RGBA16U;
      break;
    default:
      // We should never get here, but just in case...
      qFatal("Invalid output format");
    }

    aspect_ratio_ = instance->sample_aspect_ratio();

    //QMetaObject::invokeMethod(&clear_timer_, "start");
  }

  time_base_ = instance->stream()->time_base;
  start_time_ = instance->stream()->start_time;

  // All allocation succeeded so we set the state to open
  open_ = true;

  {
    QMutexLocker l(&instance_lock_);

    QList<FFmpegDecoderInstance*> list = instances_.value(stream().get());
    list.append(instance);
    instances_.insert(stream().get(), list);
  }

  return true;
}

Decoder::RetrieveState FFmpegDecoder::GetRetrieveState(const rational& time)
{
  QMutexLocker locker(&mutex_);

  if (!open_) {
    return kFailedToOpen;
  }

  if (stream()->type() == Stream::kVideo) {

    // Do nothing

  } else if (stream()->type() == Stream::kAudio) {
    AudioStreamPtr audio_stream = std::static_pointer_cast<AudioStream>(stream());

    if (time > audio_stream->index_length() && !audio_stream->index_done()) {
      return kIndexUnavailable;
    }
  }

  return kReady;
}

FramePtr FFmpegDecoder::RetrieveVideo(const rational &timecode, const int &divider)
{
  QMutexLocker locker(&mutex_);

  if (!open_) {
    qWarning() << "Tried to retrieve video on a decoder that's still closed";
    return nullptr;
  }

  if (stream()->type() != Stream::kVideo) {
    return nullptr;
  }

  int64_t target_ts = Timecode::time_to_timestamp(timecode, time_base_) + start_time_;

  FFmpegDecoderInstance* working_instance = nullptr;
  AVFrame* return_frame = nullptr;

  // Find instance
  do {
    QMutexLocker list_locker(&instance_lock_);

    QList<FFmpegDecoderInstance*> non_ideal_contenders;

    QList<FFmpegDecoderInstance*> instances = instances_.value(stream().get());

    foreach (FFmpegDecoderInstance* i, instances) {
      i->cache_lock()->lock();

      if (i->CacheContainsTime(target_ts)) {

        // Found our instance, allow others to enter the list

        list_locker.unlock();

        // Get the frame from this cache
        return_frame = i->GetFrameFromCache(target_ts);

        // Got our frame, allow cache to continue
        i->cache_lock()->unlock();
        break;

      } else if (i->CacheWillContainTime(target_ts)) {

        // Found our instance, allow others to enter the list
        list_locker.unlock();

        do {
          // Allow instance to continue to the next frame
          i->cache_wait_cond()->wait(i->cache_lock());

          // See if the cache now contains this frame, if so we'll exit this loop
          if (i->CacheContainsTime(target_ts)) {
            return_frame = i->GetFrameFromCache(target_ts);
          }
        } while (!return_frame);

        // Got our frame, allow cache to continue
        i->cache_lock()->unlock();
        break;

      } else if (i->CacheCouldContainTime(target_ts)) {

        // Found our instance, allow others to enter the list
        list_locker.unlock();

        // Wait for this instance to finish working
        while (i->IsWorking()) {
          i->cache_wait_cond()->wait(i->cache_lock());
        }

        // Grab this instance
        working_instance = i;

        // We DON'T unlock here, since we'll be starting our own retrieve
        break;

      } else if (i->IsWorking()) {

        // Ignore currently working instances
        i->cache_lock()->unlock();

      } else if (i->CacheIsEmpty()) {

        // Prioritize this cache over others (leaves this instance LOCKED in case we end up using it later)
        non_ideal_contenders.prepend(i);

      } else {

        // De-prioritize this cache (leaves this instance LOCKED in case we end up using it later)
        non_ideal_contenders.append(i);

      }
    }

    // If we didn't find a suitable contender, grab the first non-suitable and roll with that
    if (!return_frame && !working_instance && !non_ideal_contenders.isEmpty()) {
      working_instance = non_ideal_contenders.takeFirst();
    }

    // For all instances we left locked but didn't end up using, lock them now
    foreach (FFmpegDecoderInstance* unsuitable_instance, non_ideal_contenders) {
      unsuitable_instance->cache_lock()->unlock();
    }
  } while (!return_frame && !working_instance);

  if (!return_frame && working_instance) {

    // This instance SHOULD remain locked from our earlier loop, making this operation safe
    working_instance->SetWorking(true);

    // Retrieve frame
    return_frame = working_instance->RetrieveFrame(target_ts, true);

    // Set working to false and wake any threads waiting
    working_instance->cache_lock()->lock();
    working_instance->SetWorking(false);
    working_instance->cache_wait_cond()->wakeAll();
    working_instance->cache_lock()->unlock();
  }

  // We found the frame, we'll return a copy
  if (return_frame) {
    if (divider != scale_divider_) {
      FreeScaler();
      SetupScaler(divider);
    }

    VideoStream* vs = static_cast<VideoStream*>(stream().get());

    FramePtr copy = Frame::Create();
    copy->set_video_params(VideoRenderingParams(vs->width() / divider,
                                                vs->height() / divider,
                                                native_pix_fmt_));
    copy->set_timestamp(Timecode::timestamp_to_time(target_ts, time_base_));
    copy->set_sample_aspect_ratio(aspect_ratio_);
    copy->allocate();

    // Convert frame to RGBA for the rest of the pipeline
    uint8_t* output_data = reinterpret_cast<uint8_t*>(copy->data());
    int output_linesize = copy->width() * PixelFormat::ChannelCount(native_pix_fmt_) * PixelFormat::BytesPerChannel(native_pix_fmt_);

    sws_scale(scale_ctx_,
              return_frame->data,
              return_frame->linesize,
              0,
              return_frame->height,
              &output_data,
              &output_linesize);

    return copy;
  }

  return nullptr;
}

SampleBufferPtr FFmpegDecoder::RetrieveAudio(const rational &timecode, const rational &length, const AudioRenderingParams &params)
{
  QMutexLocker locker(&mutex_);

  if (!open_) {
    qWarning() << "Tried to retrieve audio on a decoder that's still closed";
    return nullptr;
  }

  if (stream()->type() != Stream::kAudio) {
    return nullptr;
  }

  WaveInput input(GetConformedFilename(params));

  if (input.open()) {
    const AudioRenderingParams& input_params = input.params();

    // Read bytes from wav
    QByteArray packed_data = input.read(input_params.time_to_bytes(timecode), input_params.time_to_bytes(length));
    input.close();

    // Create sample buffer
    SampleBufferPtr sample_buffer = SampleBuffer::CreateFromPackedData(input_params, packed_data);

    return sample_buffer;
  }

  return nullptr;
}

void FFmpegDecoder::Close()
{
  QMutexLocker locker(&mutex_);

  /* FIXME: Consider methods of clearing an instance (whichever is the least useful)
  {
    QMutexLocker l(&instance_lock_);

    QList<FFmpegDecoder*> list = instances_.value(stream().get());
    list.removeOne(this);
    instances_.insert(stream().get(), list);
  }
  */

  ClearResources();

  //clear_timer_.stop();
}

QString FFmpegDecoder::id()
{
  return QStringLiteral("ffmpeg");
}

bool FFmpegDecoder::SupportsVideo()
{
  return true;
}

bool FFmpegDecoder::SupportsAudio()
{
  return true;
}

bool FFmpegDecoder::Probe(Footage *f, const QAtomicInt* cancelled)
{
  if (open_) {
    qWarning() << "Probe must be called while the Decoder is closed";
    return false;
  }

  // Variable for receiving errors from FFmpeg
  int error_code;

  // Result to return
  bool result = false;

  // Convert QString to a C strng
  QByteArray ba = f->filename().toUtf8();
  const char* filename = ba.constData();

  // Open file in a format context
  AVFormatContext* fmt_ctx = nullptr;
  error_code = avformat_open_input(&fmt_ctx, filename, nullptr, nullptr);

  QList<Stream*> streams_that_need_manual_duration;

  // Handle format context error
  if (error_code == 0) {

    // Retrieve metadata about the media
    avformat_find_stream_info(fmt_ctx, nullptr);

    // Dump it into the Footage object
    for (unsigned int i=0;i<fmt_ctx->nb_streams;i++) {

      AVStream* avstream = fmt_ctx->streams[i];

      StreamPtr str;

      if (avstream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {

        // Create a video stream object
        VideoStreamPtr video_stream = std::make_shared<VideoStream>();

        video_stream->set_width(avstream->codecpar->width);
        video_stream->set_height(avstream->codecpar->height);
        video_stream->set_frame_rate(av_guess_frame_rate(fmt_ctx, avstream, nullptr));
        video_stream->set_start_time(avstream->start_time);

        str = video_stream;

      } else if (avstream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {

        // Create an audio stream object
        AudioStreamPtr audio_stream = std::make_shared<AudioStream>();

        uint64_t channel_layout = avstream->codecpar->channel_layout;
        if (!channel_layout) {
          channel_layout = static_cast<uint64_t>(av_get_default_channel_layout(avstream->codecpar->channels));
        }

        audio_stream->set_channel_layout(channel_layout);
        audio_stream->set_channels(avstream->codecpar->channels);
        audio_stream->set_sample_rate(avstream->codecpar->sample_rate);

        str = audio_stream;

      } else {

        // This is data we can't utilize at the moment, but we make a Stream object anyway to keep parity with the file
        str = std::make_shared<Stream>();

        // Set the correct codec type based on FFmpeg's result
        switch (avstream->codecpar->codec_type) {
        case AVMEDIA_TYPE_UNKNOWN:
          str->set_type(Stream::kUnknown);
          break;
        case AVMEDIA_TYPE_DATA:
          str->set_type(Stream::kData);
          break;
        case AVMEDIA_TYPE_SUBTITLE:
          str->set_type(Stream::kSubtitle);
          break;
        case AVMEDIA_TYPE_ATTACHMENT:
          str->set_type(Stream::kAttachment);
          break;
        default:
          // We should never realistically get here, but we make an "invalid" stream just in case
          str->set_type(Stream::kUnknown);
          break;
        }

      }

      str->set_index(avstream->index);
      str->set_timebase(avstream->time_base);
      str->set_duration(avstream->duration);

      // The container/stream info may not contain a duration, so we'll need to manually retrieve it
      if (avstream->duration == AV_NOPTS_VALUE) {
        streams_that_need_manual_duration.append(str.get());
      }

      f->add_stream(str);
    }

    // As long as we can open the container and retrieve information, this was a successful probe
    result = true;
  }

  // If the metadata did not contain a duration, we'll need to loop through the file to retrieve it
  if (!streams_that_need_manual_duration.isEmpty()) {

    AVPacket* pkt = av_packet_alloc();

    QVector<int64_t> durations(streams_that_need_manual_duration.size());
    durations.fill(0);

    while (true) {
      if (cancelled && *cancelled) {
        break;
      }

      // Ensure previous buffers are cleared
      av_packet_unref(pkt);

      // Read packet from file
      int ret = av_read_frame(fmt_ctx, pkt);

      if (ret < 0) {
        // Handle errors that aren't EOF (which simply means the file is finished)
        if (ret != AVERROR_EOF) {
          qWarning() << "Error while finding duration";
        }
        break;
      } else {
        for (int i=0;i<streams_that_need_manual_duration.size();i++) {
          if (streams_that_need_manual_duration.at(i)->index() == pkt->stream_index
              && pkt->pts > durations.at(i)) {
            durations.replace(i, pkt->pts);
          }
        }
      }
    }

    av_packet_free(&pkt);

    if (!cancelled || !*cancelled) {
      for (int i=0;i<streams_that_need_manual_duration.size();i++) {
        streams_that_need_manual_duration.at(i)->set_duration(durations.at(i));
      }
    }

  }

  // Free all memory
  avformat_close_input(&fmt_ctx);

  return result;
}

void FFmpegDecoder::FFmpegError(int error_code)
{
  char err[1024];
  av_strerror(error_code, err, 1024);

  Error(QStringLiteral("Error decoding %1 - %2 %3").arg(stream()->footage()->filename(),
                                                        QString::number(error_code),
                                                        err));
}

void FFmpegDecoder::Error(const QString &s)
{
  qWarning() << s;

  ClearResources();
}

void FFmpegDecoder::Index(const QAtomicInt* cancelled)
{
  QMutexLocker locker(stream()->index_process_lock());

  if (stream()->type() == Stream::kAudio) {

    if (QFileInfo::exists(GetIndexFilename())) {
      WaveInput input(GetIndexFilename());
      if (input.open()) {
        std::static_pointer_cast<AudioStream>(stream())->set_index_done(true);
        std::static_pointer_cast<AudioStream>(stream())->set_index_length(input.params().bytes_to_time(input.data_length()));

        input.close();
      }
    } else {
      UnconditionalAudioIndex(cancelled);
    }

  }
}

QString FFmpegDecoder::GetIndexFilename()
{
  return GetMediaIndexFilename(GetUniqueFileIdentifier(stream()->footage()->filename()))
      .append(QString::number(stream()->index()));
}

void FFmpegDecoder::UnconditionalAudioIndex(const QAtomicInt* cancelled)
{
  // Iterate through each audio frame and extract the PCM data

  QByteArray fn_bytes = stream()->footage()->filename().toUtf8();

  FFmpegDecoderInstance index_instance(fn_bytes.constData(), stream()->index());

  uint64_t channel_layout = index_instance.stream()->codecpar->channel_layout;
  if (!channel_layout) {
    if (!index_instance.stream()->codecpar->channels) {
      // No channel data - we can't do anything with this
      return;
    }

    channel_layout = static_cast<uint64_t>(av_get_default_channel_layout(index_instance.stream()->codecpar->channels));
  }

  AudioStreamPtr audio_stream = std::static_pointer_cast<AudioStream>(stream());

  // This should be unnecessary, but just in case...
  audio_stream->clear_index();

  SwrContext* resampler = nullptr;
  AVSampleFormat src_sample_fmt = static_cast<AVSampleFormat>(index_instance.stream()->codecpar->format);
  AVSampleFormat dst_sample_fmt;

  // We don't use planar types internally, so if this is a planar format convert it now
  if (av_sample_fmt_is_planar(src_sample_fmt)) {
    dst_sample_fmt = av_get_packed_sample_fmt(src_sample_fmt);

    resampler = swr_alloc_set_opts(nullptr,
                                   static_cast<int64_t>(index_instance.stream()->codecpar->channel_layout),
                                   dst_sample_fmt,
                                   index_instance.stream()->codecpar->sample_rate,
                                   static_cast<int64_t>(index_instance.stream()->codecpar->channel_layout),
                                   src_sample_fmt,
                                   index_instance.stream()->codecpar->sample_rate,
                                   0,
                                   nullptr);

    swr_init(resampler);
  } else {
    dst_sample_fmt = src_sample_fmt;
  }

  AudioRenderingParams wave_params(index_instance.stream()->codecpar->sample_rate,
                                   channel_layout,
                                   FFmpegCommon::GetNativeSampleFormat(dst_sample_fmt));
  WaveOutput wave_out(GetIndexFilename(), wave_params);

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int ret;

  if (wave_out.open()) {
    bool success = false;

    while (true) {
      // Check if we have a `cancelled` ptr and its value
      if (cancelled && *cancelled) {
        break;
      }

      ret = index_instance.GetFrame(pkt, frame);

      if (ret < 0) {

        if (ret == AVERROR_EOF) {
          success = true;
        } else {
          char err_str[50];
          av_strerror(ret, err_str, 50);
          qWarning() << "Failed to index:" << ret << err_str;
        }
        break;

      } else {
        char* data;
        int nb_samples;

        if (resampler) {

          nb_samples = swr_get_out_samples(resampler, frame->nb_samples);

          data = new char[wave_params.samples_to_bytes(nb_samples)];

          // We must need to resample this (mainly just convert from planar to packed if necessary)
          nb_samples = swr_convert(resampler,
                                   reinterpret_cast<uint8_t**>(&data),
                                   nb_samples,
                                   const_cast<const uint8_t**>(frame->data),
                                   frame->nb_samples);

          if (nb_samples < 0) {
            char err_str[50];
            av_strerror(nb_samples, err_str, 50);
            qWarning() << "libswresample failed with error:" << nb_samples << err_str;
            break;
          }

        } else {

          // No resampling required, we can write directly from te frame buffer
          data = reinterpret_cast<char*>(frame->data[0]);
          nb_samples = frame->nb_samples;

        }

        // Write packed WAV data to the disk cache
        wave_out.write(data, wave_params.samples_to_bytes(nb_samples));

        audio_stream->set_index_length(wave_params.bytes_to_time(wave_out.data_length()));

        // If we allocated an output for the resampler, delete it here
        if (data != reinterpret_cast<char*>(frame->data[0])) {
          delete [] data;
        }

        SignalIndexProgress(frame->pts);
      }
    }

    wave_out.close();

    if (success) {
      audio_stream->set_index_done(true);
    } else {
      // Audio index didn't complete, delete it
      QFile(GetIndexFilename()).remove();
      audio_stream->clear_index();
    }
  } else {
    qWarning() << "Failed to open WAVE output for indexing";
  }

  if (resampler != nullptr) {
    swr_free(&resampler);
  }

  av_frame_free(&frame);
  av_packet_free(&pkt);
}

int FFmpegDecoderInstance::GetFrame(AVPacket *pkt, AVFrame *frame)
{
  TIME_THIS_FUNCTION;

  bool eof = false;

  int ret;

  // Clear any previous frames
  av_frame_unref(frame);

  while ((ret = avcodec_receive_frame(codec_ctx_, frame)) == AVERROR(EAGAIN) && !eof) {

    // Find next packet in the correct stream index
    do {
      // Free buffer in packet if there is one
      av_packet_unref(pkt);

      // Read packet from file
      ret = av_read_frame(fmt_ctx_, pkt);
    } while (pkt->stream_index != avstream_->index && ret >= 0);

    if (ret == AVERROR_EOF) {
      // Don't break so that receive gets called again, but don't try to read again
      eof = true;

      // Send a null packet to signal end of
      avcodec_send_packet(codec_ctx_, nullptr);
    } else if (ret < 0) {
      // Handle other error by breaking loop and returning the code we received
      break;
    } else {
      // Successful read, send the packet
      ret = avcodec_send_packet(codec_ctx_, pkt);

      // We don't need the packet anymore, so free it
      av_packet_unref(pkt);

      if (ret < 0) {
        break;
      }
    }
  }

  return ret;
}

QMutex *FFmpegDecoderInstance::cache_lock()
{
  return &cache_lock_;
}

QWaitCondition *FFmpegDecoderInstance::cache_wait_cond()
{
  return &cache_wait_cond_;
}

bool FFmpegDecoderInstance::IsWorking() const
{
  return is_working_;
}

void FFmpegDecoderInstance::SetWorking(bool working)
{
  is_working_ = working;
}

void FFmpegDecoderInstance::Seek(int64_t timestamp)
{
  TIME_THIS_FUNCTION;

  avcodec_flush_buffers(codec_ctx_);
  av_seek_frame(fmt_ctx_, avstream_->index, timestamp, AVSEEK_FLAG_BACKWARD);
}

void FFmpegDecoder::CacheFrameToDisk(AVFrame *f)
{
  QFile save_frame(GetIndexFilename().append(QString::number(f->pts)));
  if (save_frame.open(QFile::WriteOnly)) {

    // Save frame to media index
    int cached_buffer_sz = av_image_get_buffer_size(static_cast<AVPixelFormat>(f->format),
                                                    f->width,
                                                    f->height,
                                                    1);

    QByteArray cached_frame(cached_buffer_sz, Qt::Uninitialized);

    av_image_copy_to_buffer(reinterpret_cast<uint8_t*>(cached_frame.data()),
                            cached_frame.size(),
                            f->data,
                            f->linesize,
                            static_cast<AVPixelFormat>(f->format),
                            f->width,
                            f->height,
                            1);

    save_frame.write(qCompress(cached_frame, 1));
    save_frame.close();

    DiskManager::instance()->CreatedFile(save_frame.fileName(), QByteArray());
  }

  // See if we stored this frame in the disk cache
  /*
  QByteArray frame_loader;
  if (!got_frame) {
    QFile compressed_frame(GetIndexFilename().append(QString::number(target_ts)));
    if (compressed_frame.exists()
        && compressed_frame.size() > 0
        && compressed_frame.open(QFile::ReadOnly)) {
      DiskManager::instance()->Accessed(compressed_frame.fileName());

      // Read data
      frame_loader = qUncompress(compressed_frame.readAll());

      av_image_fill_arrays(input_data,
                           input_linesize,
                           reinterpret_cast<uint8_t*>(frame_loader.data()),
                           static_cast<AVPixelFormat>(avstream_->codecpar->format),
                           avstream_->codecpar->width,
                           avstream_->codecpar->height,
                           1);

      got_frame = true;
    }
  }
  */
}

/*void FFmpegDecoder::RemoveFirstFromFrameCache()
{
  if (cached_frames_.isEmpty()) {
    return;
  }

  AVFrame* first = cached_frames_.takeFirst();
  av_frame_free(&first);
  cache_at_zero_ = false;
}

void FFmpegDecoder::RemoveLastFromFrameCache()
{
  if (cached_frames_.isEmpty()) {
    return;
  }

  AVFrame* last = cached_frames_.takeLast();
  av_frame_free(&last);
  cache_at_eof_ = false;
}*/

void FFmpegDecoderInstance::ClearFrameCache()
{
  cached_frames_.clear();
  cache_at_eof_ = false;
  cache_at_zero_ = false;
}

AVFrame *FFmpegDecoderInstance::RetrieveFrame(const int64_t& target_ts, bool cache_is_locked)
{
  if (!cache_is_locked) {
    cache_lock_.lock();
  }

  int64_t seek_ts = target_ts;
  bool still_seeking = false;

  cache_target_time_ = target_ts;

  // If the frame wasn't in the frame cache, see if this frame cache is too old to use
  if (cached_frames_.isEmpty()
      || target_ts < cached_frames_.first()->pts
      || target_ts > cached_frames_.last()->pts + 2*second_ts_) {
    ClearFrameCache();

    Seek(seek_ts);
    if (seek_ts == 0) {
      cache_at_zero_ = true;
    }

    still_seeking = true;
  }

  int ret;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* return_frame = nullptr;

  while (true) {
    // Allocate a new frame
    AVFrame* working_frame = av_frame_alloc();

    // Pull from the decoder
    ret = GetFrame(pkt, working_frame);

    // Handle any errors that aren't EOF (EOF is handled later on)
    if (ret < 0 && ret != AVERROR_EOF) {
      av_frame_free(&working_frame);
      cache_lock_.unlock();
      qCritical() << "Failed to retrieve frame:" << ret;
      break;
    }

    if (still_seeking) {
      // Handle a failure to seek (occurs on some media)
      // We'll only be here if the frame cache was emptied earlier
      if (!cache_at_zero_ && (ret == AVERROR_EOF || working_frame->pts > target_ts)) {

        seek_ts = qMax(static_cast<int64_t>(0), seek_ts - second_ts_);
        Seek(seek_ts);
        if (seek_ts == 0) {
          cache_at_zero_ = true;
        }
        av_frame_free(&working_frame);
        continue;

      } else {

        still_seeking = false;

      }
    }

    if (cache_is_locked) {
      cache_is_locked = false;
    } else {
      cache_lock_.lock();
    }

    if (ret == AVERROR_EOF) {

      // Handle an "expected" EOF by using the last frame of our cache
      cache_at_eof_ = true;

      cache_wait_cond_.wakeAll();
      cache_lock_.unlock();

      return_frame = cached_frames_.last();
      av_frame_free(&working_frame);
      break;

    } else {

      // Whatever it is, keep this frame in memory for the time being just in case
      cached_frames_.append(working_frame);

      cache_wait_cond_.wakeAll();
      cache_lock_.unlock();

      // If this is a valid frame, see if this or the frame before it are the one we need
      if (working_frame->pts == target_ts) {
        return_frame = working_frame;
        break;
      } else if (working_frame->pts > target_ts) {
        if (cached_frames_.isEmpty() && cache_at_zero_) {
          return_frame = working_frame;
          break;
        } else {
          return_frame = cached_frames_.at(cached_frames_.size() - 2);
          break;
        }
      }
    }
  }

  av_packet_free(&pkt);

  return return_frame;
}

void FFmpegDecoder::ClearResources()
{
  FreeScaler();

  open_ = false;
}

void FFmpegDecoder::SetupScaler(const int &divider)
{
  VideoStream* vs = static_cast<VideoStream*>(stream().get());

  scale_ctx_ = sws_getContext(vs->width(),
                              vs->height(),
                              src_pix_fmt_,
                              vs->width() / divider,
                              vs->height() / divider,
                              ideal_pix_fmt_,
                              SWS_FAST_BILINEAR,
                              nullptr,
                              nullptr,
                              nullptr);

  if (!scale_ctx_) {
    Error(QStringLiteral("Failed to allocate SwsContext"));
  } else {
    scale_divider_ = divider;
  }
}

void FFmpegDecoder::FreeScaler()
{
  if (scale_ctx_) {
    sws_freeContext(scale_ctx_);
    scale_ctx_ = nullptr;
    scale_divider_ = -1;
  }
}

int64_t FFmpegDecoderInstance::RangeStart() const
{
  if (cached_frames_.isEmpty()) {
    return AV_NOPTS_VALUE;
  }
  return cached_frames_.first()->pts;
}

int64_t FFmpegDecoderInstance::RangeEnd() const
{
  if (cached_frames_.isEmpty()) {
    return AV_NOPTS_VALUE;
  }
  return cached_frames_.last()->pts;
}

bool FFmpegDecoderInstance::CacheContainsTime(const int64_t &t) const
{
  return !cached_frames_.isEmpty()
      && ((RangeStart() <= t && RangeEnd() >= t)
          || (cache_at_zero_ && t < cached_frames_.first()->pts)
          || (cache_at_eof_ && t > cached_frames_.last()->pts));
}

bool FFmpegDecoderInstance::CacheWillContainTime(const int64_t &t) const
{
  return !cached_frames_.isEmpty() && t >= cached_frames_.first()->pts && t <= cache_target_time_;
}

bool FFmpegDecoderInstance::CacheCouldContainTime(const int64_t &t) const
{
  return !cached_frames_.isEmpty() && t >= cached_frames_.first()->pts && t <= (cache_target_time_ + 2*second_ts_);
}

bool FFmpegDecoderInstance::CacheIsEmpty() const
{
  return cached_frames_.isEmpty();
}

AVFrame *FFmpegDecoderInstance::GetFrameFromCache(const int64_t &t) const
{
  if (t < cached_frames_.first()->pts) {

    if (cache_at_zero_) {
      return cached_frames_.first();
    }

  } else if (t > cached_frames_.last()->pts) {

    if (cache_at_eof_) {
      return cached_frames_.last();
    }

  } else {

    // We already have this frame in the cache, find it
    for (int i=0;i<cached_frames_.size();i++) {
      AVFrame* this_frame = cached_frames_.at(i);

      if (this_frame->pts == t // Test for an exact match
          || (i < cached_frames_.size() - 1 && cached_frames_.at(i+1)->pts > t)) { // Or for this frame to be the "closest"

        return this_frame;

      }
    }
  }

  return nullptr;
}

rational FFmpegDecoderInstance::sample_aspect_ratio() const
{
  return av_guess_sample_aspect_ratio(fmt_ctx_, avstream_, nullptr);
}

AVStream *FFmpegDecoderInstance::stream() const
{
  return avstream_;
}

FFmpegDecoderInstance::FFmpegDecoderInstance(const char *filename, int stream_index) :
  fmt_ctx_(nullptr),
  opts_(nullptr),
  is_working_(false),
  cache_at_zero_(false),
  cache_at_eof_(false)
{
  // Open file in a format context
  int error_code = avformat_open_input(&fmt_ctx_, filename, nullptr, nullptr);

  // Handle format context error
  if (error_code != 0) {
    qDebug() << "Failed to open input:" << filename << error_code;
    ClearResources();
    return;
  }

  // Get stream information from format
  error_code = avformat_find_stream_info(fmt_ctx_, nullptr);

  // Handle get stream information error
  if (error_code < 0) {
    qDebug() << "Failed to find stream info:" << error_code;
    ClearResources();
    return;
  }

  // Get reference to correct AVStream
  avstream_ = fmt_ctx_->streams[stream_index];

  // Find decoder
  AVCodec* codec = avcodec_find_decoder(avstream_->codecpar->codec_id);

  // Handle failure to find decoder
  if (codec == nullptr) {
    qCritical() << "Failed to find appropriate decoder for this codec:" << filename << stream_index << avstream_->codecpar->codec_id;
    ClearResources();
    return;
  }

  // Allocate context for the decoder
  codec_ctx_ = avcodec_alloc_context3(codec);
  if (codec_ctx_ == nullptr) {
    qCritical() << "Failed to allocate codec context";
    ClearResources();
    return;
  }

  // Copy parameters from the AVStream to the AVCodecContext
  error_code = avcodec_parameters_to_context(codec_ctx_, avstream_->codecpar);

  // Handle failure to copy parameters
  if (error_code < 0) {
    qCritical() << "Failed to copy parameters from AVStream to AVCodecContext";
    ClearResources();
    return;
  }

  // Set multithreading setting
  error_code = av_dict_set(&opts_, "threads", "auto", 0);

  // Handle failure to set multithreaded decoding
  if (error_code < 0) {
    qCritical() << "Failed to set codec options, performance may suffer";
  }

  // Open codec
  error_code = avcodec_open2(codec_ctx_, codec, &opts_);
  if (error_code < 0) {
    qDebug() << "Failed to open codec" << codec->id << error_code;
    ClearResources();
    return;
  }

  // Store one second in the source's timebase
  second_ts_ = qRound64(av_q2d(av_inv_q(avstream_->time_base)));
}

FFmpegDecoderInstance::~FFmpegDecoderInstance()
{
  ClearResources();
}

bool FFmpegDecoderInstance::IsValid() const
{
  return codec_ctx_;
}

void FFmpegDecoderInstance::ClearResources()
{
  ClearFrameCache();

  if (opts_) {
    av_dict_free(&opts_);
    opts_ = nullptr;
  }

  if (codec_ctx_) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
  }

  if (fmt_ctx_) {
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = nullptr;
  }
}

/*void FFmpegDecoder::ClearTimerEvent()
{
  QMutexLocker locker(&mutex_);

  cache_at_zero_ = false;
  cached_frames_.remove_old_frames(QDateTime::currentMSecsSinceEpoch() - clear_timer_.interval());
}*/

OLIVE_NAMESPACE_EXIT
