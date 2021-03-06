#include "previewgenerator.h"

#include "media.h"
#include "panels/viewer.h"
#include "io/config.h"

#include <QPainter>
#include <QPixmap>
#include <QDebug>
#include <QtMath>
#include <QTreeWidgetItem>

#define WAVEFORM_RESOLUTION 64

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

PreviewGenerator::PreviewGenerator(QTreeWidgetItem* i, Media* m, bool r) :
	QThread(0),
	fmt_ctx(NULL),
	item(i),
	media(m),
	retrieve_duration(false),
	contains_still_image(false),
	replace(r)
{
    connect(this, SIGNAL(finished()), this, SLOT(deleteLater()));
}

void PreviewGenerator::parse_media() {
    // detect video/audio streams in file
    for (int i=0;i<(int)fmt_ctx->nb_streams;i++) {
        // Find the decoder for the video stream
        if (avcodec_find_decoder(fmt_ctx->streams[i]->codecpar->codec_id) == NULL) {
            qDebug() << "[ERROR] Unsupported codec in stream %d.\n";
        } else {
            MediaStream* ms = media->get_stream_from_file_index(fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO, i);
            bool append = false;
            if (ms == NULL) {
                ms = new MediaStream();
                ms->preview_done = false;
                ms->file_index = i;
                append = true;
            }
			if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
					&& fmt_ctx->streams[i]->codecpar->width > 0
					&& fmt_ctx->streams[i]->codecpar->height > 0) {
				if (fmt_ctx->streams[i]->avg_frame_rate.den == 0) { // source is LIKELY a still image
					ms->infinite_length = true;
					contains_still_image = true;
					ms->video_frame_rate = 0;
				} else {
					ms->infinite_length = false;
					ms->video_frame_rate = av_q2d(fmt_ctx->streams[i]->avg_frame_rate);
				}
				ms->video_width = fmt_ctx->streams[i]->codecpar->width;
				ms->video_height = fmt_ctx->streams[i]->codecpar->height;
				if (append) media->video_tracks.append(ms);
            } else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ms->audio_channels = fmt_ctx->streams[i]->codecpar->channels;
                ms->audio_layout = fmt_ctx->streams[i]->codecpar->channel_layout;
                ms->audio_frequency = fmt_ctx->streams[i]->codecpar->sample_rate;
                if (append) media->audio_tracks.append(ms);
			} else if (append) {
				delete ms;
			}
        }
    }
    media->length = fmt_ctx->duration;

	if (fmt_ctx->duration == INT64_MIN) {
		retrieve_duration = true;
	} else {
		finalize_media();
	}
}

void PreviewGenerator::finalize_media() {
	media->ready = true;

	if (media->video_tracks.size() == 0) {
		emit set_icon(ICON_TYPE_AUDIO, replace);
	} else {
		emit set_icon(ICON_TYPE_VIDEO, replace);
	}

	if (!contains_still_image || media->audio_tracks.size() > 0) {
		double frame_rate = 30;
		if (!contains_still_image && media->video_tracks.size() > 0) frame_rate = media->video_tracks.at(0)->video_frame_rate;
        item->setText(1, frame_to_timecode(media->get_length_in_frames(frame_rate), config.timecode_view, frame_rate));
	}
}

void PreviewGenerator::generate_waveform() {
    SwsContext* sws_ctx;
    SwrContext* swr_ctx;
    AVFrame* temp_frame = av_frame_alloc();
    AVCodecContext** codec_ctx = new AVCodecContext* [fmt_ctx->nb_streams];
	int64_t* media_lengths = new int64_t[fmt_ctx->nb_streams]{0};
    for (unsigned int i=0;i<fmt_ctx->nb_streams;i++) {
        codec_ctx[i] = NULL;
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            AVCodec* codec = avcodec_find_decoder(fmt_ctx->streams[i]->codecpar->codec_id);
            codec_ctx[i] = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(codec_ctx[i], fmt_ctx->streams[i]->codecpar);
            avcodec_open2(codec_ctx[i], codec, NULL);

            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && codec_ctx[i]->channel_layout == 0) {
				codec_ctx[i]->channel_layout = av_get_default_channel_layout(fmt_ctx->streams[i]->codecpar->channels);
            }
        }
    }
    AVPacket packet;
    bool done = true;

    bool end_of_file = false;

    // get the ball rolling
    av_read_frame(fmt_ctx, &packet);
    avcodec_send_packet(codec_ctx[packet.stream_index], &packet);

	while (!end_of_file) {
		while (codec_ctx[packet.stream_index] == NULL || avcodec_receive_frame(codec_ctx[packet.stream_index], temp_frame) == AVERROR(EAGAIN)) {
            av_packet_unref(&packet);
            int read_ret = av_read_frame(fmt_ctx, &packet);
            if (read_ret < 0) {
                end_of_file = true;
                if (read_ret != AVERROR_EOF) qDebug() << "[ERROR] Failed to read packet for preview generation" << read_ret;
                break;
            }
            if (codec_ctx[packet.stream_index] != NULL) {
                int send_ret = avcodec_send_packet(codec_ctx[packet.stream_index], &packet);
                if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
                    qDebug() << "[ERROR] Failed to send packet for preview generation - aborting" << send_ret;
                    end_of_file = true;
                    break;
                }
            }
		}
        if (!end_of_file) {
            MediaStream* s = media->get_stream_from_file_index(fmt_ctx->streams[packet.stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO, packet.stream_index);
			if (s != NULL) {
				if (fmt_ctx->streams[packet.stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
					if (!s->preview_done) {
						int dstH = 120;
						int dstW = dstH * ((float)temp_frame->width/(float)temp_frame->height);
						uint8_t* data = new uint8_t[dstW*dstH*3];

						sws_ctx = sws_getContext(
								temp_frame->width,
								temp_frame->height,
								static_cast<AVPixelFormat>(temp_frame->format),
								dstW,
								dstH,
								static_cast<AVPixelFormat>(AV_PIX_FMT_RGB24),
								SWS_FAST_BILINEAR,
								NULL,
								NULL,
								NULL
							);

						int linesize[AV_NUM_DATA_POINTERS];
						linesize[0] = dstW*3;
						sws_scale(sws_ctx, temp_frame->data, temp_frame->linesize, 0, temp_frame->height, &data, linesize);

						s->video_preview = QImage(data, dstW, dstH, linesize[0], QImage::Format_RGB888);

		//                  delete [] data;

						s->preview_done = true;

						sws_freeContext(sws_ctx);

						if (!retrieve_duration) {
							avcodec_close(codec_ctx[packet.stream_index]);
							codec_ctx[packet.stream_index] = NULL;
						}
					}
					media_lengths[packet.stream_index]++;
				} else if (fmt_ctx->streams[packet.stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
					int interval = qFloor((temp_frame->sample_rate/WAVEFORM_RESOLUTION)/4)*4;

					AVFrame* swr_frame = av_frame_alloc();
					swr_frame->channel_layout = temp_frame->channel_layout;
					swr_frame->sample_rate = temp_frame->sample_rate;
					swr_frame->format = AV_SAMPLE_FMT_S16P;

					swr_ctx = swr_alloc_set_opts(
								NULL,
								temp_frame->channel_layout,
								static_cast<AVSampleFormat>(swr_frame->format),
								temp_frame->sample_rate,
								temp_frame->channel_layout,
								static_cast<AVSampleFormat>(temp_frame->format),
								temp_frame->sample_rate,
								0,
								NULL
							);

					swr_init(swr_ctx);

					swr_convert_frame(swr_ctx, swr_frame, temp_frame);

					// TODO implement a way to terminate this if the user suddenly closes the project while the waveform is being generated
					int sample_size = av_get_bytes_per_sample(static_cast<AVSampleFormat>(swr_frame->format));
					int nb_bytes = swr_frame->nb_samples * sample_size;
					int byte_interval = interval * sample_size;
					for (int i=0;i<nb_bytes;i+=byte_interval) {
						for (int j=0;j<swr_frame->channels;j++) {
							qint16 min = 0;
							qint16 max = 0;
							for (int k=0;k<byte_interval;k+=sample_size) {
								if (i+k < nb_bytes) {
									qint16 sample = ((swr_frame->data[j][i+k+1] << 8) | swr_frame->data[j][i+k]);
									if (sample > max) {
										max = sample;
									} else if (sample < min) {
										min = sample;
									}
								} else {
									break;
								}
							}
							s->audio_preview.append(min >> 8);
							s->audio_preview.append(max >> 8);
						}
					}

					swr_free(&swr_ctx);
					av_frame_unref(swr_frame);
					av_frame_free(&swr_frame);
				}
			}

			// check if we've got all our previews
			if (retrieve_duration) {
				done = false;
			} else if (media->audio_tracks.size() == 0) {
				done = true;
				for (int i=0;i<media->video_tracks.size();i++) {
					if (!media->video_tracks.at(i)->preview_done) {
						done = false;
						break;
					}
				}
				if (done) {
					end_of_file = true;
					break;
				}
			}
            av_packet_unref(&packet);
        }
    }
    for (int i=0;i<media->audio_tracks.size();i++) {
        media->audio_tracks.at(i)->preview_done = true;
    }
    av_frame_free(&temp_frame);
    for (unsigned int i=0;i<fmt_ctx->nb_streams;i++) {
        if (codec_ctx[i] != NULL) {
            avcodec_close(codec_ctx[i]);
        }
	}
	if (retrieve_duration) {
		media->length = 0;
		int maximum_stream = 0;
		for (unsigned int i=0;i<fmt_ctx->nb_streams;i++) {
			if (media_lengths[i] > media_lengths[maximum_stream]) {
				maximum_stream = i;
			}
		}
		media->length = (double) media_lengths[maximum_stream] / av_q2d(fmt_ctx->streams[maximum_stream]->avg_frame_rate) * AV_TIME_BASE;
		finalize_media();
	}
	delete [] media_lengths;
    delete [] codec_ctx;
}

void PreviewGenerator::run() {
    Q_ASSERT(media != NULL);
    Q_ASSERT(item != NULL);

    QByteArray ba = media->url.toLatin1();
    char* filename = new char[ba.size()+1];
    strcpy(filename, ba.data());

    bool error = false;
    int errCode = avformat_open_input(&fmt_ctx, filename, NULL, NULL);
    if(errCode != 0) {
        char err[1024];
        av_strerror(errCode, err, 1024);
        item->setToolTip(0, "Could not open file - " + QString(err));
        error = true;
    } else {
        errCode = avformat_find_stream_info(fmt_ctx, NULL);
        if (errCode < 0) {
            char err[1024];
            av_strerror(errCode, err, 1024);
            item->setToolTip(0, "Could not find stream information - " + QString(err));
            error = true;
        } else {
            av_dump_format(fmt_ctx, 0, filename, 0);
            parse_media();
            generate_waveform();
        }
        avformat_close_input(&fmt_ctx);
    }
    if (error) {
		emit set_icon(ICON_TYPE_ERROR, replace);
    } else {
        item->setToolTip(0, QString());
    }
    delete [] filename;
}
